#include "FprimeTransport.hpp"
#include "FprimeDictionary.hpp"
#include <dartwic/EngineProtocol.h>
#include <tempest/Peer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static constexpr Socket INVALID_SOCKET_HANDLE = INVALID_SOCKET;
static void closeSocket(Socket socket) { if (socket != INVALID_SOCKET_HANDLE) closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static constexpr Socket INVALID_SOCKET_HANDLE = -1;
static void closeSocket(Socket socket) { if (socket != INVALID_SOCKET_HANDLE) close(socket); }
#endif

namespace FPrimeBridge {
namespace {
using namespace std::chrono_literals;

uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
uint64_t steadyNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint16_t read16(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }
uint32_t read32(const uint8_t* p) { return (static_cast<uint32_t>(read16(p)) << 16) | read16(p + 2); }
void put16(std::vector<uint8_t>& out, uint16_t n) { out.push_back(static_cast<uint8_t>(n >> 8)); out.push_back(static_cast<uint8_t>(n)); }
void put32(std::vector<uint8_t>& out, uint32_t n) { put16(out, static_cast<uint16_t>(n >> 16)); put16(out, static_cast<uint16_t>(n)); }
uint16_t crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}
bool sendFully(Socket socket, const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const int count = ::send(socket, reinterpret_cast<const char*>(bytes.data() + offset),
                                 static_cast<int>(bytes.size() - offset), 0);
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}
bool boolean(const TEMPEST::Value& value) {
    if (const auto* result = std::get_if<bool>(&value.storage())) return *result;
    throw TEMPEST::RemoteError("running must be a boolean", "invalid_arguments");
}
std::string eventType(std::string_view severity) {
    if (severity == "FATAL") return "error";
    if (severity == "WARNING_HI" || severity == "WARNING_LO") return "warning";
    return "message";
}
std::string valueText(const TEMPEST::Value& value, unsigned depth = 0) {
    if (depth > 4) return "...";
    return std::visit([depth](const auto& item) -> std::string {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::monostate>) return "null";
        else if constexpr (std::is_same_v<Item, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<Item, std::string>) return item;
        else if constexpr (std::is_same_v<Item, TEMPEST::Value::Array>) {
            std::string text = "[";
            for (const auto& value : item) {
                if (text.size() > 1) text += ", ";
                text += valueText(value, depth + 1);
            }
            return text + "]";
        } else if constexpr (std::is_same_v<Item, TEMPEST::Value::Object>) {
            std::string text = "{";
            for (const auto& [key, value] : item) {
                if (text.size() > 1) text += ", ";
                text += key + "=" + valueText(value, depth + 1);
            }
            return text + "}";
        } else return std::to_string(item);
    }, value.storage());
}
std::string eventLogText(const DecodedEvent& event) {
    std::string text = event.severity.empty() ? "" : "[" + event.severity + "] ";
    text += event.name;
    if (!event.arguments.empty()) {
        text += " | ";
        bool first = true;
        for (const auto& [name, value] : event.arguments) {
            if (!first) text += ", ";
            text += name + "=" + valueText(value);
            first = false;
        }
    } else if (!event.format.empty()) text += " | " + event.format;
    return text + "\n";
}
}

struct FprimeTransport::Impl {
    std::string bind_host;
    std::string node;
    std::string command_name;
    uint16_t port;
    uint16_t spacecraft_id;
    uint8_t vcid;
    size_t tm_size;
    Dictionary dictionary;
    TEMPEST::TransportCallbacks callbacks;
    std::mutex mutex;
    std::deque<TEMPEST::Message> outbound;
    std::unordered_map<std::string, DARTWIC::ChannelSnapshot> channels;
    std::atomic<bool> running{false};
    std::thread worker;
    std::thread decoder;
    Socket listener = INVALID_SOCKET_HANDLE;
    std::atomic<Socket> client{INVALID_SOCKET_HANDLE};
    std::vector<uint8_t> incoming;
    struct InboundFrame { std::vector<uint8_t> bytes; uint64_t epoch; std::string source_session; };
    std::mutex frame_mutex;
    std::condition_variable frame_ready;
    std::deque<InboundFrame> frames;
    std::atomic<uint64_t> link_epoch{0};
    std::atomic<uint64_t> received_bytes{0}, sent_bytes{0}, dropped_frames{0};
    std::atomic<uint64_t> last_receive_ms{0}, last_send_ms{0}, last_valid_frame_steady_ns{0};
    uint64_t downlink_idle_timeout_ns;
    uint16_t tc_sequence = 0;
    uint64_t revision = 1;
    uint64_t event_sequence = 1;
    uint64_t session_sequence = 0;
    std::string session;
#ifdef _WIN32
    bool winsock_ready = false;
#endif

    explicit Impl(const nlohmann::json& config)
        : bind_host(config.value("bind_host", std::string{"127.0.0.1"})),
          node(config.value("node_name", std::string{"HADRON_SITL"})),
          port(static_cast<uint16_t>(config.value("port", 50101))),
          spacecraft_id(static_cast<uint16_t>(config.value("spacecraft_id", 68))),
          vcid(static_cast<uint8_t>(config.value("virtual_channel_id", 1))),
          tm_size(static_cast<size_t>(config.value("tm_frame_size", 1024))),
          downlink_idle_timeout_ns(static_cast<uint64_t>(config.value("downlink_idle_timeout_ms", 10000)) * 1'000'000ULL) {
        if (node.empty() || port == 0 || spacecraft_id > 1023 || vcid > 7 || tm_size < 16 || tm_size > 65536)
            throw std::invalid_argument("Invalid F Prime bridge configuration");
        std::string error;
        if (!dictionary.load(config.at("dictionary").get<std::string>(), error))
            throw std::invalid_argument(error);
        // The demo command is optional; real F Prime deployments expose their
        // own dictionary commands through fprime/command.
        try { command_name = dictionary.commandWithSuffix(".SET_RUNNING"); }
        catch (const std::invalid_argument&) { command_name.clear(); }
    }

    void respond(const TEMPEST::Message& request, TEMPEST::Value::Object value,
                 bool error = false, std::string code = {}) {
        if (callbacks.on_message) callbacks.on_message({TEMPEST::Message::Kind::Response,
            request.request_id, request.name, std::move(value), error, std::move(code)});
    }
    void fail(const TEMPEST::Message& request, std::string reason, std::string code) {
        respond(request, {{"error", std::move(reason)}}, true, std::move(code));
    }
    void state(TEMPEST::ConnectionState state_value, std::string reason = {}) {
        if (callbacks.on_state) callbacks.on_state(state_value, std::move(reason));
    }

    void publishChannel(uint32_t id, const DecodedChannel& decoded, uint64_t flight_time,
                        const std::string& source_session, uint64_t epoch) {
        if (!decoded.decoded || !decoded.numeric || epoch != link_epoch.load()) return;
        const uint64_t received = nowNs();
        DARTWIC::ChannelTelemetry telemetry;
        telemetry.upsert.owner_node = node;
        telemetry.upsert.channel = decoded.name;
        telemetry.upsert.value = decoded.value;
        telemetry.upsert.timestamp = received;
        telemetry.upsert.channel_data = {{"value", decoded.value}, {"timestamp", received},
            {"control_policy", "observe_only"}, {"data_frame", "fprime.ccsds"},
            {"flight_time_us", flight_time}};
        telemetry.upsert.revision = revision++;
        telemetry.upsert.source_session_id = source_session;
        {
            std::lock_guard lock(mutex);
            if (epoch != link_epoch.load()) return;
            channels[decoded.name] = {node, decoded.name, telemetry.upsert.channel_data,
                                      telemetry.upsert.revision, source_session};
        }
        if (callbacks.on_message) {
            auto name = DARTWIC::EngineContract::topic(telemetry);
            auto payload = DARTWIC::EngineContract::payload(telemetry);
            callbacks.on_message({TEMPEST::Message::Kind::Telemetry, {}, std::move(name), std::move(payload)});
        }
    }

    void parsePacket(uint16_t apid, const uint8_t* payload, size_t size,
                     const std::string& source_session, uint64_t epoch) {
        if (size < 2 || read16(payload) != apid) return;
        payload += 2; size -= 2;
        if (apid == 1) {
            while (size >= 15) {
                const uint32_t id = read32(payload);
                size_t value_size = 0;
                const auto decoded = dictionary.decodeChannelPrefix(id, payload + 15, size - 15, value_size);
                if (!decoded.decoded || value_size == 0) break;
                const uint64_t flight_time = static_cast<uint64_t>(read32(payload + 7)) * 1'000'000ULL
                    + static_cast<uint64_t>(read32(payload + 11));
                publishChannel(id, decoded, flight_time, source_session, epoch);
                payload += 15 + value_size;
                size -= 15 + value_size;
            }
        } else if (apid == 2 && size >= 15) {
            const auto event = dictionary.decodeEvent(read32(payload), payload + 15, size - 15);
            if (!event.decoded) return;
            const auto sequence = event_sequence++;
            const auto received = nowNs();
            DARTWIC::ArgusEventTelemetry telemetry;
            telemetry.owner_node = node;
            telemetry.event.owner_node = node;
            telemetry.event.event_id = node + ":" + source_session + ":" + std::to_string(sequence);
            telemetry.event.title = event.name;
            telemetry.event.description = event.format;
            telemetry.event.type = eventType(event.severity);
            telemetry.event.timestamp = received;
            telemetry.event.details = event.arguments;
            telemetry.event.details["fprime_event_id"] = static_cast<uint64_t>(read32(payload));
            telemetry.event.details["fprime_severity"] = event.severity;
            if (epoch == link_epoch.load() && callbacks.on_message) {
                callbacks.on_message({TEMPEST::Message::Kind::Telemetry, {},
                    DARTWIC::EngineContract::topic(telemetry), DARTWIC::EngineContract::payload(telemetry)});
                callbacks.on_message({TEMPEST::Message::Kind::Telemetry, {}, "tempest-peer/argus/logs", {
                    {"owner_node", node}, {"session", source_session}, {"stream", "F Prime Events"},
                    {"channel", "ccsds"}, {"level", telemetry.event.type == "message" ? "info" : telemetry.event.type},
                    {"text", eventLogText(event)},
                    {"sequence", std::to_string(sequence)}, {"timestamp_ns", std::to_string(received)}}});
            }
        }
    }

    void parseIncoming() {
        while (incoming.size() >= tm_size) {
            const uint16_t first = read16(incoming.data());
            if (((first >> 4) & 0x03FF) != spacecraft_id || ((first >> 1) & 0x07) != vcid
                || crc16(incoming.data(), tm_size - 2) != read16(incoming.data() + tm_size - 2)) {
                incoming.erase(incoming.begin());
                continue;
            }
            last_valid_frame_steady_ns.store(steadyNs(), std::memory_order_relaxed);
            {
                std::lock_guard lock(frame_mutex);
                if (frames.size() >= 256) {
                    frames.pop_front();
                    dropped_frames.fetch_add(1, std::memory_order_relaxed);
                }
                frames.push_back({std::vector<uint8_t>(incoming.begin(), incoming.begin() + tm_size),
                    link_epoch.load(), session});
            }
            frame_ready.notify_one();
            incoming.erase(incoming.begin(), incoming.begin() + tm_size);
        }
        if (incoming.size() > tm_size * 2) incoming.erase(incoming.begin(), incoming.end() - tm_size);
    }

    void decodeFrames() {
        while (true) {
            InboundFrame frame;
            {
                std::unique_lock lock(frame_mutex);
                frame_ready.wait(lock, [this] { return !running || !frames.empty(); });
                if (!running) return;
                frame = std::move(frames.front());
                frames.pop_front();
            }
            if (frame.epoch != link_epoch.load()) continue;
            try {
                size_t pos = 6;
                const size_t end = frame.bytes.size() - 2;
                while (pos + 6 <= end && frame.epoch == link_epoch.load()) {
                    const uint16_t id = read16(frame.bytes.data() + pos);
                    const uint16_t apid = id & 0x07FF;
                    const size_t bytes = static_cast<size_t>(read16(frame.bytes.data() + pos + 4)) + 7;
                    if (bytes < 7 || pos + bytes > end || apid == 0x07FF) break;
                    if ((id & 0xF800) == 0) parsePacket(apid, frame.bytes.data() + pos + 6,
                        bytes - 6, frame.source_session, frame.epoch);
                    pos += bytes;
                }
            } catch (...) {
                dropped_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::vector<uint8_t> commandFrame(const std::string& command, const TEMPEST::Value::Object& arguments,
                                       uint64_t& opcode) {
        const auto encoded = dictionary.encodeCommand(command, arguments, 900);
        opcode = encoded.opcode;
        if (opcode > 0xFFFFFFFFULL) throw std::invalid_argument("F Prime opcode exceeds 32 bits");
        std::vector<uint8_t> data;
        put16(data, 0); // F Prime command packet descriptor
        put32(data, static_cast<uint32_t>(opcode));
        data.insert(data.end(), encoded.arguments.begin(), encoded.arguments.end());
        std::vector<uint8_t> packet;
        put16(packet, 0x1000); // CCSDS TC space packet, APID 0
        put16(packet, static_cast<uint16_t>(0xC000 | (tc_sequence++ & 0x3FFF)));
        put16(packet, static_cast<uint16_t>(data.size() - 1));
        packet.insert(packet.end(), data.begin(), data.end());
        const size_t frame_size = packet.size() + 7;
        if (frame_size > 1024) throw std::invalid_argument("F Prime command exceeds TC frame size");
        std::vector<uint8_t> frame;
        put16(frame, static_cast<uint16_t>(0x2000 | spacecraft_id));
        put16(frame, static_cast<uint16_t>((vcid << 10) | (frame_size - 1)));
        frame.push_back(0); // bypass FARM
        frame.insert(frame.end(), packet.begin(), packet.end());
        put16(frame, crc16(frame.data(), frame.size()));
        return frame;
    }

    void handle(TEMPEST::Message message) {
        if (message.kind != TEMPEST::Message::Kind::Request) return;
        if (message.name == "tempest/register-peer") {
            respond(message, {{"version", 2}, {"node_name", node}, {"session_id", session},
                {"peer_id", "fprime_bridge.fprime_ccsds"}, {"peer_version", 1},
                {"protocol_id", "tempest.engine"}, {"protocol_version", 1}});
            return;
        }
        if (message.name == "tempest/heartbeat") { respond(message, {}); return; }
        if (message.name == "tempest/operations/list") {
            TEMPEST::Value::Array operations;
            if (!command_name.empty()) operations.push_back(TEMPEST::operationDescriptorValue({
                "demo/set-running", "Set mock simulation running", "Starts or pauses mock telemetry.",
                "F Prime", {{"running", "boolean", "Whether to run the mock launch.", true}}}));
            operations.push_back(TEMPEST::operationDescriptorValue({
                "fprime/command", "Send F Prime command", "Encodes a command from the loaded F Prime dictionary.",
                "F Prime", {{"name", "string", "Qualified dictionary command name.", true},
                            {"arguments", "object", "Dictionary command arguments.", true}}}));
            for (const auto& command : dictionary.commands()) {
                TEMPEST::OperationDescriptor descriptor;
                descriptor.name = command.name;
                descriptor.display_name = command.name;
                descriptor.description = command.description;
                descriptor.category = "F Prime";
                for (const auto& argument : command.arguments) {
                    TEMPEST::OperationArgument field;
                    field.name = argument.name;
                    field.type = argument.type;
                    field.description = argument.description;
                    field.required = true;
                    field.choices = argument.choices;
                    descriptor.arguments.push_back(std::move(field));
                }
                operations.push_back(TEMPEST::operationDescriptorValue(descriptor));
            }
            respond(message, {{"operations", std::move(operations)}});
            return;
        }
        if (message.name == "tempest/telemetry/list") {
            respond(message, {{"telemetry", TEMPEST::Value::Array{}},
                {"reason", "F Prime CCSDS channels and events are decoded from the topology dictionary; this peer registers no TEMPEST telemetry topics."}});
            return;
        }
        if (message.name == "tempest-peer/channels/query") {
            DARTWIC::ChannelQueryResult result;
            { std::lock_guard lock(mutex); for (const auto& [_, channel] : channels) result.channels.push_back(channel); }
            respond(message, DARTWIC::EngineContract::result(result));
            return;
        }
        if (message.name == "tempest-peer/argus/query") {
            respond(message, DARTWIC::EngineContract::result(DARTWIC::ArgusQueryResult{}));
            return;
        }
        if ((message.name == "demo/set-running" && command_name.empty())
            || (message.name != "demo/set-running" && message.name != "fprime/command"
                && !dictionary.hasCommand(message.name))) {
            fail(message, "F Prime does not support this operation", "unsupported");
            return;
        }
        try {
            std::string command = command_name;
            TEMPEST::Value::Object arguments;
            if (message.name == "demo/set-running") arguments = {{"running", boolean(message.payload.at("running"))}};
            else if (message.name == "fprime/command") {
                command = message.payload.at("name").string();
                arguments = message.payload.at("arguments").object();
            } else { command = message.name; arguments = message.payload; }
            uint64_t opcode = 0;
            auto frame = commandFrame(command, arguments, opcode);
            if (message.deadline < std::chrono::steady_clock::now()) {
                fail(message, "Command deadline expired before transmission", "timeout"); return;
            }
            if (!sendFully(client, frame)) {
                fail(message, "F Prime link failed during send; execution is unknown", "disconnected");
                disconnectLink("F Prime TCP command send failed");
                return;
            }
            sent_bytes.fetch_add(frame.size(), std::memory_order_relaxed);
            last_send_ms.store(nowNs() / 1'000'000ULL, std::memory_order_relaxed);
            respond(message, {{"status", "sent"}, {"execution_confirmed", false},
                              {"command", command}, {"opcode", opcode}});
        } catch (const std::exception& error) { fail(message, error.what(), "invalid_arguments"); }
    }

    void disconnectLink(const char* reason) {
        const Socket socket = client.exchange(INVALID_SOCKET_HANDLE);
        if (socket == INVALID_SOCKET_HANDLE) return;
        closeSocket(socket);
        link_epoch.fetch_add(1);
        incoming.clear();
        {
            std::lock_guard lock(frame_mutex);
            frames.clear();
        }
        { std::lock_guard lock(mutex); channels.clear(); outbound.clear(); }
        state(TEMPEST::ConnectionState::Disconnected, reason);
    }

    void run() {
        while (running) {
            if (client == INVALID_SOCKET_HANDLE) {
                fd_set read_set; FD_ZERO(&read_set); FD_SET(listener, &read_set);
                timeval timeout{0, 100000};
                if (select(static_cast<int>(listener + 1), &read_set, nullptr, nullptr, &timeout) <= 0) continue;
                client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET_HANDLE) continue;
#ifdef _WIN32
                DWORD send_timeout_ms = 1000;
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&send_timeout_ms), sizeof(send_timeout_ms));
#else
                timeval send_timeout{1, 0};
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
#endif
                incoming.clear();
                { std::lock_guard lock(mutex); channels.clear(); outbound.clear(); }
                session = node + ":" + std::to_string(nowNs()) + ":" + std::to_string(++session_sequence);
                link_epoch.fetch_add(1);
                last_valid_frame_steady_ns.store(steadyNs(), std::memory_order_relaxed);
                state(TEMPEST::ConnectionState::Connected);
            }
            std::deque<TEMPEST::Message> pending;
            { std::lock_guard lock(mutex); pending.swap(outbound); }
            for (auto& message : pending) {
                if (client == INVALID_SOCKET_HANDLE) break;
                handle(std::move(message));
            }
            if (client == INVALID_SOCKET_HANDLE) continue;
            fd_set read_set; FD_ZERO(&read_set); FD_SET(client, &read_set);
            timeval timeout{0, 100000};
            const int ready = select(static_cast<int>(client + 1), &read_set, nullptr, nullptr, &timeout);
            if (ready == 0) {
                const auto last = last_valid_frame_steady_ns.load(std::memory_order_relaxed);
                if (downlink_idle_timeout_ns && steadyNs() - last > downlink_idle_timeout_ns)
                    disconnectLink("F Prime CCSDS downlink idle; reconnecting");
                continue;
            }
            std::array<uint8_t, 4096> buffer{};
            const int count = ready < 0 ? -1 : recv(client, reinterpret_cast<char*>(buffer.data()),
                                                   static_cast<int>(buffer.size()), 0);
            if (count <= 0) {
                disconnectLink("F Prime TCP connection lost");
                continue;
            }
            received_bytes.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
            last_receive_ms.store(nowNs() / 1'000'000ULL, std::memory_order_relaxed);
            incoming.insert(incoming.end(), buffer.begin(), buffer.begin() + count);
            parseIncoming();
        }
        disconnectLink("F Prime transport stopped");
    }

    void start(TEMPEST::TransportCallbacks value) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        winsock_ready = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET_HANDLE) throw std::runtime_error("F Prime listener socket failed");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
        if (inet_pton(AF_INET, bind_host.c_str(), &address.sin_addr) != 1)
            throw std::invalid_argument("F Prime bind_host must be an IPv4 address");
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
            throw std::runtime_error("Cannot bind F Prime listener on port " + std::to_string(port));
        callbacks = std::move(value);
        running = true;
        decoder = std::thread([this] { decodeFrames(); });
        worker = std::thread([this] { run(); });
    }
    void stop() {
        running = false;
        frame_ready.notify_all();
        if (worker.joinable()) worker.join();
        if (decoder.joinable()) decoder.join();
        if (listener != INVALID_SOCKET_HANDLE) { closeSocket(listener); listener = INVALID_SOCKET_HANDLE; }
#ifdef _WIN32
        if (winsock_ready) { WSACleanup(); winsock_ready = false; }
#endif
        callbacks = {};
    }
};

FprimeTransport::FprimeTransport(const nlohmann::json& config) : impl_(std::make_unique<Impl>(config)) {}
FprimeTransport::~FprimeTransport() { stop(); }
void FprimeTransport::start(TEMPEST::TransportCallbacks callbacks) { impl_->start(std::move(callbacks)); }
void FprimeTransport::send(TEMPEST::Message message) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running || impl_->client == INVALID_SOCKET_HANDLE)
        throw TEMPEST::DisconnectedError("F Prime is not connected");
    if (impl_->outbound.size() >= 256) throw TEMPEST::QueueFullError("F Prime command queue full");
    impl_->outbound.push_back(std::move(message));
}
void FprimeTransport::stop() { if (impl_) impl_->stop(); }
std::vector<TEMPEST::TransportPath> FprimeTransport::diagnostics() const {
    TEMPEST::TransportPath path;
    path.id = "fprime/ccsds";
    path.role = "Bidirectional CCSDS TCP";
    path.endpoint = impl_->bind_host + ":" + std::to_string(impl_->port);
    path.state = impl_->client.load() == INVALID_SOCKET_HANDLE ? "disconnected" : "connected";
    path.transport = "fprime/ccsds";
    path.last_receive_ms = impl_->last_receive_ms.load();
    path.last_send_ms = impl_->last_send_ms.load();
    path.bytes_received = impl_->received_bytes.load();
    path.bytes_sent = impl_->sent_bytes.load();
    path.dropped = impl_->dropped_frames.load();
    path.byte_counters_available = true;
    { std::lock_guard lock(impl_->frame_mutex); path.queued = impl_->frames.size(); }
    return {path};
}

} // namespace FPrimeBridge
