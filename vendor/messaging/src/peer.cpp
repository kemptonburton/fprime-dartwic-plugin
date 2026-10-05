#include <tempest/Peer.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace TEMPEST {
namespace {
constexpr auto registration = "tempest/register-peer";
constexpr auto heartbeat = "tempest/heartbeat";
constexpr int wire_version = 2;
std::atomic<uint64_t> identities{1};
std::string identity() {
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
        + ":" + std::to_string(identities.fetch_add(1));
}
void validateName(const std::string& name) {
    if (name.empty()) throw Error("An operation or topic name cannot be empty.");
}
void finishThread(std::thread& thread) {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) thread.detach();
    else thread.join();
}
}

Value::Object operationDescriptorValue(const OperationDescriptor& descriptor) {
    Value::Array arguments;
    for (const auto& argument : descriptor.arguments) {
        Value::Object item{{"name", argument.name}, {"type", argument.type},
                           {"description", argument.description}, {"required", argument.required}};
        if (argument.default_value) item["default"] = *argument.default_value;
        if (!argument.choices.empty()) item["choices"] = Value::Array(argument.choices);
        arguments.emplace_back(std::move(item));
    }
    return {{"name", descriptor.name},
            {"display_name", descriptor.display_name.empty() ? descriptor.name : descriptor.display_name},
            {"description", descriptor.description}, {"category", descriptor.category},
            {"arguments", std::move(arguments)}, {"plugin_id", descriptor.plugin_id},
            {"allowed_roles", descriptor.allowed_roles == AllowedRoles::AdminAndViewer
                ? Value::Array{Value{"admin"}, Value{"viewer"}} : Value::Array{Value{"admin"}}}};
}

Value::Object telemetryDescriptorValue(const TelemetryDescriptor& descriptor) {
    return {{"topic", descriptor.topic},
            {"display_name", descriptor.display_name.empty() ? descriptor.topic : descriptor.display_name},
            {"description", descriptor.description}, {"category", descriptor.category},
            {"direction", descriptor.direction}, {"delivery", descriptor.delivery},
            {"plugin_id", descriptor.plugin_id}};
}

struct Peer::Impl : std::enable_shared_from_this<Impl> {
    PeerConfig config;
    TransportPtr transport;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool running = false;
    bool stopping = false;
    bool link = false;
    bool ready = false;
    ConnectionState state = ConnectionState::Disconnected;
    std::string session;
    std::string remote_node;
    std::string remote_session;
    uint64_t next_id = 1;
    uint64_t dropped = 0;
    uint64_t operation_timeouts = 0;
    uint64_t epoch = 0;
    struct Pending { bool done = false; Message response; };
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending;
    struct BoundOperation { OperationDescriptor descriptor; OperationHandler handler; };
    std::unordered_map<std::string, BoundOperation> operations;
    std::function<std::vector<OperationDescriptor>()> catalog_provider;
    std::unordered_map<std::string, TelemetryDescriptor> telemetry_descriptors;
    std::function<std::vector<TelemetryDescriptor>()> telemetry_catalog_provider;
    std::function<Value::Object(const std::string&, const Value::Object&)> fallback;
    std::unordered_map<std::string, TelemetryHandler> topics;
    std::function<void(const std::string&, const Value::Object&)> telemetry_fallback;
    std::function<void(ConnectionState, std::string)> state_handler;
    std::deque<Message> requests;
    std::deque<std::function<void()>> callbacks;
    std::thread request_thread, callback_thread, maintenance_thread;

    Impl(PeerConfig c, TransportPtr t) : config(std::move(c)), transport(std::move(t)) {
        if (config.node_name.empty() || config.peer_id.empty() || !config.peer_version
            || config.protocol_id.empty() || !config.protocol_version || !transport)
            throw Error("A peer needs a node name, peer definition, version, and transport.");
        if (config.request_timeout.count() <= 0 || config.heartbeat_interval.count() <= 0
            || config.heartbeat_timeout <= config.heartbeat_interval
            || !config.max_pending_requests || !config.max_queued_requests || !config.max_queued_callbacks)
            throw Error("Invalid peer timeouts or queue limits.");
    }

    void notifyStateLocked(ConnectionState value, std::string reason) {
        state = value;
        if (state_handler) {
            // State changes must survive telemetry congestion.
            if (callbacks.size() >= config.max_queued_callbacks) { callbacks.pop_back(); ++dropped; }
            auto handler = state_handler;
            callbacks.push_back([handler, value, reason = std::move(reason)] { handler(value, reason); });
        }
        cv.notify_all();
    }

    void connectionChanged(ConnectionState value, std::string reason) {
        std::lock_guard lock(mutex);
        if (!running) return;
        ++epoch;
        ready = false;
        link = value == ConnectionState::Connected;
        session = identity();
        remote_session.clear();
        remote_node.clear();
        requests.clear();
        pending.clear();
        notifyStateLocked(link ? ConnectionState::Connecting : value, std::move(reason));
    }

    Value::Object localIdentity() {
        std::lock_guard lock(mutex);
        return {{"version", wire_version}, {"node_name", config.node_name},
                {"session_id", session}, {"peer_id", config.peer_id}, {"peer_version", config.peer_version},
                {"protocol_id", config.protocol_id}, {"protocol_version", config.protocol_version}};
    }

    void acceptIdentity(const Value::Object& payload) {
        const auto version = payload.at("version").storage();
        const bool validVersion = (std::holds_alternative<int64_t>(version)
            && std::get<int64_t>(version) == wire_version)
            || (std::holds_alternative<uint64_t>(version) && std::get<uint64_t>(version) == wire_version);
        if (!validVersion) throw RemoteError("Unsupported TEMPEST peer version.", "version_mismatch");
        auto node = payload.at("node_name").string();
        auto remote = payload.at("session_id").string();
        const auto peer_id = payload.at("peer_id").string();
        const auto peer_version = payload.at("peer_version").storage();
        const bool valid_peer_version = (std::holds_alternative<int64_t>(peer_version)
            && std::get<int64_t>(peer_version) == static_cast<int64_t>(config.peer_version))
            || (std::holds_alternative<uint64_t>(peer_version)
                && std::get<uint64_t>(peer_version) == config.peer_version);
        if (peer_id != config.peer_id || !valid_peer_version)
            throw RemoteError("Peer definition or version does not match.", "peer_definition_mismatch");
        const auto protocol_id = payload.at("protocol_id").string();
        const auto protocol_version = payload.at("protocol_version").storage();
        const bool valid_protocol_version = (std::holds_alternative<int64_t>(protocol_version)
            && std::get<int64_t>(protocol_version) == static_cast<int64_t>(config.protocol_version))
            || (std::holds_alternative<uint64_t>(protocol_version)
                && std::get<uint64_t>(protocol_version) == config.protocol_version);
        if (protocol_id != config.protocol_id || !valid_protocol_version)
            throw RemoteError("Peer protocol or version does not match.", "protocol_mismatch");
        if (node.empty() || remote.empty() || node == config.node_name)
            throw RemoteError("Invalid remote peer identity.", "invalid_identity");
        std::lock_guard lock(mutex);
        if (!running || !link)
            throw RemoteError("This peer connection was disconnected. Add the peer again to reconnect.", "peer_disconnected");
        if (!remote_session.empty() && remote_session != remote) {
            ++epoch;
            ready = false;
            pending.clear();
            requests.clear();
            callbacks.clear();
        }
        remote_node = std::move(node);
        remote_session = std::move(remote);
        if (!ready) {
            ready = true;
            notifyStateLocked(ConnectionState::Connected, {});
        }
        cv.notify_all();
    }

    void reply(const Message& request, Value::Object payload, bool error = false, std::string code = {}) {
        transport->send({Message::Kind::Response, request.request_id, request.name,
                         std::move(payload), error, std::move(code)});
    }

    void receive(Message message) {
        if (message.name.empty()) return;
        if (message.kind == Message::Kind::Response) {
            std::lock_guard lock(mutex);
            auto found = pending.find(message.request_id);
            if (found == pending.end()) return;
            found->second->response = std::move(message);
            found->second->done = true;
            cv.notify_all();
            return;
        }
        if (message.kind == Message::Kind::Request) {
            if (message.request_id.empty()) return;
            // Transport control never waits behind an application command.
            if (message.name == registration || message.name == heartbeat) {
                try {
                    if (message.name == registration) acceptIdentity(message.payload);
                    reply(message, message.name == registration ? localIdentity() : Value::Object{});
                } catch (const RemoteError& e) {
                    reply(message, {{"error", e.what()}}, true, e.code());
                } catch (const std::exception& e) {
                    reply(message, {{"error", e.what()}}, true, "invalid_registration");
                }
                return;
            }
            bool accepted = false;
            bool connected = false;
            {
                std::lock_guard lock(mutex);
                connected = running && link;
                if (connected && requests.size() < config.max_queued_requests) {
                    requests.push_back(message);
                    accepted = true;
                }
            }
            if (!accepted) reply(message, {{"error", connected ? "Peer request queue is full." : "Peer is not ready."}},
                true, connected ? "queue_full" : "disconnected");
            cv.notify_all();
            return;
        }
        std::lock_guard lock(mutex);
        if (!running || !ready) return;
        auto found = topics.find(message.name);
        if (found == topics.end() && !telemetry_fallback) return;
        if (callbacks.size() >= config.max_queued_callbacks) { ++dropped; return; }
        if (found != topics.end())
            callbacks.push_back([handler = found->second, payload = std::move(message.payload)] { handler(payload); });
        else
            callbacks.push_back([handler = telemetry_fallback, topic = std::move(message.name), payload = std::move(message.payload)] { handler(topic, payload); });
        cv.notify_all();
    }

    Value::Object request(std::string name, Value::Object payload, std::chrono::milliseconds timeout, bool internal) {
        if (timeout.count() <= 0) timeout = config.request_timeout;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto value = std::make_shared<Pending>();
        std::string id;
        uint64_t request_epoch;
        {
            std::lock_guard lock(mutex);
            if (!running || !link || (!internal && !ready)) throw DisconnectedError("TEMPEST peer is not ready.");
            if (pending.size() >= config.max_pending_requests + (internal ? 1 : 0))
                throw QueueFullError("TEMPEST pending request limit reached.");
            id = session + ":" + std::to_string(next_id++);
            request_epoch = epoch;
            pending.emplace(id, value);
        }
        try {
            Message message{Message::Kind::Request, id, std::move(name), std::move(payload)};
            message.deadline = deadline;
            transport->send(std::move(message));
        } catch (...) {
            std::lock_guard lock(mutex);
            pending.erase(id);
            throw;
        }
        std::unique_lock lock(mutex);
        const bool completed = cv.wait_until(lock, deadline, [&] {
            return value->done || !running || request_epoch != epoch;
        });
        pending.erase(id);
        if (!running || request_epoch != epoch) throw DisconnectedError("Peer session ended while awaiting a response.");
        if (!completed) {
            if (!internal) ++operation_timeouts;
            throw TimeoutError("Operation timed out; remote completion is unknown.");
        }
        auto response = std::move(value->response);
        lock.unlock();
        if (response.error) {
            const auto error = response.payload.find("error");
            throw RemoteError(error == response.payload.end() ? "Remote operation failed." : error->second.string(),
                              response.error_code);
        }
        return std::move(response.payload);
    }

    void requestLoop() {
        std::unique_lock lock(mutex);
        while (running) {
            cv.wait(lock, [&] { return !running || (ready && !requests.empty()); });
            if (!running) break;
            auto message = std::move(requests.front());
            requests.pop_front();
            const auto request_epoch = epoch;
            const auto found = operations.find(message.name);
            auto handler = found == operations.end() ? OperationHandler{} : found->second.handler;
            auto fallback_handler = fallback;
            auto external_catalog = catalog_provider;
            auto external_telemetry_catalog = telemetry_catalog_provider;
            std::vector<OperationDescriptor> own_catalog;
            std::vector<TelemetryDescriptor> own_telemetry_catalog;
            if (message.name == "tempest/operations/list") {
                own_catalog.reserve(operations.size());
                for (const auto& [_, bound] : operations) own_catalog.push_back(bound.descriptor);
            } else if (message.name == "tempest/telemetry/list") {
                own_telemetry_catalog.reserve(telemetry_descriptors.size());
                for (const auto& [_, descriptor] : telemetry_descriptors) own_telemetry_catalog.push_back(descriptor);
            }
            lock.unlock();
            Value::Object result;
            bool error = false;
            std::string code;
            try {
                if (message.name == "tempest/operations/list") {
                    auto catalog = external_catalog ? external_catalog() : std::vector<OperationDescriptor>{};
                    for (auto& descriptor : own_catalog) {
                        std::erase_if(catalog, [&](const auto& entry) { return entry.name == descriptor.name; });
                        catalog.push_back(std::move(descriptor));
                    }
                    std::sort(catalog.begin(), catalog.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
                    Value::Array items;
                    for (const auto& descriptor : catalog) items.emplace_back(operationDescriptorValue(descriptor));
                    result = {{"operations", std::move(items)}};
                }
                else if (message.name == "tempest/telemetry/list") {
                    auto catalog = external_telemetry_catalog ? external_telemetry_catalog() : std::vector<TelemetryDescriptor>{};
                    for (auto& descriptor : own_telemetry_catalog) {
                        std::erase_if(catalog, [&](const auto& entry) { return entry.topic == descriptor.topic; });
                        catalog.push_back(std::move(descriptor));
                    }
                    std::sort(catalog.begin(), catalog.end(), [](const auto& a, const auto& b) { return a.topic < b.topic; });
                    Value::Array items;
                    for (const auto& descriptor : catalog) items.emplace_back(telemetryDescriptorValue(descriptor));
                    result = {{"telemetry", std::move(items)}};
                }
                else if (handler) result = handler(message.payload);
                else if (fallback_handler) result = fallback_handler(message.name, message.payload);
                else throw RemoteError("No handler for operation '" + message.name + "'.", "unsupported");
            } catch (const RemoteError& e) { error = true; code = e.code(); result = {{"error", e.what()}}; }
            catch (const std::exception& e) { error = true; code = "handler_error"; result = {{"error", e.what()}}; }
            catch (...) { error = true; code = "handler_error"; result = {{"error", "Unknown handler failure."}}; }
            lock.lock();
            const bool current = running && epoch == request_epoch;
            lock.unlock();
            if (current) { try { reply(message, std::move(result), error, std::move(code)); } catch (...) {} }
            lock.lock();
        }
    }

    void callbackLoop() {
        std::unique_lock lock(mutex);
        while (running || !callbacks.empty()) {
            cv.wait(lock, [&] { return !running || !callbacks.empty(); });
            if (callbacks.empty()) break;
            auto callback = std::move(callbacks.front());
            callbacks.pop_front();
            lock.unlock();
            try { callback(); } catch (...) {}
            lock.lock();
        }
    }

    void maintenanceLoop() {
        auto last_alive = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        while (running) {
            cv.wait(lock, [&] { return !running || link; });
            if (!running) break;
            const bool registered = ready;
            lock.unlock();
            bool alive = false;
            std::string disconnected_reason, registration_error;
            try {
                auto result = request(registered ? heartbeat : registration,
                    registered ? Value::Object{} : localIdentity(),
                    registered ? std::chrono::milliseconds{250} : config.request_timeout, true);
                if (!registered) acceptIdentity(result);
                alive = true;
            } catch (const RemoteError& error) {
                if (error.code() == "peer_disconnected") disconnected_reason = error.what();
                else registration_error = error.what();
            } catch (const std::exception& error) {
                registration_error = error.what();
            } catch (...) { registration_error = "Peer registration failed with an unknown error."; }
            lock.lock();
            // Ordinary operation failures do not affect liveness. Initial
            // registration failures are connection failures and must be visible.
            if (running && link && !registered && !ready && !alive && disconnected_reason.empty()
                && state != ConnectionState::Reconnecting)
                notifyStateLocked(ConnectionState::Reconnecting, "Peer registration failed: " + registration_error);
            if (!disconnected_reason.empty()) {
                // An operator closed this connection. Heartbeat recovery must
                // not turn that deliberate action into a new registration.
                ++epoch;
                ready = false;
                link = false;
                pending.clear();
                requests.clear();
                notifyStateLocked(ConnectionState::Disconnected, std::move(disconnected_reason));
                // Release the hub slot as well as stopping heartbeat recovery.
                // Otherwise a fresh dial can reuse this dormant transport and
                // receive a registration reply without a ready application peer.
                lock.unlock();
                try { transport->stop(); } catch (...) {}
                lock.lock();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (alive) last_alive = now;
            if (ready && now - last_alive >= config.heartbeat_timeout) {
                ++epoch;
                ready = false;
                pending.clear();
                requests.clear();
                session = identity();
                remote_node.clear(); remote_session.clear();
                notifyStateLocked(ConnectionState::Reconnecting, "Peer heartbeat expired.");
                last_alive = now;
            }
            cv.wait_for(lock, ready ? config.heartbeat_interval : std::chrono::milliseconds{100}, [&] { return !running; });
        }
    }
};

Peer::Peer(PeerConfig config, TransportPtr transport)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(transport))) {}
Peer::~Peer() { stop(); }
void Peer::start() {
    auto p = impl_;
    {
        std::lock_guard lock(p->mutex);
        if (p->running) return;
        if (p->stopping) throw Error("A stopped peer cannot be restarted; construct a new peer.");
        p->running = true;
        p->session = identity();
        p->state = ConnectionState::Connecting;
    }
    p->request_thread = std::thread([p] { p->requestLoop(); });
    p->callback_thread = std::thread([p] { p->callbackLoop(); });
    p->maintenance_thread = std::thread([p] { p->maintenanceLoop(); });
    std::weak_ptr<Impl> weak = p;
    try {
        p->transport->start({
            [weak](Message message) { if (auto p = weak.lock()) p->receive(std::move(message)); },
            [weak](ConnectionState state, std::string reason) {
                if (auto p = weak.lock()) p->connectionChanged(state, std::move(reason));
            }});
    } catch (...) { stop(); throw; }
}
void Peer::stop() {
    auto p = impl_;
    {
        std::lock_guard lock(p->mutex);
        if (!p->running) return;
        p->running = false; p->stopping = true; p->ready = false; p->link = false;
        ++p->epoch;
        p->requests.clear(); p->pending.clear();
        p->callbacks.clear();
        p->notifyStateLocked(ConnectionState::Stopping, {});
    }
    p->cv.notify_all();
    p->transport->stop();
    finishThread(p->maintenance_thread);
    finishThread(p->request_thread);
    finishThread(p->callback_thread);
    std::lock_guard lock(p->mutex);
    p->state = ConnectionState::Disconnected;
}
void Peer::registerOperation(OperationDescriptor descriptor, OperationHandler handler) {
    validateName(descriptor.name);
    if (descriptor.name.starts_with("tempest/")) throw Error("The tempest/ operation namespace is reserved.");
    if (!handler) throw Error("Operation handler cannot be empty.");
    std::lock_guard lock(impl_->mutex);
    const auto name = descriptor.name;
    if (!impl_->operations.emplace(name, Impl::BoundOperation{std::move(descriptor), std::move(handler)}).second)
        throw Error("An operation with this name is already registered.");
}
void Peer::registerOperation(std::string name, OperationHandler handler) {
    registerOperation(OperationDescriptor{.name = std::move(name)}, std::move(handler));
}
std::vector<OperationDescriptor> Peer::operations() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<OperationDescriptor> catalog;
    catalog.reserve(impl_->operations.size());
    for (const auto& [_, bound] : impl_->operations) catalog.push_back(bound.descriptor);
    std::sort(catalog.begin(), catalog.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return catalog;
}
void Peer::setOperationCatalogProvider(std::function<std::vector<OperationDescriptor>()> provider) {
    std::lock_guard lock(impl_->mutex);
    impl_->catalog_provider = std::move(provider);
}
void Peer::registerTelemetry(TelemetryDescriptor descriptor) {
    validateName(descriptor.topic);
    std::lock_guard lock(impl_->mutex);
    impl_->telemetry_descriptors[descriptor.topic] = std::move(descriptor);
}
std::vector<TelemetryDescriptor> Peer::telemetryTopics() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<TelemetryDescriptor> catalog;
    catalog.reserve(impl_->telemetry_descriptors.size());
    for (const auto& [_, descriptor] : impl_->telemetry_descriptors) catalog.push_back(descriptor);
    std::sort(catalog.begin(), catalog.end(), [](const auto& a, const auto& b) { return a.topic < b.topic; });
    return catalog;
}
void Peer::setTelemetryCatalogProvider(std::function<std::vector<TelemetryDescriptor>()> provider) {
    std::lock_guard lock(impl_->mutex);
    impl_->telemetry_catalog_provider = std::move(provider);
}
void Peer::onTelemetry(std::string topic, TelemetryHandler handler) {
    validateName(topic);
    if (!handler) throw Error("Telemetry handler cannot be empty.");
    std::lock_guard lock(impl_->mutex);
    impl_->topics[std::move(topic)] = std::move(handler);
}
void Peer::setOperationFallback(std::function<Value::Object(const std::string&, const Value::Object&)> handler) {
    std::lock_guard lock(impl_->mutex); impl_->fallback = std::move(handler);
}
void Peer::setTelemetryFallback(std::function<void(const std::string&, const Value::Object&)> handler) {
    std::lock_guard lock(impl_->mutex); impl_->telemetry_fallback = std::move(handler);
}
void Peer::onStateChanged(std::function<void(ConnectionState, std::string)> handler) {
    std::lock_guard lock(impl_->mutex); impl_->state_handler = std::move(handler);
}
Value::Object Peer::call(std::string name, Value::Object args, std::chrono::milliseconds timeout) {
    validateName(name);
    return impl_->request(std::move(name), std::move(args), timeout, false);
}
void Peer::publish(std::string topic, Value::Object payload) {
    validateName(topic);
    { std::lock_guard lock(impl_->mutex); if (!impl_->ready) { ++impl_->dropped; return; } }
    try { impl_->transport->send({Message::Kind::Telemetry, {}, std::move(topic), std::move(payload)}); }
    catch (...) { std::lock_guard lock(impl_->mutex); ++impl_->dropped; }
}
bool Peer::ready() const { std::lock_guard lock(impl_->mutex); return impl_->ready; }
bool Peer::publishSerializedTelemetry(std::string topic, std::string encoding,
    std::shared_ptr<const std::string> payload) {
    validateName(topic);
    auto* adapter = dynamic_cast<SerializedTelemetryTransport*>(impl_->transport.get());
    if (!adapter) return false;
    { std::lock_guard lock(impl_->mutex); if (!impl_->ready) { ++impl_->dropped; return true; } }
    try { return adapter->sendSerializedTelemetry(std::move(topic), std::move(encoding), std::move(payload)); }
    catch (...) { std::lock_guard lock(impl_->mutex); ++impl_->dropped; return true; }
}
void Peer::waitUntilReady(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->cv.wait_for(lock, timeout, [&] { return impl_->ready || impl_->stopping; }))
        throw TimeoutError("TEMPEST peer registration timed out.");
    if (!impl_->ready) throw DisconnectedError("TEMPEST peer stopped before registration.");
}
ConnectionState Peer::state() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
std::string Peer::nodeName() const { return impl_->config.node_name; }
std::string Peer::sessionId() const { std::lock_guard lock(impl_->mutex); return impl_->session; }
std::string Peer::remoteNode() const { std::lock_guard lock(impl_->mutex); return impl_->remote_node; }
std::string Peer::remoteSession() const { std::lock_guard lock(impl_->mutex); return impl_->remote_session; }
std::string Peer::peerId() const { return impl_->config.peer_id; }
uint64_t Peer::peerVersion() const { return impl_->config.peer_version; }
std::string Peer::protocolId() const { return impl_->config.protocol_id; }
uint64_t Peer::protocolVersion() const { return impl_->config.protocol_version; }
std::vector<TransportPath> Peer::transportDiagnostics() const { return impl_->transport->diagnostics(); }
uint64_t Peer::droppedTelemetry() const { std::lock_guard lock(impl_->mutex); return impl_->dropped; }
uint64_t Peer::operationTimeouts() const { std::lock_guard lock(impl_->mutex); return impl_->operation_timeouts; }
} // namespace TEMPEST
