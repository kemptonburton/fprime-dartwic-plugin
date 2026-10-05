#include <dartwic/EngineProtocol.h>
#include <mutex>
#include <unordered_map>

namespace DARTWIC {
namespace {
Value::Object logValue(const LogEntry& entry) {
    return {{"owner_node", entry.owner_node}, {"session", entry.session}, {"stream", entry.stream},
        {"channel", entry.channel}, {"level", entry.level}, {"text", entry.text},
        {"sequence", std::to_string(entry.sequence)}, {"timestamp_ns", std::to_string(entry.timestamp_ns)}};
}
LogEntry readLog(const Value::Object& row) {
    auto get = [&](const char* key) { auto i = row.find(key); return i == row.end() ? std::string{} : i->second.string(); };
    return {get("owner_node"), get("session"), get("stream"), get("channel"), get("level"),
        get("text"), std::stoull(get("sequence")), std::stoull(get("timestamp_ns"))};
}
}
struct EngineProtocol::Impl {
    TEMPEST::Peer& peer;
    std::mutex mutex;
    ChannelHandlers channel_handlers;
    ArgusHandlers argus_handlers;
    LogHandlers log_handlers;
    std::unordered_map<std::string,std::function<void(const ArgusAction&)>> prompts;
    uint64_t revision=1;
    uint64_t event_id=1;
    explicit Impl(TEMPEST::Peer& p):peer(p) {}

    void check() {
        if(peer.protocolId()!="tempest.engine" || peer.protocolVersion()!=EngineContract::version)
            throw TEMPEST::RemoteError("Peer is not configured for this DARTWIC Engine Protocol version.","unsupported_contract");
    }
    Value::Object call(EngineRequest request) {
        check();
        return peer.call(EngineContract::operation(request),EngineContract::arguments(request));
    }
    Value::Object dispatch(const std::string& name,const Value::Object& args) {
        check();
        auto request=EngineContract::request(name,args);
        ChannelHandlers channels; ArgusHandlers argus;
        { std::lock_guard lock(mutex);channels=channel_handlers;argus=argus_handlers; }
        return std::visit([&](const auto& v)->Value::Object {
            using T=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T,ChannelUpsert>) { if(channels.upsert){channels.upsert(v);return {};} }
            else if constexpr(std::is_same_v<T,ChannelRemove>) { if(channels.remove){channels.remove(v);return {};} }
            else if constexpr(std::is_same_v<T,ChannelBulkUpsert>) {
                if(v.samples.empty()) throw TEMPEST::RemoteError("Bulk command needs samples.","invalid_arguments");
                if(channels.bulk_upsert){channels.bulk_upsert(v);return {};}
            }
            else if constexpr(std::is_same_v<T,ChannelQuery>) { if(channels.query)return EngineContract::result(channels.query(v)); }
            else if constexpr(std::is_same_v<T,ArgusQuery>) { if(argus.query)return EngineContract::result(argus.query(v)); }
            else {
                std::function<void(const ArgusAction&)> callback;
                if(v.kind==ArgusAction::Kind::Respond) {
                    std::lock_guard lock(mutex);
                    auto it=prompts.find(v.event_id);
                    if(it!=prompts.end()){callback=std::move(it->second);prompts.erase(it);}
                }
                if(callback) callback(v);
                if(argus.action)return EngineContract::result(argus.action(v));
                if(callback)return EngineContract::result(ArgusActionResult{v.event_id,{}});
            }
            throw TEMPEST::RemoteError("No engine handler for "+name,"unsupported");
        },request);
    }
    void receive(const std::string& name,const Value::Object& args) {
        check();
        const auto t=EngineContract::telemetry(name,args);
        ChannelHandlers channels; ArgusHandlers argus;
        {std::lock_guard lock(mutex);channels=channel_handlers;argus=argus_handlers;}
        std::visit([&](const auto& v) {
            using T=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T,ChannelTelemetry>){if(channels.telemetry)channels.telemetry(v);}
            else if constexpr(std::is_same_v<T,ChannelSnapshotTelemetry>){if(channels.snapshot)channels.snapshot(v);}
            else if constexpr(std::is_same_v<T,FixedGenerationTelemetry>){if(channels.fixed_generation)channels.fixed_generation(v);}
            else {if(argus.telemetry)argus.telemetry(v);}
        },t);
    }
    void publish(EngineTelemetry telemetry) {
        if(!peer.ready())return;
        check();
        if(auto t=std::get_if<ChannelTelemetry>(&telemetry)) {
            std::lock_guard lock(mutex);
            auto stamp=[&](auto& v){
                if(v.owner_node.empty())v.owner_node=peer.nodeName();
                if(v.owner_node==peer.nodeName()){
                    if(v.source_session_id.empty())v.source_session_id=peer.sessionId();
                    if(!v.revision)v.revision=revision++;
                }
            };
            if(t->kind==ChannelTelemetry::Kind::Upsert)stamp(t->upsert);
            else if(t->kind==ChannelTelemetry::Kind::Remove)stamp(t->remove);
            else stamp(t->bulk);
        }
        peer.publish(EngineContract::topic(telemetry),EngineContract::payload(telemetry));
    }
    std::string prepareEvent(ArgusEvent& event) {
        std::lock_guard lock(mutex);
        if(event.owner_node.empty())event.owner_node=peer.nodeName();
        if(event.event_id.empty())event.event_id=peer.nodeName()+":"+peer.sessionId()+":"+std::to_string(event_id++);
        return event.event_id;
    }
};

EngineProtocol::EngineProtocol(TEMPEST::Peer& peer):impl_(std::make_shared<Impl>(peer)) {
    std::weak_ptr<Impl> weak=impl_;
    for(const auto* name:{"tempest-peer/channels/upsert","tempest-peer/channels/remove","tempest-peer/channels/upsert-bulk",
                         "tempest-peer/channels/query","tempest-peer/argus/query","tempest-peer/argus/action"}) {
        peer.registerOperation(name,[weak,name](const Value::Object& args) {
            auto p=weak.lock();
            if(!p)throw TEMPEST::DisconnectedError("Engine Protocol was destroyed.");
            return p->dispatch(name,args);
        });
    }
    for(const auto* name:{"tempest-peer/channels/updated","tempest-peer/channels/removed","tempest-peer/channels/samples",
                         "tempest-peer/channels/snapshot",
                         "tempest-peer/channels/generation","tempest-peer/argus/events"}) {
        peer.onTelemetry(name,[weak,name](const Value::Object& args){if(auto p=weak.lock())p->receive(name,args);});
    }
    peer.registerOperation("tempest-peer/argus/logs/query", [weak](const Value::Object& args) {
        auto p = weak.lock();
        if (!p) throw TEMPEST::DisconnectedError("Engine Protocol was destroyed.");
        LogHandlers handlers;
        { std::lock_guard lock(p->mutex); handlers = p->log_handlers; }
        if (!handlers.query) throw TEMPEST::RemoteError("Log query is unsupported.", "unsupported");
        auto get = [&](const char* key) { auto i = args.find(key); return i == args.end() ? std::string{} : i->second.string(); };
        auto number = [&](const char* key, uint64_t fallback) {
            const auto value = get(key);
            return value.empty() ? fallback : static_cast<uint64_t>(std::stoull(value));
        };
        LogQuery query{get("owner_node"), get("session"), get("stream"), get("text"),
            number("before", 0), number("after", 0), static_cast<size_t>(number("limit", 250))};
        query.channel = get("channel");
        query.level = get("level");
        query.from_ns = number("from_ns", 0);
        query.to_ns = number("to_ns", 0);
        Value::Array rows;
        for (const auto& entry : handlers.query(query)) rows.emplace_back(logValue(entry));
        return Value::Object{{"records", rows}};
    });
    peer.registerOperation("tempest-peer/argus/logs/list", [weak](const Value::Object&) {
        auto p = weak.lock();
        if (!p) throw TEMPEST::DisconnectedError("Engine Protocol was destroyed.");
        LogHandlers handlers;
        { std::lock_guard lock(p->mutex); handlers = p->log_handlers; }
        if (!handlers.list) throw TEMPEST::RemoteError("Log listing is unsupported.", "unsupported");
        Value::Array streams;
        for (const auto& stream : handlers.list()) streams.emplace_back(Value::Object{
            {"owner_node", stream.owner_node}, {"session", stream.session}, {"stream", stream.stream}});
        return Value::Object{{"streams", streams}};
    });
    peer.onTelemetry("tempest-peer/argus/logs", [weak](const Value::Object& args) {
        auto p = weak.lock();
        if (!p) return;
        LogHandlers handlers;
        { std::lock_guard lock(p->mutex); handlers = p->log_handlers; }
        if (!handlers.telemetry) return;
        handlers.telemetry(readLog(args));
    });
    peer.onTelemetry("tempest-peer/argus/logs/batch", [weak](const Value::Object& args) {
        auto p = weak.lock();
        if (!p) return;
        LogHandlers handlers;
        { std::lock_guard lock(p->mutex); handlers = p->log_handlers; }
        if (!handlers.telemetry) return;
        auto found = args.find("records");
        if (found == args.end()) return;
        for (const auto& row : found->second.array()) handlers.telemetry(readLog(row.object()));
    });
}
void EngineProtocol::Logs::setHandlers(LogHandlers handlers) const {
    std::lock_guard lock(owner_->impl_->mutex);
    owner_->impl_->log_handlers = std::move(handlers);
}
std::vector<LogEntry> EngineProtocol::Logs::query(LogQuery request) const {
    Value::Object args{{"owner_node", request.owner_node}, {"session", request.session},
        {"stream", request.stream}, {"text", request.text}, {"before", std::to_string(request.before)},
        {"after", std::to_string(request.after)}, {"limit", std::to_string(request.limit)},
        {"channel", request.channel}, {"level", request.level},
        {"from_ns", std::to_string(request.from_ns)}, {"to_ns", std::to_string(request.to_ns)}};
    const auto response = owner_->impl_->peer.call("tempest-peer/argus/logs/query", args);
    std::vector<LogEntry> records;
    const auto it = response.find("records");
    if (it == response.end()) return records;
    for (const auto& value : it->second.array()) {
        const auto& row = value.object();
        records.push_back(readLog(row));
    }
    return records;
}
std::vector<LogStream> EngineProtocol::Logs::list() const {
    const auto result = owner_->impl_->peer.call("tempest-peer/argus/logs/list", {});
    std::vector<LogStream> streams;
    auto found = result.find("streams");
    if (found == result.end()) return streams;
    for (const auto& item : found->second.array()) {
        const auto& row = item.object();
        streams.push_back({row.at("owner_node").string(), row.at("session").string(), row.at("stream").string()});
    }
    return streams;
}
void EngineProtocol::Logs::publish(LogEntry entry) const {
    owner_->impl_->peer.publish("tempest-peer/argus/logs", logValue(entry));
}
void EngineProtocol::Logs::publishBatch(std::vector<LogEntry> entries) const {
    if (entries.empty()) return;
    Value::Array rows;
    rows.reserve(entries.size());
    for (const auto& entry : entries) rows.emplace_back(logValue(entry));
    owner_->impl_->peer.publish("tempest-peer/argus/logs/batch", {{"records", rows}});
}
EngineProtocol::~EngineProtocol()=default;
void EngineProtocol::Channels::setHandlers(ChannelHandlers h)const{std::lock_guard lock(owner_->impl_->mutex);owner_->impl_->channel_handlers=std::move(h);}
void EngineProtocol::Channels::upsert(ChannelUpsert r)const{if(r.channel.empty())throw TEMPEST::Error("Channel name is required.");owner_->impl_->call(std::move(r));}
void EngineProtocol::Channels::remove(ChannelRemove r)const{if(r.channel.empty())throw TEMPEST::Error("Channel name is required.");owner_->impl_->call(std::move(r));}
void EngineProtocol::Channels::upsertBulk(ChannelBulkUpsert r)const{if(r.channel.empty()||r.samples.empty())throw TEMPEST::Error("Bulk command needs channel and samples.");owner_->impl_->call(std::move(r));}
ChannelQueryResult EngineProtocol::Channels::query(ChannelQuery r)const{return EngineContract::channelResult(owner_->impl_->call(std::move(r)));}
void EngineProtocol::Channels::publishTelemetry(ChannelTelemetry t)const{owner_->impl_->publish(std::move(t));}
void EngineProtocol::Channels::publishTelemetry(FixedGenerationTelemetry t)const{owner_->impl_->publish(std::move(t));}
void EngineProtocol::Argus::setHandlers(ArgusHandlers h)const{std::lock_guard lock(owner_->impl_->mutex);owner_->impl_->argus_handlers=std::move(h);}
void EngineProtocol::Argus::publishTyped(const char* type,ArgusEvent e)const{
    e.type=type;owner_->impl_->prepareEvent(e);
    const auto owner=e.owner_node;
    owner_->impl_->publish(ArgusEventTelemetry{ArgusEventTelemetry::Kind::Created,owner,std::move(e),{}});
}
void EngineProtocol::Argus::message(ArgusEvent e)const{publishTyped("message",std::move(e));}
void EngineProtocol::Argus::warning(ArgusEvent e)const{publishTyped("warning",std::move(e));}
void EngineProtocol::Argus::error(ArgusEvent e)const{publishTyped("error",std::move(e));}
void EngineProtocol::Argus::abort(ArgusEvent e)const{publishTyped("abort",std::move(e));}
void EngineProtocol::Argus::hold(ArgusEvent e)const{publishTyped("hold",std::move(e));}
std::string EngineProtocol::Argus::prompt(ArgusEvent e,std::function<void(const ArgusAction&)> response)const{
    const auto id=owner_->impl_->prepareEvent(e);
    if(response){std::lock_guard lock(owner_->impl_->mutex);owner_->impl_->prompts[id]=std::move(response);}
    publishTyped("prompt",std::move(e));return id;
}
void EngineProtocol::Argus::respond(std::string id,Value::Object payload)const{owner_->impl_->call(ArgusAction{ArgusAction::Kind::Respond,std::move(id),"respond",std::move(payload)});}
void EngineProtocol::Argus::updateStatus(std::string id,std::string status)const{owner_->impl_->call(ArgusAction{ArgusAction::Kind::UpdateStatus,std::move(id),"update-status",{{"status",std::move(status)}}});}
void EngineProtocol::Argus::releaseHold(std::string id,Value::Object payload)const{owner_->impl_->call(ArgusAction{ArgusAction::Kind::ReleaseHold,std::move(id),"release-hold",std::move(payload)});}
void EngineProtocol::Argus::remove(std::string id)const{owner_->impl_->call(ArgusAction{ArgusAction::Kind::Delete,std::move(id),"delete",{}});}
ArgusQueryResult EngineProtocol::Argus::query(ArgusQuery r)const{return EngineContract::argusResult(owner_->impl_->call(std::move(r)));}
void EngineProtocol::Argus::publishTelemetry(ArgusEventTelemetry t)const{owner_->impl_->publish(std::move(t));}
} // namespace DARTWIC
