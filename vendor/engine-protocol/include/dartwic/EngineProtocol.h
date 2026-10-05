#pragma once
#include <dartwic/EngineTypes.h>

namespace DARTWIC {
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char* ARGUS_UPDATE_EVENT_STATUS_OPERATION = "tempest-peer/argus/update-event-status";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char* ARGUS_RESPOND_TO_PROMPT_OPERATION = "tempest-peer/argus/respond-to-prompt";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char* ARGUS_RELEASE_HOLD_OPERATION = "tempest-peer/argus/release-hold";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char* ARGUS_DELETE_EVENT_OPERATION = "tempest-peer/argus/delete-event";


// DARTWIC's operation/topic contract, carried directly by TEMPEST. These
// functions describe application payloads; they do not serialize wire bytes.
namespace EngineContract {
/** DARTWIC application contract version. */
inline constexpr int version = 1;
/** Map a typed request to its TEMPEST operation name. */
std::string operation(const EngineRequest& request);
/** Map a typed request to its argument object. */
Value::Object arguments(const EngineRequest& request);
/** Validate and parse a TEMPEST operation argument object. */
EngineRequest request(const std::string& operation, const Value::Object& arguments);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ChannelQueryResult& result);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ArgusQueryResult& result);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ArgusActionResult& result);
/** Parse a channel snapshot response. */
ChannelQueryResult channelResult(const Value::Object& result);
/** Parse an ARGUS query response. */
ArgusQueryResult argusResult(const Value::Object& result);
/** Parse an ARGUS action response. */
ArgusActionResult actionResult(const Value::Object& result);
/** Map typed telemetry to its TEMPEST topic. */
std::string topic(const EngineTelemetry& telemetry);
/** Map typed telemetry to its application object. */
Value::Object payload(const EngineTelemetry& telemetry);
/** Parse a known topic and its application object. */
EngineTelemetry telemetry(const std::string& topic, const Value::Object& payload);
}

/** Typed channel and ARGUS operations composed over an existing Peer. Construct before Peer.start; Peer owns transport and lifecycle. Operations wait for completion; telemetry is best effort. */
class EngineProtocol {
public:
    explicit EngineProtocol(TEMPEST::Peer& peer);
    ~EngineProtocol();
    EngineProtocol(const EngineProtocol&) = delete;
    EngineProtocol& operator=(const EngineProtocol&) = delete;

    /** Channel commands, snapshot queries and telemetry. Publish reports state without acknowledgement; upsert/remove/upsertBulk wait for the remote handler. */
    class Channels {
    public:
        void upsert(ChannelUpsert request) const;
        void remove(ChannelRemove request) const;
        void upsertBulk(ChannelBulkUpsert request) const;
        ChannelQueryResult query(ChannelQuery request = {}) const;
        void publishTelemetry(ChannelTelemetry telemetry) const;
        void publishTelemetry(FixedGenerationTelemetry telemetry) const;
        void setHandlers(ChannelHandlers handlers) const;
    private:
        friend class EngineProtocol;
        explicit Channels(EngineProtocol* owner) : owner_(owner) {}
        EngineProtocol* owner_;
    };
    /** Event publication, queries and owner-directed actions. Prompt callbacks run on the operation worker when a response arrives. */
    class Argus {
    public:
        void message(ArgusEvent event) const;
        void warning(ArgusEvent event) const;
        void error(ArgusEvent event) const;
        void abort(ArgusEvent event) const;
        void hold(ArgusEvent event) const;
        std::string prompt(ArgusEvent event, std::function<void(const ArgusAction&)> response = {}) const;
        void respond(std::string event_id, /** Map typed telemetry to its application object. */
Value::Object payload) const;
        void updateStatus(std::string event_id, std::string status) const;
        void releaseHold(std::string event_id, /** Map typed telemetry to its application object. */
Value::Object payload = {}) const;
        void remove(std::string event_id) const;
        ArgusQueryResult query(ArgusQuery request = {}) const;
        void publishTelemetry(ArgusEventTelemetry telemetry) const;
        void setHandlers(ArgusHandlers handlers) const;
    private:
        friend class EngineProtocol;
        explicit Argus(EngineProtocol* owner) : owner_(owner) {}
        void publishTyped(const char* type, ArgusEvent event) const;
        EngineProtocol* owner_;
    };
    /** Query and publish named text streams. The receiver persists telemetry locally. */
    class Logs {
    public:
        /** Queries bounded log history from the peer, including optional UTC time filters. */
        std::vector<LogEntry> query(LogQuery request = {}) const;
        /** Lists the peer's available node/session/stream combinations. */
        std::vector<LogStream> list() const;
        /** Publishes one timestamped log entry to the peer. */
        void publish(LogEntry entry) const;
        /** Publishes a batch of timestamped log entries to the peer. */
        void publishBatch(std::vector<LogEntry> entries) const;
        void setHandlers(LogHandlers handlers) const;
    private:
        friend class EngineProtocol;
        explicit Logs(EngineProtocol* owner) : owner_(owner) {}
        EngineProtocol* owner_;
    };
    Channels& channels() { return channels_; }
    const Channels& channels() const { return channels_; }
    Argus& argus() { return argus_; }
    const Argus& argus() const { return argus_; }
    Logs& logs() { return logs_; }
    const Logs& logs() const { return logs_; }
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    Channels channels_{this};
    Argus argus_{this};
    Logs logs_{this};
};
} // namespace DARTWIC
