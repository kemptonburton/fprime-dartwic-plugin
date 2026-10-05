#pragma once
#include <tempest/Peer.h>
#include <optional>
#include <variant>
#include <vector>

namespace DARTWIC {
/** Application value type shared with TEMPEST; no wire codec is implied. */
using Value = TEMPEST::Value;
/** One channel value with its RAPID timestamp in nanoseconds. */
struct ChannelSample { Value value; uint64_t timestamp = 0; };
/** Channel field write or telemetry update, including optional snapshot data and owner/session/revision lineage. */
struct ChannelUpsert {
    std::string owner_node;
    std::string channel;
    std::string field = "value";
    Value value;
    std::optional<uint64_t> timestamp;
    std::vector<ChannelSample> samples;
    Value::Object channel_data;
    std::string command_origin;
    uint64_t revision = 0;
    std::string source_session_id;
    bool take_manual_control = false;
    bool release_manual_override = false;
};
/** Channel deletion identified by owner and channel, with optional session/revision lineage. */
struct ChannelRemove {
    std::string owner_node;
    std::string channel;
    uint64_t revision = 0;
    std::string source_session_id;
};
/** Batch of timestamped samples for one channel. A command requires at least one sample. */
struct ChannelBulkUpsert {
    std::string owner_node;
    std::string channel;
    std::vector<ChannelSample> samples;
    std::string command_origin;
    uint64_t revision = 0;
    std::string source_session_id;
};
/** Channel keys and requested fields for a synchronous peer snapshot query. */
struct ChannelQuery { std::vector<std::string> channels; std::vector<std::string> fields; };
/** One channel returned by a query, with its data and owner/session/revision lineage. */
struct ChannelSnapshot {
    std::string owner_node;
    std::string channel;
    Value::Object channel_data;
    uint64_t revision = 0;
    std::string source_session_id;
};
/** Channel snapshots returned by a peer query handler. */
struct ChannelQueryResult { std::vector<ChannelSnapshot> channels; };
/** One field write within a fixed-generation telemetry batch. */
struct FixedGenerationWrite {
    std::string channel;
    std::string field = "value";
    Value value;
    std::optional<uint64_t> timestamp;
    uint64_t revision = 0;
};
/** Writes belonging to one source generation, with a source timestamp and relay route. */
struct FixedGenerationTelemetry {
    std::string owner_node;
    uint64_t generation = 0;
    uint64_t source_timestamp = 0;
    std::vector<FixedGenerationWrite> writes;
    std::vector<std::string> route;
};
/** One-way channel upsert, removal, or bulk update. Kind selects the corresponding payload member. */
struct ChannelTelemetry {
    enum class Kind { Upsert, Remove, BulkUpsert };
    Kind kind = Kind::Upsert;
    ChannelUpsert upsert;
    ChannelRemove remove;
    ChannelBulkUpsert bulk;
    std::vector<std::string> route;
};
/** Complete current channel records sent by one engine peer. */
struct ChannelSnapshotTelemetry {
    std::string owner_node;
    Value::Object channels;
    std::vector<std::string> route;
};

/** ARGUS event identity, presentation, ownership, channel references, and details. The engine interprets timestamp as Unix nanoseconds when created_at_ns is absent. */
struct ArgusEvent {
    std::string event_id;
    std::string type = "message";
    std::string title;
    std::string description;
    std::string status = "new";
    std::string owner_node;
    uint64_t timestamp = 0;
    std::vector<std::string> channels;
    Value::Object details;
};
/** ARGUS event selection by identity, status, type, owner, time range, limit, and additional filters. */
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
struct ArgusQueryResult { std::vector<ArgusEvent> events; size_t count = 0; size_t total_count = 0; };
/** Action on an ARGUS event. Kind selects a built-in action or a custom operation and payload. */
struct ArgusAction {
    enum class Kind { Respond, UpdateStatus, ReleaseHold, Delete, Custom };
    Kind kind = Kind::Custom;
    std::string event_id;
    std::string operation;
    Value::Object payload;
};
/** Event identity and result fields returned by an ARGUS action handler. */
struct ArgusActionResult { std::string event_id; Value::Object result; };
/** One-way ARGUS event creation, update, or removal, with ownership and relay route. */
struct ArgusEventTelemetry {
    enum class Kind { Created, Updated, Removed };
    Kind kind = Kind::Created;
    std::string owner_node;
    ArgusEvent event;
    std::vector<std::string> route;
};

/** Application handlers for incoming channel commands, queries, and telemetry. Install before Peer.start so initial synchronization can be served. */
struct ChannelHandlers {
    std::function<void(const ChannelUpsert&)> upsert;
    std::function<void(const ChannelRemove&)> remove;
    std::function<void(const ChannelBulkUpsert&)> bulk_upsert;
    std::function<ChannelQueryResult(const ChannelQuery&)> query;
    std::function<void(const ChannelTelemetry&)> telemetry;
    std::function<void(const ChannelSnapshotTelemetry&)> snapshot;
    std::function<void(const FixedGenerationTelemetry&)> fixed_generation;
};

/** Application handlers for incoming ARGUS queries, actions, and telemetry. Install before Peer.start so initial synchronization can be served. */
struct ArgusHandlers {
    std::function<ArgusQueryResult(const ArgusQuery&)> query;
    std::function<ArgusActionResult(const ArgusAction&)> action;
    std::function<void(const ArgusEventTelemetry&)> telemetry;
};

/** Text output from one process stream. Sequence is scoped to node/session/stream. */
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
/** Selects one node, process session, and stream, with optional sequence, text, channel, level, and UTC nanosecond time filters. */
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
struct LogStream { std::string owner_node; std::string session; std::string stream; };
/** Callbacks through which an EngineProtocol peer serves log history and receives live logs. */
struct LogHandlers {
    std::function<std::vector<LogEntry>(const LogQuery&)> query;
    std::function<void(const LogEntry&)> telemetry;
    std::function<std::vector<LogStream>()> list;
};


/** Requests supported by the DARTWIC application contract. */
using EngineRequest = std::variant<ChannelUpsert, ChannelRemove, ChannelBulkUpsert, ChannelQuery, ArgusQuery, ArgusAction>;
/** Telemetry supported by the DARTWIC application contract. */
using EngineTelemetry = std::variant<ChannelTelemetry, ChannelSnapshotTelemetry,
    FixedGenerationTelemetry, ArgusEventTelemetry>;
} // namespace DARTWIC
