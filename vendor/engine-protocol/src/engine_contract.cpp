#include <cmath>
#include <dartwic/EngineProtocol.h>
#include <limits>

namespace DARTWIC::EngineContract {
namespace {
using Object = Value::Object;
using Array = Value::Array;
const Value *find(const Object &o, const char *key) {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
std::string text(const Object &o, const char *key, std::string fallback = {}) {
  auto v = find(o, key);
  return v ? v->string() : std::move(fallback);
}
uint64_t number(const Object &o, const char *key, uint64_t fallback = 0) {
  auto v = find(o, key);
  if (!v)
    return fallback;
  if (auto n = std::get_if<uint64_t>(&v->storage()))
    return *n;
  if (auto n = std::get_if<int64_t>(&v->storage()); n && *n >= 0)
    return static_cast<uint64_t>(*n);
  throw TEMPEST::RemoteError(std::string(key) +
                                 " must be a nonnegative integer.",
                             "invalid_arguments");
}
Object object(const Object &o, const char *key) {
  auto v = find(o, key);
  return v ? v->object() : Object{};
}
Array array(const Object &o, const char *key) {
  auto v = find(o, key);
  return v ? v->array() : Array{};
}
Array strings(const std::vector<std::string> &values) {
  Array a;
  for (const auto &v : values)
    a.emplace_back(v);
  return a;
}
std::vector<std::string> strings(const Object &o, const char *key) {
  std::vector<std::string> a;
  for (const auto &v : array(o, key))
    a.push_back(v.string());
  return a;
}
Object event(const ArgusEvent &e) {
  return {
      {"event_id", e.event_id},   {"type", eventTypeName(e.type)},
      {"title", e.title},         {"description", e.description},
      {"status", e.status},       {"owner_node", e.owner_node},
      {"timestamp", e.timestamp}, {"channels", strings(e.channels)},
      {"details", e.details},     {"separation_seconds", e.separation_seconds}};
}
ArgusEvent event(const Object &o) {
  ArgusEvent result{
      text(o, "event_id"),      eventTypeFromName(text(o, "type", "message")),
      text(o, "title"),         text(o, "description"),
      text(o, "status", "new"), text(o, "owner_node"),
      number(o, "timestamp"),   strings(o, "channels"),
      object(o, "details")};
  if (const auto *value = find(o, "separation_seconds")) {
    const auto &storage = value->storage();
    if (const auto *real = std::get_if<double>(&storage))
      result.separation_seconds = *real;
    else if (const auto *integer = std::get_if<int64_t>(&storage))
      result.separation_seconds = static_cast<double>(*integer);
    else if (const auto *integer = std::get_if<uint64_t>(&storage))
      result.separation_seconds = static_cast<double>(*integer);
    else
      throw TEMPEST::RemoteError("separation_seconds must be numeric.",
                                 "invalid_arguments");
  }
  if (!std::isfinite(result.separation_seconds) ||
      result.separation_seconds < 0)
    throw TEMPEST::RemoteError(
        "separation_seconds must be finite and nonnegative.",
        "invalid_arguments");
  return result;
}
} // namespace
Object fields(const ChannelFields &f) {
  Object o = f.extra;
  if (f.value)
    o["value"] = *f.value;
  if (f.timestamp)
    o["timestamp"] = *f.timestamp;
  if (f.units)
    o["units"] = *f.units;
  if (f.stale_timeout)
    o["stale_timeout"] = *f.stale_timeout;
  if (f.record_mode)
    o["record_mode"] = *f.record_mode;
  if (f.calculation_scripts)
    o["calculation_scripts"] = *f.calculation_scripts;
  if (f.value_options)
    o["value_options"] = *f.value_options;
  if (f.startup_value)
    o["startup_value"] = *f.startup_value;
  return o;
}
ChannelFields fields(const Object &o) {
  ChannelFields f;
  f.extra = o;
  if (auto v = find(o, "value"))
    f.value = *v;
  if (find(o, "timestamp"))
    f.timestamp = number(o, "timestamp");
  if (find(o, "units"))
    f.units = text(o, "units");
  if (auto v = find(o, "stale_timeout"))
    std::visit(
        [&](const auto &n) {
          if constexpr (std::is_arithmetic_v<std::decay_t<decltype(n)>>)
            f.stale_timeout = static_cast<double>(n);
          else
            throw TEMPEST::Error("stale_timeout must be numeric");
        },
        v->storage());
  if (find(o, "record_mode"))
    f.record_mode = text(o, "record_mode");
  if (auto v = find(o, "calculation_scripts"))
    f.calculation_scripts = *v;
  if (auto v = find(o, "value_options"))
    f.value_options = *v;
  if (auto v = find(o, "startup_value"))
    f.startup_value = *v;
  for (auto key :
       {"value", "timestamp", "units", "stale_timeout", "record_mode",
        "calculation_scripts", "value_options", "startup_value"})
    f.extra.erase(key);
  return f;
}
std::string operation(const EngineRequest &r) {
  return std::visit(
      [](const auto &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ChannelUpsertRequest>)
          return "tempest-peer/channels/upsert";
        else if constexpr (std::is_same_v<T, ChannelRemoveRequest>)
          return "tempest-peer/channels/remove";
        else if constexpr (std::is_same_v<T, ChannelQuery>)
          return "tempest-peer/channels/query";
        else if constexpr (std::is_same_v<T, ArgusQuery>)
          return "tempest-peer/argus/query";
        else {
          switch (v.kind) {
          case ArgusAction::Kind::UpdateStatus:
            return ARGUS_UPDATE_EVENT_STATUS_OPERATION;
          case ArgusAction::Kind::Respond:
            return ARGUS_RESPOND_TO_PROMPT_OPERATION;
          case ArgusAction::Kind::ReleaseHold:
            return ARGUS_RELEASE_HOLD_OPERATION;
          case ArgusAction::Kind::Delete:
            return ARGUS_DELETE_EVENT_OPERATION;
          default:
            throw TEMPEST::Error("Unsupported event action");
          }
        }
      },
      r);
}
Object arguments(const EngineRequest &r) {
  return std::visit(
      [](const auto &v) -> Object {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ChannelUpsertRequest>) {
          Array changes;
          for (const auto &c : v.changes) {
            if (c.channel.empty() || !c.fields.extra.empty() ||
                (c.fields.timestamp && !c.fields.value))
              throw TEMPEST::RemoteError("Invalid channel change",
                                         "invalid_arguments");
            changes.emplace_back(
                Object{{"channel", c.channel}, {"fields", fields(c.fields)}});
          }
          if (changes.empty())
            throw TEMPEST::RemoteError("Upsert needs changes",
                                       "invalid_arguments");
          return {{"changes", changes},
                  {"command_origin", v.command_origin},
                  {"automation_operation", v.automation_operation},
                  {"take_manual_control", v.take_manual_control},
                  {"release_manual_override", v.release_manual_override}};
        } else if constexpr (std::is_same_v<T, ChannelRemoveRequest>)
          return {{"channel", v.channel}, {"command_origin", v.command_origin}};
        else if constexpr (std::is_same_v<T, ChannelQuery>)
          return {{"channels", strings(v.channels)},
                  {"fields", strings(v.fields)}};
        else if constexpr (std::is_same_v<T, ArgusQuery>)
          return {{"event_id", v.event_id},
                  {"status", v.status},
                  {"type", v.type},
                  {"owner_node", v.owner_node},
                  {"since", v.since},
                  {"until", v.until},
                  {"limit", static_cast<uint64_t>(v.limit)},
                  {"filters", v.filters}};
        else
          return {{"event_id", v.event_id}, {"payload", v.payload}};
      },
      r);
}
EngineRequest request(const std::string &name, const Object &o) {
  if (name == "tempest-peer/channels/upsert") {
    ChannelUpsertRequest r;
    for (const auto &c : array(o, "changes"))
      r.changes.push_back(
          {text(c.object(), "channel"), fields(object(c.object(), "fields"))});
    r.command_origin = text(o, "command_origin");
    r.automation_operation = text(o, "automation_operation");
    if (auto v = find(o, "take_manual_control"))
      r.take_manual_control = std::get<bool>(v->storage());
    if (auto v = find(o, "release_manual_override"))
      r.release_manual_override = std::get<bool>(v->storage());
    arguments(r);
    return r;
  }
  if (name == "tempest-peer/channels/remove") {
    if (text(o, "channel").empty())
      throw TEMPEST::RemoteError("Channel required", "invalid_arguments");
    return ChannelRemoveRequest{text(o, "channel"), text(o, "command_origin")};
  }
  if (name == "tempest-peer/channels/query")
    return ChannelQuery{strings(o, "channels"), strings(o, "fields")};
  if (name == "tempest-peer/argus/query")
    return ArgusQuery{text(o, "event_id"),
                      text(o, "status"),
                      text(o, "type"),
                      text(o, "owner_node"),
                      number(o, "since"),
                      number(o, "until"),
                      static_cast<size_t>(number(o, "limit")),
                      object(o, "filters")};
  for (auto kind :
       {ArgusAction::Kind::UpdateStatus, ArgusAction::Kind::Respond,
        ArgusAction::Kind::ReleaseHold, ArgusAction::Kind::Delete}) {
    ArgusAction a{kind, text(o, "event_id"), {}, object(o, "payload")};
    if (operation(a) == name) {
      if (a.event_id.empty())
        throw TEMPEST::RemoteError("Event ID required", "invalid_arguments");
      return a;
    }
  }
  throw TEMPEST::RemoteError("Unknown Engine operation: " + name,
                             "unsupported");
}

Object result(const ChannelQueryResult &r) {
  Array a;
  for (const auto &c : r.channels)
    a.emplace_back(Object{{"channel", c.channel},
                          {"fields", fields(c.fields)},
                          {"owner_node", c.owner_node},
                          {"revision", c.revision},
                          {"source_session_id", c.source_session_id}});
  return {{"channels", a}};
}
ChannelQueryResult channelResult(const Object &o) {
  ChannelQueryResult r;
  for (const auto &v : array(o, "channels")) {
    const auto &c = v.object();
    r.channels.push_back({text(c, "channel"), fields(object(c, "fields")),
                          text(c, "owner_node"), number(c, "revision"),
                          text(c, "source_session_id"), strings(c, "route")});
  }
  return r;
}
Object result(const ArgusQueryResult &r) {
  Array a;
  for (const auto &e : r.events)
    a.emplace_back(event(e));
  return {{"events", a},
          {"count", static_cast<uint64_t>(r.count)},
          {"total_count", static_cast<uint64_t>(r.total_count)}};
}
ArgusQueryResult argusResult(const Object &o) {
  ArgusQueryResult r;
  for (const auto &e : array(o, "events"))
    r.events.push_back(event(e.object()));
  r.count = number(o, "count", r.events.size());
  r.total_count = number(o, "total_count", r.count);
  return r;
}
Object result(const ArgusActionResult &r) {
  return {{"event_id", r.event_id}, {"result", r.result}};
}
ArgusActionResult actionResult(const Object &o) {
  return {text(o, "event_id"), object(o, "result")};
}
std::string topic(const EngineTelemetry &t) {
  return std::visit(
      [](const auto &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ChannelUpdates>)
          return "tempest-peer/channels/updated";
        if constexpr (std::is_same_v<T, ChannelSnapshot>)
          return "tempest-peer/channels/snapshot";
        if constexpr (std::is_same_v<T, ArgusEvent>)
          return "tempest-peer/argus/events";
        return "tempest-peer/argus/board-snapshot";
      },
      t);
}
Object payload(const EngineTelemetry &t) {
  return std::visit(
      [](const auto &v) -> Object {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ChannelUpdates>)
          return result(ChannelQueryResult{v});
        else if constexpr (std::is_same_v<T, ChannelSnapshot>) {
          Object records;
          for (const auto &c : v.channels)
            records[c.channel] = fields(c.fields);
          return {{"channels", records},
                  {"owner_node", v.owner_node},
                  {"complete", true}};
        } else if constexpr (std::is_same_v<T, ArgusEvent>)
          return event(v);
        else {
          Array a;
          for (const auto &e : v.events)
            a.emplace_back(event(e));
          return {
              {"events", a}, {"owner_node", v.owner_node}, {"complete", true}};
        }
      },
      t);
}
EngineTelemetry telemetry(const std::string &name, const Object &o) {
  if (name == "tempest-peer/channels/updated")
    return channelResult(o).channels;
  if (name == "tempest-peer/channels/snapshot") {
    if (!find(o, "complete") || !std::get<bool>(o.at("complete").storage()) ||
        !find(o, "channels"))
      throw TEMPEST::Error("Snapshot must be complete");
    ChannelSnapshot s;
    s.owner_node = text(o, "owner_node");
    for (const auto &[name, v] : o.at("channels").object())
      s.channels.push_back({name, fields(v.object()), s.owner_node});
    return s;
  }
  if (name == "tempest-peer/argus/events")
    return event(o);
  if (name == "tempest-peer/argus/board-snapshot") {
    if (!find(o, "complete") || !std::get<bool>(o.at("complete").storage()) ||
        !find(o, "events"))
      throw TEMPEST::Error("Board must be complete");
    ArgusBoardSnapshot s;
    s.owner_node = text(o, "owner_node");
    for (const auto &e : array(o, "events"))
      s.events.push_back(event(e.object()));
    return s;
  }
  throw TEMPEST::Error("Unknown telemetry topic: " + name);
}
} // namespace DARTWIC::EngineContract
namespace DARTWIC {
std::string eventTypeName(EventType t) {
  switch (t) {
  case EventType::Message:
    return "message";
  case EventType::Warning:
    return "warning";
  case EventType::Error:
    return "error";
  case EventType::Abort:
    return "abort";
  case EventType::Hold:
    return "hold";
  case EventType::Prompt:
    return "prompt";
  }
  throw TEMPEST::Error("Invalid event type");
}
EventType eventTypeFromName(const std::string &n) {
  for (auto t : {EventType::Message, EventType::Warning, EventType::Error,
                 EventType::Abort, EventType::Hold, EventType::Prompt})
    if (eventTypeName(t) == n)
      return t;
  throw TEMPEST::Error("Invalid event type: " + n);
}
} // namespace DARTWIC
