#pragma once
#include <dartwic/EngineTypes.h>
#include <dartwic/InternalVersions.h>

namespace DARTWIC {
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char *ARGUS_UPDATE_EVENT_STATUS_OPERATION =
    "tempest-peer/argus/update-event-status";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char *ARGUS_RESPOND_TO_PROMPT_OPERATION =
    "tempest-peer/argus/respond-to-prompt";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char *ARGUS_RELEASE_HOLD_OPERATION =
    "tempest-peer/argus/release-hold";
/** Engine operation name for owner-directed ARGUS actions. */
inline constexpr const char *ARGUS_DELETE_EVENT_OPERATION =
    "tempest-peer/argus/delete-event";

// DARTWIC's operation/topic contract, carried directly by TEMPEST. These
// functions describe application payloads; they do not serialize wire bytes.
namespace EngineContract {
/** TEMPEST compatibility version supplied by the SDK. */
inline constexpr int version = TEMPEST_COMPATIBILITY_VERSION;
/** Encode supplied channel fields without inventing omitted values. */
Value::Object fields(const ChannelFields &fields);
/** Decode channel fields, retaining read-only observation attributes
 * separately. */
ChannelFields fields(const Value::Object &fields);
/** Map a typed request to its TEMPEST operation name. */
std::string operation(const EngineRequest &request);
/** Map a typed request to its argument object. */
Value::Object arguments(const EngineRequest &request);
/** Validate and parse a TEMPEST operation argument object. */
EngineRequest request(const std::string &operation,
                      const Value::Object &arguments);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ChannelQueryResult &result);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ArgusQueryResult &result);
/** Map a typed result to its TEMPEST response object. */
Value::Object result(const ArgusActionResult &result);
/** Parse a channel snapshot response. */
ChannelQueryResult channelResult(const Value::Object &result);
/** Parse an ARGUS query response. */
ArgusQueryResult argusResult(const Value::Object &result);
/** Parse an ARGUS action response. */
ArgusActionResult actionResult(const Value::Object &result);
/** Map typed telemetry to its TEMPEST topic. */
std::string topic(const EngineTelemetry &telemetry);
/** Map typed telemetry to its application object. */
Value::Object payload(const EngineTelemetry &telemetry);
/** Parse a known topic and its application object. */
EngineTelemetry telemetry(const std::string &topic,
                          const Value::Object &payload);
} // namespace EngineContract

/** Typed application contract over Peer. No application storage is allocated.
 * Commands wait; telemetry is best effort. */
class EngineProtocol {
public:
  explicit EngineProtocol(TEMPEST::Peer &peer);
  ~EngineProtocol();
  EngineProtocol(const EngineProtocol &) = delete;
  EngineProtocol &operator=(const EngineProtocol &) = delete;
  class Channels {
  public:
    void upsert(std::string channel, ChannelFields fields) const;
    void upsert(ChannelChanges changes) const;
    void upsert(ChannelUpsertRequest request) const;
    void remove(std::string channel) const;
    void remove(ChannelRemoveRequest request) const;
    ChannelQueryResult query(ChannelQuery request = {}) const;
    ChannelQueryResult query(std::initializer_list<std::string> channels) const;
    void publishChannelTelemetry(ChannelUpdate update) const;
    void publishChannelTelemetry(ChannelUpdates updates) const;
    void publishSnapshotTelemetry(ChannelSnapshot snapshot) const;
    void setHandlers(ChannelHandlers handlers) const;

  private:
    friend class EngineProtocol;
    explicit Channels(EngineProtocol *owner) : owner_(owner) {}
    EngineProtocol *owner_;
  };
  class Argus {
  public:
    std::string publishEventTelemetry(ArgusEvent event) const;
    void publishSnapshotTelemetry(ArgusBoardSnapshot snapshot) const;
    ArgusQueryResult query(ArgusQuery request = {}) const;
    ArgusActionResult updateStatus(std::string event_id,
                                   std::string status) const;
    ArgusActionResult respondToPrompt(std::string event_id,
                                      Value::Object response) const;
    ArgusActionResult releaseHold(std::string event_id,
                                  Value::Object response = {}) const;
    ArgusActionResult remove(std::string event_id) const;
    void setHandlers(ArgusHandlers handlers) const;

  private:
    friend class EngineProtocol;
    explicit Argus(EngineProtocol *owner) : owner_(owner) {}
    EngineProtocol *owner_;
  };
  class Logs {
  public:
    std::vector<LogEntry> query(LogQuery request = {}) const;
    std::vector<LogStream> list() const;
    void publishLogTelemetry(LogEntry entry) const;
    void publishLogTelemetry(std::vector<LogEntry> entries) const;
    void setHandlers(LogHandlers handlers) const;

  private:
    friend class EngineProtocol;
    explicit Logs(EngineProtocol *owner) : owner_(owner) {}
    EngineProtocol *owner_;
  };
  Channels &channels() { return channels_; }
  const Channels &channels() const { return channels_; }
  Argus &argus() { return argus_; }
  const Argus &argus() const { return argus_; }
  Logs &logs() { return logs_; }
  const Logs &logs() const { return logs_; }

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  Channels channels_{this};
  Argus argus_{this};
  Logs logs_{this};
};
} // namespace DARTWIC
