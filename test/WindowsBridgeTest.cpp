#include "FprimeTransport.hpp"
#include <tempest/Peer.h>
#include <nlohmann/json.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}
static void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, static_cast<uint16_t>(value >> 16));
    put16(out, static_cast<uint16_t>(value));
}
static uint16_t read16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}
static uint32_t read32(const uint8_t* data) {
    return (static_cast<uint32_t>(read16(data)) << 16) | read16(data + 2);
}
static uint16_t crc(const uint8_t* data, size_t length) {
    uint16_t value = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        value ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            value = value & 0x8000 ? static_cast<uint16_t>((value << 1) ^ 0x1021)
                                   : static_cast<uint16_t>(value << 1);
    }
    return value;
}
static void sendAll(SOCKET socket, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int count = send(socket, reinterpret_cast<const char*>(data.data() + sent),
                               static_cast<int>(data.size() - sent), 0);
        require(count > 0, "Windows test socket send failed");
        sent += static_cast<size_t>(count);
    }
}
static void recvAll(SOCKET socket, std::vector<uint8_t>& data, size_t size) {
    data.resize(size);
    size_t received = 0;
    while (received < size) {
        const int count = recv(socket, reinterpret_cast<char*>(data.data() + received),
                               static_cast<int>(size - received), 0);
        require(count > 0, "Windows test socket receive failed");
        received += static_cast<size_t>(count);
    }
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass a generated Hadron SITL F Prime dictionary");
        std::ifstream input(argv[1]);
        require(input.good(), "dictionary missing");
        const auto dictionary = nlohmann::json::parse(input);
        uint32_t channel_id = 0;
        uint32_t command_opcode = 0;
        uint32_t warning_event_id = 0;
        for (const auto& channel : dictionary.at("telemetryChannels"))
            if (channel.at("name") == "HadronFSW.plantModel.SimAltitudeM"
                || channel.at("name") == "HadronFSW.plantModel.DemoAltitudeM")
                channel_id = channel.at("id").get<uint32_t>();
        for (const auto& command : dictionary.at("commands"))
            if (command.at("name") == "CdhCore.cmdDisp.CMD_NO_OP")
                command_opcode = command.at("opcode").get<uint32_t>();
        for (const auto& event : dictionary.at("events"))
            if (event.at("name") == "ComCcsds.frameAccumulator.NoBufferAvailable")
                warning_event_id = event.at("id").get<uint32_t>();
        require(channel_id != 0 && command_opcode != 0 && warning_event_id != 0,
                "Hadron channel, warning event, or no-op command missing");

        WSADATA winsock{};
        require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WSAStartup failed");
        std::mutex mutex;
        std::condition_variable changed;
        bool connected = false;
        std::vector<TEMPEST::Message> messages;
        FPrimeBridge::FprimeTransport transport({
            {"node_name", "HADRON_SITL"}, {"bind_host", "127.0.0.1"}, {"port", 50129},
            {"dictionary", argv[1]}, {"spacecraft_id", 68},
            {"virtual_channel_id", 1}, {"tm_frame_size", 1024},
            {"downlink_idle_timeout_ms", 3000}});
        transport.start({
            [&](TEMPEST::Message message) {
                { std::lock_guard lock(mutex); messages.push_back(std::move(message)); }
                changed.notify_all();
            },
            [&](TEMPEST::ConnectionState state, std::string) {
                { std::lock_guard lock(mutex); connected = state == TEMPEST::ConnectionState::Connected; }
                changed.notify_all();
            }});

        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket != INVALID_SOCKET, "cannot create test socket");
        DWORD timeout = 3000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(50129);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        require(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "cannot connect to bridge listener");
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] { return connected; }), "bridge did not connect");
        }
        transport.send({TEMPEST::Message::Kind::Request, "catalog-1", "tempest/operations/list", {}});
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                return std::any_of(messages.begin(), messages.end(), [](const auto& message) {
                    return message.request_id == "catalog-1" && message.kind == TEMPEST::Message::Kind::Response;
                });
            }), "F Prime command catalog did not respond");
            const auto found = std::find_if(messages.begin(), messages.end(), [](const auto& message) {
                return message.request_id == "catalog-1";
            });
            bool saw_noop = false, saw_typed_argument = false;
            size_t dictionary_commands = 0;
            for (const auto& operation : found->payload.at("operations").array()) {
                const auto& item = operation.object();
                const auto name = item.at("name").string();
                if (name.find('.') != std::string::npos) ++dictionary_commands;
                if (name == "CdhCore.cmdDisp.CMD_NO_OP") {
                    saw_noop = true;
                    require(item.at("arguments").array().empty(), "no-op command should have no arguments");
                }
                if (name == "CdhCore.cmdDisp.CMD_NO_OP_STRING") {
                    const auto& arguments = item.at("arguments").array();
                    saw_typed_argument = arguments.size() == 1
                        && arguments.front().object().at("type").string() == "string";
                }
            }
            require(saw_noop && saw_typed_argument
                    && dictionary_commands == dictionary.at("commands").size(),
                    "dictionary commands or argument types missing from catalog");
        }

        transport.send({TEMPEST::Message::Kind::Request, "telemetry-catalog-1", "tempest/telemetry/list", {}});
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                return std::any_of(messages.begin(), messages.end(), [](const auto& message) {
                    return message.request_id == "telemetry-catalog-1"
                        && message.kind == TEMPEST::Message::Kind::Response;
                });
            }), "F Prime telemetry catalog did not respond");
            const auto found = std::find_if(messages.begin(), messages.end(), [](const auto& message) {
                return message.request_id == "telemetry-catalog-1";
            });
            require(!found->error && found->payload.at("telemetry").array().empty()
                && !found->payload.at("reason").string().empty(),
                "F Prime telemetry catalog should explain that no topics are registered");
        }

        // F Prime TlmChan packet: descriptor, channel ID, 11-byte Fw::Time, F32 value.
        std::vector<uint8_t> payload;
        put16(payload, 1);
        put32(payload, channel_id);
        payload.insert(payload.end(), {0, 0, 0});
        put32(payload, 123);
        put32(payload, 500000);
        put32(payload, 0x42c80000); // 100.0f
        std::vector<uint8_t> space_packet;
        put16(space_packet, 1);
        put16(space_packet, 0xc000);
        put16(space_packet, static_cast<uint16_t>(payload.size() - 1));
        space_packet.insert(space_packet.end(), payload.begin(), payload.end());
        std::vector<uint8_t> frame;
        put16(frame, static_cast<uint16_t>((68 << 4) | (1 << 1)));
        frame.insert(frame.end(), {0, 0});
        put16(frame, 0x1800);
        frame.insert(frame.end(), space_packet.begin(), space_packet.end());
        const size_t idle_size = 1024 - 2 - frame.size();
        require(idle_size >= 7, "test telemetry exceeds frame");
        put16(frame, 0x07ff);
        put16(frame, 0xc000);
        put16(frame, static_cast<uint16_t>(idle_size - 7));
        frame.insert(frame.end(), idle_size - 6, 0x55);
        put16(frame, crc(frame.data(), frame.size()));
        require(frame.size() == 1024, "wrong TM frame length");
        sendAll(socket, frame);
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                for (const auto& message : messages)
                    if (message.kind == TEMPEST::Message::Kind::Telemetry) return true;
                return false;
            }), "TM frame did not produce TEMPEST telemetry");
        }

        std::vector<uint8_t> event_payload;
        put16(event_payload, 2);
        put32(event_payload, warning_event_id);
        event_payload.insert(event_payload.end(), 11, 0);
        std::vector<uint8_t> event_packet;
        put16(event_packet, 2);
        put16(event_packet, 0xc000);
        put16(event_packet, static_cast<uint16_t>(event_payload.size() - 1));
        event_packet.insert(event_packet.end(), event_payload.begin(), event_payload.end());
        std::vector<uint8_t> event_frame;
        put16(event_frame, static_cast<uint16_t>((68 << 4) | (1 << 1)));
        event_frame.insert(event_frame.end(), {0, 0});
        put16(event_frame, 0x1800);
        event_frame.insert(event_frame.end(), event_packet.begin(), event_packet.end());
        const size_t event_idle = 1024 - 2 - event_frame.size();
        put16(event_frame, 0x07ff);
        put16(event_frame, 0xc000);
        put16(event_frame, static_cast<uint16_t>(event_idle - 7));
        event_frame.insert(event_frame.end(), event_idle - 6, 0x55);
        put16(event_frame, crc(event_frame.data(), event_frame.size()));
        sendAll(socket, event_frame);
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                return std::any_of(messages.begin(), messages.end(), [](const auto& message) {
                    return message.name == "tempest-peer/argus/logs"
                        && message.payload.at("stream").string() == "F Prime Events";
                });
            }), "CCSDS event did not reach ARGUS logs");
            const auto found = std::find_if(messages.begin(), messages.end(), [](const auto& message) {
                return message.name == "tempest-peer/argus/logs"
                    && message.payload.at("stream").string() == "F Prime Events";
            });
            require(found->payload.at("level").string() == "warning", "F Prime warning severity was lost");
        }

        transport.send({TEMPEST::Message::Kind::Request, "command-1", "fprime/command",
                        {{"name", "CdhCore.cmdDisp.CMD_NO_OP"}, {"arguments", TEMPEST::Value::Object{}}}});
        std::vector<uint8_t> tc;
        recvAll(socket, tc, 5);
        const size_t tc_length = (read16(tc.data() + 2) & 0x03ff) + 1;
        require(tc_length >= 19, "TC frame too short");
        std::vector<uint8_t> tail;
        recvAll(socket, tail, tc_length - 5);
        tc.insert(tc.end(), tail.begin(), tail.end());
        require(read16(tc.data() + tc.size() - 2) == crc(tc.data(), tc.size() - 2),
                "TC CRC invalid");
        require(read16(tc.data()) == 0x2044, "TC spacecraft ID or bypass flag invalid");
        require(read16(tc.data() + 5) == 0x1000, "TC space packet is not APID 0");
        require(read16(tc.data() + 11) == 0, "F Prime command descriptor invalid");
        require(read32(tc.data() + 13) == command_opcode, "F Prime opcode invalid");
        transport.send({TEMPEST::Message::Kind::Request, "command-2", "CdhCore.cmdDisp.CMD_NO_OP", {}});
        recvAll(socket, tc, 5);
        const size_t direct_length = (read16(tc.data() + 2) & 0x03ff) + 1;
        recvAll(socket, tail, direct_length - 5);
        tc.insert(tc.end(), tail.begin(), tail.end());
        require(read32(tc.data() + 13) == command_opcode, "catalog command opcode invalid");
        // A real dictionary command with a string followed by a bool verifies
        // the outbound wire bytes. This socket is a fixture, not flight software.
        uint32_t remove_file_opcode = 0;
        for (const auto& command : dictionary.at("commands"))
            if (command.at("name") == "FileHandling.fileManager.RemoveFile")
                remove_file_opcode = command.at("opcode").get<uint32_t>();
        require(remove_file_opcode != 0, "Hadron boolean command missing");
        for (const bool ignore_errors : {true, false}) {
            transport.send({TEMPEST::Message::Kind::Request,
                ignore_errors ? "boolean-true" : "boolean-false", "FileHandling.fileManager.RemoveFile",
                {{"fileName", "fixture"}, {"ignoreErrors", ignore_errors}}});
            recvAll(socket, tc, 5);
            const size_t bool_length = (read16(tc.data() + 2) & 0x03ff) + 1;
            require(bool_length == 29, "boolean command frame length invalid");
            recvAll(socket, tail, bool_length - 5);
            tc.insert(tc.end(), tail.begin(), tail.end());
            require(read16(tc.data() + tc.size() - 2) == crc(tc.data(), tc.size() - 2),
                "boolean command frame CRC invalid");
            require(read32(tc.data() + 13) == remove_file_opcode,
                "boolean command opcode invalid");
            require(read16(tc.data() + 17) == 7
                && std::string(tc.begin() + 19, tc.begin() + 26) == "fixture",
                "boolean command preceding string invalid");
            require(tc[26] == (ignore_errors ? 0xFF : 0x00),
                "boolean command must use F Prime FF/00 wire values");
        }
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                for (const auto& message : messages)
                    if (message.request_id == "command-1" &&
                        message.kind == TEMPEST::Message::Kind::Response && !message.error) return true;
                return false;
            }), "command send was not acknowledged");
        }
        for (int index = 0; index < 128; ++index) sendAll(socket, frame);
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                return std::count_if(messages.begin(), messages.end(), [](const auto& message) {
                    return message.name == "tempest-peer/channels/updated";
                }) >= 64;
            }), "sustained CCSDS telemetry did not decode");
        }
        const auto paths = transport.diagnostics();
        require(paths.size() == 1 && paths[0].byte_counters_available
            && paths[0].bytes_received >= frame.size() * 129
            && paths[0].bytes_sent >= tc.size(), "physical bridge byte counters did not advance");
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 5s, [&] { return !connected; }),
                "idle CCSDS downlink did not mark the bridge disconnected");
        }
        closesocket(socket);
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket != INVALID_SOCKET && connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "bridge listener did not accept a recovered CCSDS link");
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] { return connected; }), "bridge did not reconnect");
        }
        std::size_t before_reconnect = 0;
        { std::lock_guard lock(mutex); before_reconnect = std::count_if(messages.begin(), messages.end(),
            [](const auto& message) { return message.name == "tempest-peer/channels/updated"; }); }
        sendAll(socket, frame);
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 3s, [&] {
                return std::count_if(messages.begin(), messages.end(), [](const auto& message) {
                    return message.name == "tempest-peer/channels/updated";
                }) > before_reconnect;
            }),
                "CCSDS telemetry did not resume after reconnect");
        }
        closesocket(socket);
        transport.stop();

        auto peer_transport = std::make_shared<FPrimeBridge::FprimeTransport>(nlohmann::json{
            {"node_name", "HADRON_SITL"}, {"bind_host", "127.0.0.1"}, {"port", 50130},
            {"dictionary", argv[1]}, {"spacecraft_id", 68},
            {"virtual_channel_id", 1}, {"tm_frame_size", 1024}});
        TEMPEST::PeerConfig peer_config;
        peer_config.node_name = "MY_NODE";
        peer_config.peer_id = "fprime_bridge.fprime_ccsds";
        peer_config.protocol_id = "tempest.engine";
        TEMPEST::Peer peer(peer_config, peer_transport);
        peer.start();
        SOCKET peer_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(peer_socket != INVALID_SOCKET, "cannot create peer test socket");
        address.sin_port = htons(50130);
        require(connect(peer_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "cannot connect peer test socket");
        peer.waitUntilReady(3s);
        require(peer.remoteNode() == "HADRON_SITL", "peer registered the wrong node");
        peer.stop();
        closesocket(peer_socket);
        WSACleanup();
        std::cout << "Windows CCSDS bridge telemetry and command round-trip passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
