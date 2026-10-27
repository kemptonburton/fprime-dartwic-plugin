#pragma once
#include <limits>
#include <optional>
#include <tempest/Peer.h>
#include <variant>
#include <vector>

namespace DARTWIC {
/** Application values shared with TEMPEST, independent of wire encoding. */
using Value = TEMPEST::Value;
/** Optional fields; omitted values leave existing data unchanged. Extra
 * observation fields are read-only. */
struct ChannelFields {
  std::optional<Value> value;
  std::optional<uint64_t> timestamp;
  std::optional<std::string> units;
  std::optional<double> stale_timeout;
  std::optional<std::string> record_mode;
  std::optional<Value> calculation_scripts;
  std::optional<Value> value_options;
  std::optional<Value> startup_value;
  Value::Object extra;
};
/** A requested change to one channel. Identity and authority are not writable
 * fields. */
struct ChannelChange {
  std::string channel;
  ChannelFields fields;
};
/** One or more requested channel field changes. */
using ChannelChanges = std::vector<ChannelChange>;
/** Owner-directed batch; execution is synchronous but not atomic. Engine
 * authority context is separate from writable fields. */
struct ChannelUpsertRequest {
  ChannelChanges changes;
  std::string command_origin;
  std::string automation_operation;
  bool take_manual_control = false;
  bool release_manual_override = false;
};
/** Owner-directed deletion of one channel. */
struct ChannelRemoveRequest {
  std::string channel;
  std::string command_origin;
};
/** Current channel names and optional field selection; empty selections mean
 * all. */
struct ChannelQuery {
  std::vector<std::string> channels;
  std::vector<std::string> fields;
};
/** An observed record; local publications fill missing source lineage
 * automatically. */
struct ChannelUpdate {
  std::string channel;
  ChannelFields fields;
  std::string owner_node;
  uint64_t revision = 0;
  std::string source_session_id;
  std::vector<std::string> route;
};
/** Observed channel records, including repeated timestamped samples. */
using ChannelUpdates = std::vector<ChannelUpdate>;
/** Matching current channel records returned by a synchronous query. */
struct ChannelQueryResult {
  ChannelUpdates channels;
};
/** Complete shared selection; an empty selection clears the receiver's mirror.
 */
struct ChannelSnapshot {
  ChannelUpdates channels;
  std::string owner_node;
};
/** The six supported ARGUS presentation and workflow types. */
enum class EventType { Message, Warning, Error, Abort, Hold, Prompt };
/** Return the lowercase wire name for an event type. */
std::string eventTypeName(EventType type);
/** Parse a supported lowercase event type or throw on invalid input. */
EventType eventTypeFromName(const std::string &name);
/** ARGUS event identity, presentation, ownership, channel references, and
 * details. The engine interprets timestamp as Unix nanoseconds when
 * created_at_ns is absent. */
struct ArgusEvent {
  std::string event_id;
  EventType type = EventType::Message;
  std::string title;
  std::string description;
  std::string status = "new";
  std::string owner_node;
  uint64_t timestamp = 0;
  std::vector<std::string> channels;
  Value::Object details;
  double separation_seconds = std::numeric_limits<double>::quiet_NaN();
};
/** ARGUS event selection by identity, status, type, owner, time range, limit,
 * and additional filters. */
struct ArgusQuery {
  std::string event_id;
  std::string status;
  std::string type;
  std::string owner_node;
  uint64_t since = 0;
  uint64_t until = 0;
  size_t limit = 0;
  Value::Object filters;
};
/** ARGUS events returned by a peer, with result and total counts. */
struct ArgusQueryResult {
  std::vector<ArgusEvent> events;
  size_t count = 0;
  size_t total_count = 0;
};
/** Action on an ARGUS event. Kind selects a built-in action or a custom
 * operation and payload. */
struct ArgusAction {
  enum class Kind { Respond, UpdateStatus, ReleaseHold, Delete, Custom };
  Kind kind = Kind::Custom;
  std::string event_id;
  std::string operation;
  Value::Object payload;
};
/** Event identity and result fields returned by an ARGUS action handler. */
struct ArgusActionResult {
  std::string event_id;
  Value::Object result;
};
/** Text output from one process stream. Sequence is scoped to
 * node/session/stream. */
struct LogEntry {
  std::string owner_node;
  std::string session;
  std::string stream;
  std::string channel = "stdout";
  std::string level = "info";
  std::string text;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};
/** Selects one node, process session, and stream, with optional sequence, text,
 * channel, level, and UTC nanosecond time filters. */
struct LogQuery {
  std::string owner_node;
  std::string session;
  std::string stream;
  std::string text;
  uint64_t before = 0;
  uint64_t after = 0;
  size_t limit = 250;
  std::string channel;
  std::string level;
  uint64_t from_ns = 0;
  uint64_t to_ns = 0;
};
/** Identifies one originating node, process session, and named log source. */
struct LogStream {
  std::string owner_node;
  std::string session;
  std::string stream;
};

/** Complete current event board. */
struct ArgusBoardSnapshot {
  std::vector<ArgusEvent> events;
  std::string owner_node;
};
/** Install before Peer.start. Commands run on the request worker; telemetry
 * uses the callback worker. */
struct ChannelHandlers {
  std::function<void(const ChannelUpsertRequest &)> upsertCommand;
  std::function<ChannelQueryResult(const ChannelQuery &)> queryCommand;
  std::function<void(const ChannelRemoveRequest &)> removeCommand;
  std::function<void(const ChannelUpdates &)> channelTelemetry;
  std::function<void(const ChannelSnapshot &)> snapshotTelemetry;
};
/** Owner-side event commands and incoming event/complete-board telemetry. */
struct ArgusHandlers {
  std::function<ArgusQueryResult(const ArgusQuery &)> queryCommand;
  std::function<ArgusActionResult(const std::string &, const std::string &)>
      updateStatusCommand;
  std::function<ArgusActionResult(const std::string &, const Value::Object &)>
      respondToPromptCommand;
  std::function<ArgusActionResult(const std::string &, const Value::Object &)>
      releaseHoldCommand;
  std::function<ArgusActionResult(const std::string &)> removeCommand;
  std::function<void(const ArgusEvent &)> eventTelemetry;
  std::function<void(const ArgusBoardSnapshot &)> snapshotTelemetry;
};
/** Supported log-history commands and incoming batches of entries. */
struct LogHandlers {
  std::function<std::vector<LogEntry>(const LogQuery &)> queryCommand;
  std::function<void(const std::vector<LogEntry> &)> logTelemetry;
  std::function<std::vector<LogStream>()> listCommand;
};
/** Typed synchronous requests carried by the shared Engine contract. */
using EngineRequest = std::variant<ChannelUpsertRequest, ChannelRemoveRequest,
                                   ChannelQuery, ArgusQuery, ArgusAction>;
/** Typed best-effort observations and complete snapshots. */
using EngineTelemetry = std::variant<ChannelUpdates, ChannelSnapshot,
                                     ArgusEvent, ArgusBoardSnapshot>;
} // namespace DARTWIC
