#pragma once

#include <tempest/Transport.h>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace TEMPEST {

/** Authorization roles permitted to invoke an operation. New operations default to administrators. */
enum class AllowedRoles { AdminOnly, AdminAndViewer };

/** Base messaging failure. */
class Error : public std::runtime_error { using std::runtime_error::runtime_error; };
/** The connection ended before the operation completed. */
class DisconnectedError : public Error { using Error::Error; };
/** The single request deadline expired; remote completion is unknown. */
class TimeoutError : public Error { using Error::Error; };
/** A bounded request or transport queue rejected the operation. */
class QueueFullError : public Error { using Error::Error; };
/** Remote operation failure with a machine-readable code. */
class RemoteError : public Error {
public:
    RemoteError(std::string message, std::string code) : Error(std::move(message)), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

/** Explicit peer and protocol identity, deadlines and bounded queue limits. */
struct PeerConfig {
    std::string node_name;
    std::string peer_id;
    uint64_t peer_version = 1;
    std::string protocol_id;
    uint64_t protocol_version = 1;
    std::chrono::milliseconds request_timeout{5000};
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
    std::size_t max_pending_requests = 256;
    std::size_t max_queued_requests = 256;
    std::size_t max_queued_callbacks = 1024;
};

/** Metadata for a callable operation. Payload values remain codec independent. */
struct OperationArgument {
    std::string name;
    std::string type;
    std::string description;
    bool required = false;
    std::optional<Value> default_value;
    std::vector<Value> choices;
};
/** A peer-visible command with typed argument metadata. */
struct OperationDescriptor {
    std::string name;
    std::string display_name;
    std::string description;
    std::string category;
    std::vector<OperationArgument> arguments;
    std::string plugin_id;
    AllowedRoles allowed_roles = AllowedRoles::AdminOnly;
};
/** Encode one descriptor as a codec-independent TEMPEST object. */
Value::Object operationDescriptorValue(const OperationDescriptor& descriptor);

/** A peer-visible telemetry topic. Payload encoding remains the transport's concern. */
struct TelemetryDescriptor {
    std::string topic;
    std::string display_name;
    std::string description;
    std::string category;
    std::string direction;
    std::string delivery;
    std::string plugin_id;
};
/** Encode a telemetry descriptor as a codec-independent TEMPEST object. */
Value::Object telemetryDescriptorValue(const TelemetryDescriptor& descriptor);

/** Generic operations and telemetry over a Transport, with no DARTWIC, JSON or socket dependency. Call start after installing handlers; call stop before destroying handler state. Lifecycle calls must be serialized by the owner. Operation handlers execute on a request worker; telemetry and state callbacks execute on a separate callback worker and may call remote operations. */
class Peer {
public:
    using OperationHandler = std::function<Value::Object(const Value::Object&)>;
    using TelemetryHandler = std::function<void(const Value::Object&)>;
    Peer(PeerConfig config, TransportPtr transport);
    ~Peer();
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    /** Start transport and registration. This Peer cannot be restarted after stop. */
    void start();
    /** Cancel pending calls and callbacks, stop transport and join workers. */
    void stop();
    /** Register a unique operation. Return an object or throw RemoteError. The tempest/ namespace is reserved. */
    void registerOperation(OperationDescriptor descriptor, OperationHandler handler);
    void registerOperation(std::string name, OperationHandler handler);
    /** Descriptors for explicitly registered operations, sorted by name. */
    std::vector<OperationDescriptor> operations() const;
    void setOperationCatalogProvider(std::function<std::vector<OperationDescriptor>()> provider);
    /** Declare a topic that this peer can publish, including topics not yet emitted. */
    void registerTelemetry(TelemetryDescriptor descriptor);
    std::vector<TelemetryDescriptor> telemetryTopics() const;
    void setTelemetryCatalogProvider(std::function<std::vector<TelemetryDescriptor>()> provider);
    /** Handle operation names without an explicitly registered handler. */
    void setOperationFallback(std::function<Value::Object(const std::string&, const Value::Object&)> handler);
    /** Install or replace the handler for a topic. Delivery is best effort. */
    void onTelemetry(std::string topic, TelemetryHandler handler);
    /** Handle topics without a specific telemetry handler, including the topic name. */
    void setTelemetryFallback(std::function<void(const std::string&, const Value::Object&)> handler);
    /** Install the lifecycle callback before starting. */
    void onStateChanged(std::function<void(ConnectionState, std::string)> handler);
    /** Send one request and wait for its correlated response. Zero timeout uses request_timeout; never retries a submitted operation. */
    Value::Object call(std::string name, Value::Object arguments = {},
                       std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    /** Publish best-effort telemetry. Disconnection or enqueue failure increments droppedTelemetry. */
    void publish(std::string topic, Value::Object payload = {});
    /** Publish a validated complete snapshot through an optional adapter codec.
     * Returns false only when unsupported; accepted or dropped sends return true.
     */
    bool publishSerializedTelemetry(std::string topic, std::string encoding,
        std::shared_ptr<const std::string> payload);
    bool ready() const;
    /** Wait for registration or throw on timeout/stop. */
    void waitUntilReady(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) const;
    ConnectionState state() const;
    std::string nodeName() const;
    std::string sessionId() const;
    std::string remoteNode() const;
    std::string remoteSession() const;
    std::string peerId() const;
    uint64_t peerVersion() const;
    std::string protocolId() const;
    uint64_t protocolVersion() const;
    std::vector<TransportPath> transportDiagnostics() const;
    uint64_t droppedTelemetry() const;
    uint64_t operationTimeouts() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace TEMPEST
