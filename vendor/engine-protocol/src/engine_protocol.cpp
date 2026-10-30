#include <cmath>
#include <dartwic/EngineProtocol.h>
#include <mutex>
#include <unordered_map>
namespace DARTWIC {
namespace {
constexpr const char *LOG_QUERY_OPERATION = "tempest-peer/argus/logs/query";
constexpr const char *LOG_LIST_OPERATION = "tempest-peer/argus/logs/list";
constexpr const char *LOG_BATCH_TOPIC = "tempest-peer/argus/logs/batch";

Value::Object logValue(const LogEntry &entry) {
  return {{"owner_node", entry.owner_node},
          {"session", entry.session},
          {"stream", entry.stream},
          {"channel", entry.channel},
          {"level", entry.level},
          {"text", entry.text},
          {"sequence", std::to_string(entry.sequence)},
          {"timestamp_ns", std::to_string(entry.timestamp_ns)}};
}
LogEntry readLog(const Value::Object &row) {
  auto get = [&](const char *key) {
    auto i = row.find(key);
    return i == row.end() ? std::string{} : i->second.string();
  };
  return {get("owner_node"),
          get("session"),
          get("stream"),
          get("channel"),
          get("level"),
          get("text"),
          std::stoull(get("sequence")),
          std::stoull(get("timestamp_ns"))};
}
} // namespace
struct EngineProtocol::Impl {
  TEMPEST::Peer &peer;
  std::mutex mutex;
  ChannelHandlers channel_handlers;
  ArgusHandlers argus_handlers;
  LogHandlers log_handlers;
  uint64_t revision = 1, event_id = 1;
  std::unordered_map<std::string, uint64_t> log_sequences;
  explicit Impl(TEMPEST::Peer &p) : peer(p) {}
  void check() const {
    if (peer.protocolId() != "tempest.engine" ||
        peer.protocolVersion() != EngineContract::version)
      throw TEMPEST::RemoteError("Engine Protocol version mismatch",
                                 "unsupported_contract");
  }
  Value::Object call(EngineRequest request) {
    check();
    return peer.call(EngineContract::operation(request),
                     EngineContract::arguments(request));
  }
  Value::Object dispatch(const std::string &name, const Value::Object &args) {
    check();
    auto request = EngineContract::request(name, args);
    ChannelHandlers c;
    ArgusHandlers a;
    {
      std::lock_guard lock(mutex);
      c = channel_handlers;
      a = argus_handlers;
    }
    return std::visit(
        [&](const auto &v) -> Value::Object {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ChannelUpsertRequest>) {
            if (c.upsertCommand) {
              c.upsertCommand(v);
              return {};
            }
          } else if constexpr (std::is_same_v<T, ChannelRemoveRequest>) {
            if (c.removeCommand) {
              c.removeCommand(v);
              return {};
            }
          } else if constexpr (std::is_same_v<T, ChannelQuery>) {
            if (c.queryCommand)
              return EngineContract::result(c.queryCommand(v));
          } else if constexpr (std::is_same_v<T, ArgusQuery>) {
            if (a.queryCommand)
              return EngineContract::result(a.queryCommand(v));
          } else {
            if (v.kind == ArgusAction::Kind::UpdateStatus &&
                a.updateStatusCommand)
              return EngineContract::result(a.updateStatusCommand(
                  v.event_id, v.payload.at("status").string()));
            if (v.kind == ArgusAction::Kind::Respond &&
                a.respondToPromptCommand)
              return EngineContract::result(
                  a.respondToPromptCommand(v.event_id, v.payload));
            if (v.kind == ArgusAction::Kind::ReleaseHold &&
                a.releaseHoldCommand)
              return EngineContract::result(
                  a.releaseHoldCommand(v.event_id, v.payload));
            if (v.kind == ArgusAction::Kind::Delete && a.removeCommand)
              return EngineContract::result(a.removeCommand(v.event_id));
          }
          throw TEMPEST::RemoteError("Unsupported command: " + name,
                                     "unsupported");
        },
        request);
  }
  void receive(const std::string &name, const Value::Object &args) {
    check();
    auto t = EngineContract::telemetry(name, args);
    ChannelHandlers c;
    ArgusHandlers a;
    {
      std::lock_guard lock(mutex);
      c = channel_handlers;
      a = argus_handlers;
    }
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ChannelUpdates>) {
            if (c.channelTelemetry)
              c.channelTelemetry(v);
          } else if constexpr (std::is_same_v<T, ChannelSnapshot>) {
            if (c.snapshotTelemetry)
              c.snapshotTelemetry(v);
          } else if constexpr (std::is_same_v<T, ArgusEvent>) {
            if (a.eventTelemetry)
              a.eventTelemetry(v);
          } else {
            if (a.snapshotTelemetry)
              a.snapshotTelemetry(v);
          }
        },
        t);
  }
  void publish(EngineTelemetry t) {
    check();
    if (!peer.ready())
      return;
    peer.publish(EngineContract::topic(t), EngineContract::payload(t));
  }
  void stamp(ChannelUpdate &c) {
    if (c.channel.empty())
      throw TEMPEST::Error("Channel required");
    if (c.owner_node.empty())
      c.owner_node = peer.nodeName();
    if (c.owner_node == peer.nodeName()) {
      if (c.source_session_id.empty())
        c.source_session_id = peer.sessionId();
      if (!c.revision)
        c.revision = revision++;
    }
  }
};

EngineProtocol::EngineProtocol(TEMPEST::Peer &peer)
    : impl_(std::make_shared<Impl>(peer)) {
  std::weak_ptr<Impl> weak = impl_;
  for (const auto *topic :
       {"tempest-peer/channels/updated", "tempest-peer/channels/snapshot",
        "tempest-peer/argus/events", "tempest-peer/argus/board-snapshot"}) {
    peer.onTelemetry(
        topic, [weak, name = std::string(topic)](const Value::Object &args) {
          if (auto p = weak.lock())
            p->receive(name, args);
        });
  }
  peer.onTelemetry(LOG_BATCH_TOPIC, [weak](const Value::Object &args) {
    if (auto p = weak.lock()) {
      LogHandlers h;
      {
        std::lock_guard lock(p->mutex);
        h = p->log_handlers;
      }
      if (h.logTelemetry) {
        std::vector<LogEntry> entries;
        for (const auto &row : args.at("records").array())
          entries.push_back(readLog(row.object()));
        h.logTelemetry(entries);
      }
    }
  });
}
EngineProtocol::~EngineProtocol() = default;
void EngineProtocol::Channels::setHandlers(ChannelHandlers h) const {
  auto p = owner_->impl_;
  const bool upsert = bool(h.upsertCommand), query = bool(h.queryCommand),
             remove = bool(h.removeCommand);
  {
    std::lock_guard lock(p->mutex);
    p->channel_handlers = std::move(h);
  }
  const std::pair<const char *, bool> commands[] = {
      {"tempest-peer/channels/upsert", upsert},
      {"tempest-peer/channels/query", query},
      {"tempest-peer/channels/remove", remove}};
  for (auto [name, enabled] : commands) {
    p->peer.unregisterOperation(name);
    if (enabled)
      p->peer.registerOperation(
          name, [weak = std::weak_ptr<Impl>(p), name](const Value::Object &a) {
            auto p = weak.lock();
            if (!p)
              throw TEMPEST::DisconnectedError("Protocol destroyed");
            return p->dispatch(name, a);
          });
  }
}
void EngineProtocol::Argus::setHandlers(ArgusHandlers h) const {
  auto p = owner_->impl_;
  const std::pair<const char *, bool> commands[] = {
      {"tempest-peer/argus/query", bool(h.queryCommand)},
      {ARGUS_UPDATE_EVENT_STATUS_OPERATION, bool(h.updateStatusCommand)},
      {ARGUS_RESPOND_TO_PROMPT_OPERATION, bool(h.respondToPromptCommand)},
      {ARGUS_RELEASE_HOLD_OPERATION, bool(h.releaseHoldCommand)},
      {ARGUS_DELETE_EVENT_OPERATION, bool(h.removeCommand)}};
  {
    std::lock_guard lock(p->mutex);
    p->argus_handlers = std::move(h);
  }
  for (auto [name, enabled] : commands) {
    p->peer.unregisterOperation(name);
    if (enabled)
      p->peer.registerOperation(
          name, [weak = std::weak_ptr<Impl>(p), name](const Value::Object &a) {
            auto p = weak.lock();
            if (!p)
              throw TEMPEST::DisconnectedError("Protocol destroyed");
            return p->dispatch(name, a);
          });
  }
}
void EngineProtocol::Channels::upsert(std::string name,
                                      ChannelFields fields) const {
  upsert(ChannelChanges{{std::move(name), std::move(fields)}});
}
void EngineProtocol::Channels::upsert(ChannelChanges changes) const {
  upsert(ChannelUpsertRequest{std::move(changes)});
}
void EngineProtocol::Channels::upsert(ChannelUpsertRequest r) const {
  owner_->impl_->call(std::move(r));
}
void EngineProtocol::Channels::remove(std::string name) const {
  remove(ChannelRemoveRequest{std::move(name)});
}
void EngineProtocol::Channels::remove(ChannelRemoveRequest r) const {
  owner_->impl_->call(std::move(r));
}
ChannelQueryResult EngineProtocol::Channels::query(ChannelQuery r) const {
  return EngineContract::channelResult(owner_->impl_->call(std::move(r)));
}
ChannelQueryResult EngineProtocol::Channels::query(
    std::initializer_list<std::string> names) const {
  return query(ChannelQuery{names});
}
void EngineProtocol::Channels::publishChannelTelemetry(
    ChannelUpdate update) const {
  publishChannelTelemetry(ChannelUpdates{std::move(update)});
}
void EngineProtocol::Channels::publishChannelTelemetry(
    ChannelUpdates updates) const {
  auto p = owner_->impl_;
  {
    std::lock_guard lock(p->mutex);
    for (auto &c : updates)
      p->stamp(c);
  }
  p->publish(std::move(updates));
}
void EngineProtocol::Channels::publishSnapshotTelemetry(
    ChannelSnapshot s) const {
  auto p = owner_->impl_;
  if (s.owner_node.empty())
    s.owner_node = p->peer.nodeName();
  {
    std::lock_guard lock(p->mutex);
    for (auto &c : s.channels) {
      p->stamp(c);
      if (c.owner_node != s.owner_node)
        throw TEMPEST::Error("Snapshot mixes owners");
    }
  }
  p->publish(std::move(s));
}
std::string EngineProtocol::Argus::publishEventTelemetry(ArgusEvent e) const {
  if (!std::isfinite(e.firing_timeout_seconds) || e.firing_timeout_seconds < 0)
    throw TEMPEST::Error(
        "Event requires finite nonnegative firing_timeout_seconds");
  auto p = owner_->impl_;
  {
    std::lock_guard lock(p->mutex);
    if (e.owner_node.empty())
      e.owner_node = p->peer.nodeName();
    if (e.event_id.empty())
      e.event_id = p->peer.nodeName() + ":" + p->peer.sessionId() + ":" +
                   std::to_string(p->event_id++);
  }
  auto id = e.event_id;
  p->publish(std::move(e));
  return id;
}
void EngineProtocol::Argus::publishSnapshotTelemetry(
    ArgusBoardSnapshot s) const {
  auto p = owner_->impl_;
  if (s.owner_node.empty())
    s.owner_node = p->peer.nodeName();
  for (auto &e : s.events) {
    if (e.owner_node.empty())
      e.owner_node = s.owner_node;
    if (e.owner_node != s.owner_node || e.event_id.empty() ||
        !std::isfinite(e.firing_timeout_seconds) || e.firing_timeout_seconds < 0)
      throw TEMPEST::Error("Invalid board event");
  }
  p->publish(std::move(s));
}
ArgusQueryResult EngineProtocol::Argus::query(ArgusQuery q) const {
  return EngineContract::argusResult(owner_->impl_->call(std::move(q)));
}
ArgusActionResult
EngineProtocol::Argus::updateStatus(std::string id, std::string status) const {
  return EngineContract::actionResult(
      owner_->impl_->call(ArgusAction{ArgusAction::Kind::UpdateStatus,
                                      std::move(id),
                                      {},
                                      {{"status", std::move(status)}}}));
}
ArgusActionResult
EngineProtocol::Argus::respondToPrompt(std::string id,
                                       Value::Object response) const {
  return EngineContract::actionResult(owner_->impl_->call(ArgusAction{
      ArgusAction::Kind::Respond, std::move(id), {}, std::move(response)}));
}
ArgusActionResult
EngineProtocol::Argus::releaseHold(std::string id,
                                   Value::Object response) const {
  return EngineContract::actionResult(owner_->impl_->call(ArgusAction{
      ArgusAction::Kind::ReleaseHold, std::move(id), {}, std::move(response)}));
}
ArgusActionResult EngineProtocol::Argus::remove(std::string id) const {
  return EngineContract::actionResult(owner_->impl_->call(
      ArgusAction{ArgusAction::Kind::Delete, std::move(id), {}, {}}));
}

void EngineProtocol::Logs::setHandlers(LogHandlers h) const {
  auto p = owner_->impl_;
  const bool query = bool(h.queryCommand), list = bool(h.listCommand);
  {
    std::lock_guard lock(p->mutex);
    p->log_handlers = std::move(h);
  }
  p->peer.unregisterOperation(LOG_QUERY_OPERATION);
  p->peer.unregisterOperation(LOG_LIST_OPERATION);
  if (query)
    p->peer.registerOperation(
        LOG_QUERY_OPERATION,
        [weak = std::weak_ptr<Impl>(p)](const Value::Object &args) {
          auto p = weak.lock();
          if (!p)
            throw TEMPEST::DisconnectedError("Protocol destroyed");
          LogHandlers h;
          {
            std::lock_guard lock(p->mutex);
            h = p->log_handlers;
          }
          if (!h.queryCommand)
            throw TEMPEST::RemoteError("Log query unsupported", "unsupported");
          auto get = [&](const char *k) {
            auto i = args.find(k);
            return i == args.end() ? std::string{} : i->second.string();
          };
          auto num = [&](const char *k, uint64_t def = 0) {
            auto v = get(k);
            return v.empty() ? def : static_cast<uint64_t>(std::stoull(v));
          };
          LogQuery q{get("owner_node"),
                     get("session"),
                     get("stream"),
                     get("text"),
                     num("before"),
                     num("after"),
                     static_cast<size_t>(num("limit", 250))};
          q.channel = get("channel");
          q.level = get("level");
          q.from_ns = num("from_ns");
          q.to_ns = num("to_ns");
          Value::Array a;
          for (const auto &e : h.queryCommand(q))
            a.emplace_back(logValue(e));
          return Value::Object{{"records", a}};
        });
  if (list)
    p->peer.registerOperation(
        LOG_LIST_OPERATION,
        [weak = std::weak_ptr<Impl>(p)](const Value::Object &) {
          auto p = weak.lock();
          if (!p)
            throw TEMPEST::DisconnectedError("Protocol destroyed");
          LogHandlers h;
          {
            std::lock_guard lock(p->mutex);
            h = p->log_handlers;
          }
          if (!h.listCommand)
            throw TEMPEST::RemoteError("Log list unsupported", "unsupported");
          Value::Array a;
          for (const auto &v : h.listCommand())
            a.emplace_back(Value::Object{{"owner_node", v.owner_node},
                                         {"session", v.session},
                                         {"stream", v.stream}});
          return Value::Object{{"streams", a}};
        });
}
std::vector<LogEntry> EngineProtocol::Logs::query(LogQuery request) const {
  Value::Object args{{"owner_node", request.owner_node},
                     {"session", request.session},
                     {"stream", request.stream},
                     {"text", request.text},
                     {"before", std::to_string(request.before)},
                     {"after", std::to_string(request.after)},
                     {"limit", std::to_string(request.limit)},
                     {"channel", request.channel},
                     {"level", request.level},
                     {"from_ns", std::to_string(request.from_ns)},
                     {"to_ns", std::to_string(request.to_ns)}};
  const auto response = owner_->impl_->peer.call(LOG_QUERY_OPERATION, args);
  std::vector<LogEntry> records;
  const auto it = response.find("records");
  if (it == response.end())
    return records;
  for (const auto &value : it->second.array()) {
    const auto &row = value.object();
    records.push_back(readLog(row));
  }
  return records;
}
std::vector<LogStream> EngineProtocol::Logs::list() const {
  const auto result = owner_->impl_->peer.call(LOG_LIST_OPERATION, {});
  std::vector<LogStream> streams;
  auto found = result.find("streams");
  if (found == result.end())
    return streams;
  for (const auto &item : found->second.array()) {
    const auto &row = item.object();
    streams.push_back({row.at("owner_node").string(),
                       row.at("session").string(), row.at("stream").string()});
  }
  return streams;
}

void EngineProtocol::Logs::publishLogTelemetry(LogEntry e) const {
  publishLogTelemetry(std::vector<LogEntry>{std::move(e)});
}
void EngineProtocol::Logs::publishLogTelemetry(
    std::vector<LogEntry> entries) const {
  auto p = owner_->impl_;
  p->check();
  if (!p->peer.ready() || entries.empty())
    return;
  Value::Array rows;
  {
    std::lock_guard lock(p->mutex);
    for (auto &e : entries) {
      if (e.owner_node.empty())
        e.owner_node = p->peer.nodeName();
      if (e.owner_node == p->peer.nodeName()) {
        if (e.session.empty())
          e.session = p->peer.sessionId();
        auto &sequence = p->log_sequences[e.session + "\n" + e.stream];
        if (!e.sequence)
          e.sequence = ++sequence;
        else
          sequence = std::max(sequence, e.sequence);
      }
      rows.emplace_back(logValue(e));
    }
  }
  p->peer.publish(LOG_BATCH_TOPIC, {{"records", rows}});
}
} // namespace DARTWIC
