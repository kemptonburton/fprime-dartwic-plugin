#pragma once

#include <tempest/Value.h>
#include <functional>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace TEMPEST {

/** Peer lifecycle state; Connected means registration completed. */
enum class ConnectionState { Disconnected, Connecting, Connected, Reconnecting, Stopping };

/** Operations, replies and telemetry share this in-memory envelope. A transport chooses its encoding. The deadline is local monotonic time and must not be transmitted as a remote clock value. */
struct Message {
    enum class Kind { Request, Response, Telemetry };
    Kind kind = Kind::Request;
    std::string request_id;
    std::string name;
    Value::Object payload;
    bool error = false;
    std::string error_code;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

/** Delivery callbacks supplied by Peer. Transport control and response correlation must remain independent of application handlers. */
struct TransportCallbacks {
    std::function<void(Message)> on_message;
    std::function<void(ConnectionState, std::string)> on_state;
};

/** One observed transport path. A shared socket is reported once by its owner. */
struct TransportPath {
    std::string id;
    std::string role;
    std::string endpoint;
    std::string state = "unknown";
    bool shared = false;
    uint64_t last_receive_ms = 0;
    uint64_t last_send_ms = 0;
    std::size_t queued = 0;
    uint64_t dropped = 0;
    std::string last_error;
    std::string transport = "unknown";
    // Cumulative payload bytes accepted by this transport's physical I/O.
    // Shared paths should not be summed into an individual peer's rate.
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    bool byte_counters_available = false;
};

/** Implement start, send and stop for custom framing and I/O. Peer owns registration, correlation, deadlines, heartbeats and application dispatch. Serialize access to owned sockets on the I/O worker. Never replay accepted commands after reconnect. */
class Transport {
public:
    virtual ~Transport() = default;
    /** Install callbacks and begin delivery. Report underlying link availability with on_state. */
    virtual void start(TransportCallbacks callbacks) = 0;
    /** Nonblocking bounded enqueue; throw on failed operation enqueue. Best-effort telemetry may be dropped. Honor the original local deadline for unsent messages. */
    virtual void send(Message message) = 0;
    /** Release callbacks and stop delivery before returning. */
    virtual void stop() = 0;
    /** Return an observational snapshot; unknown fields remain unset. */
    virtual std::vector<TransportPath> diagnostics() const { return {}; }
};
/** Shared ownership of an application transport. */
using TransportPtr = std::shared_ptr<Transport>;

/** Optional adapter optimization for complete, replaceable telemetry snapshots.
 * The caller supplies a validated payload in the named encoding. Unsupported
 * adapters return false so the caller can use the ordinary typed send path.
 * This separate interface leaves custom Transport implementations unchanged.
 */
class SerializedTelemetryTransport {
public:
    virtual ~SerializedTelemetryTransport() = default;
    virtual bool sendSerializedTelemetry(std::string topic, std::string encoding,
        std::shared_ptr<const std::string> payload) = 0;
};

} // namespace TEMPEST
