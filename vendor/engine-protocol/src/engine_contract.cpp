#include <dartwic/EngineProtocol.h>
#include <limits>

namespace DARTWIC::EngineContract {
namespace {
using Object = Value::Object;
using Array = Value::Array;
const Value* find(const Object& o, const char* key) {
    auto it = o.find(key); return it == o.end() ? nullptr : &it->second;
}
std::string text(const Object& o, const char* key, std::string fallback = {}) {
    auto v = find(o, key); return v ? v->string() : std::move(fallback);
}
uint64_t number(const Object& o, const char* key, uint64_t fallback = 0) {
    auto v = find(o, key); if (!v) return fallback;
    if (auto n = std::get_if<uint64_t>(&v->storage())) return *n;
    if (auto n = std::get_if<int64_t>(&v->storage()); n && *n >= 0) return static_cast<uint64_t>(*n);
    throw TEMPEST::RemoteError(std::string(key) + " must be a nonnegative integer.", "invalid_arguments");
}
Object object(const Object& o, const char* key) { auto v=find(o,key); return v ? v->object() : Object{}; }
Array array(const Object& o, const char* key) { auto v=find(o,key); return v ? v->array() : Array{}; }
Value value(const Object& o, const char* key) { auto v=find(o,key); return v ? *v : Value{}; }
Array strings(const std::vector<std::string>& values) {
    Array a; for (const auto& v : values) a.emplace_back(v); return a;
}
std::vector<std::string> strings(const Object& o, const char* key) {
    std::vector<std::string> a; for (const auto& v : array(o,key)) a.push_back(v.string()); return a;
}
Array samples(const std::vector<ChannelSample>& values) {
    Array a; for (const auto& v : values) a.emplace_back(Object{{"value",v.value},{"timestamp",v.timestamp}}); return a;
}
std::vector<ChannelSample> samples(const Object& o) {
    std::vector<ChannelSample> a;
    for (const auto& v : array(o,"samples")) a.push_back({v.at("value"),number(v.object(),"timestamp")});
    return a;
}
Object event(const ArgusEvent& e) {
    return {{"event_id",e.event_id},{"type",e.type},{"title",e.title},{"description",e.description},
        {"status",e.status},{"owner_node",e.owner_node},{"timestamp",e.timestamp},
        {"channels",strings(e.channels)},{"details",e.details}};
}
ArgusEvent event(const Object& o) {
    return {text(o,"event_id"),text(o,"type","message"),text(o,"title"),text(o,"description"),
        text(o,"status","new"),text(o,"owner_node"),number(o,"timestamp"),strings(o,"channels"),object(o,"details")};
}
void channelName(const Object& o) {
    if (text(o,"channel").empty()) throw TEMPEST::RemoteError("Channel name is required.","invalid_arguments");
}
}

std::string operation(const EngineRequest& r) {
    return std::visit([](const auto& v)->std::string {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,ChannelUpsert>) return "tempest-peer/channels/upsert";
        if constexpr(std::is_same_v<T,ChannelRemove>) return "tempest-peer/channels/remove";
        if constexpr(std::is_same_v<T,ChannelBulkUpsert>) return "tempest-peer/channels/upsert-bulk";
        if constexpr(std::is_same_v<T,ChannelQuery>) return "tempest-peer/channels/query";
        if constexpr(std::is_same_v<T,ArgusQuery>) return "tempest-peer/argus/query";
        return "tempest-peer/argus/action";
    },r);
}
Object arguments(const EngineRequest& r) {
    return std::visit([](const auto& v)->Object {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,ChannelQuery>) return {{"channels",strings(v.channels)},{"fields",strings(v.fields)}};
        else if constexpr(std::is_same_v<T,ArgusQuery>) return {{"event_id",v.event_id},{"status",v.status},
            {"type",v.type},{"owner_node",v.owner_node},{"since",v.since},{"until",v.until},
            {"limit",static_cast<uint64_t>(v.limit)},{"filters",v.filters}};
        else if constexpr(std::is_same_v<T,ArgusAction>) return {{"kind",static_cast<int>(v.kind)},
            {"event_id",v.event_id},{"operation",v.operation},{"payload",v.payload}};
        else {
            Object o{{"owner_node",v.owner_node},{"channel",v.channel},{"revision",v.revision},{"source_session_id",v.source_session_id}};
            if constexpr(std::is_same_v<T,ChannelUpsert>) {
                o["field"]=v.field; o["value"]=v.value; o["channel_data"]=v.channel_data;
                o["take_manual_control"]=v.take_manual_control;
                o["release_manual_override"]=v.release_manual_override;
                if(v.timestamp) o["timestamp"]=*v.timestamp;
            }
            if constexpr(!std::is_same_v<T,ChannelRemove>) {
                o["samples"]=samples(v.samples); o["command_origin"]=v.command_origin;
            }
            return o;
        }
    },r);
}
EngineRequest request(const std::string& name,const Object& o) {
    if(name=="tempest-peer/channels/upsert") {
        channelName(o);
        ChannelUpsert v{text(o,"owner_node"),text(o,"channel"),text(o,"field","value"),value(o,"value")};
        if(find(o,"timestamp")) v.timestamp=number(o,"timestamp");
        v.samples=samples(o); v.channel_data=object(o,"channel_data"); v.command_origin=text(o,"command_origin");
        v.revision=number(o,"revision"); v.source_session_id=text(o,"source_session_id");
        if(const auto flag=find(o,"take_manual_control")) v.take_manual_control=std::get<bool>(flag->storage());
        if(const auto flag=find(o,"release_manual_override")) v.release_manual_override=std::get<bool>(flag->storage());
        return v;
    }
    if(name=="tempest-peer/channels/remove") {
        channelName(o); return ChannelRemove{text(o,"owner_node"),text(o,"channel"),number(o,"revision"),text(o,"source_session_id")};
    }
    if(name=="tempest-peer/channels/upsert-bulk") {
        channelName(o);
        return ChannelBulkUpsert{text(o,"owner_node"),text(o,"channel"),samples(o),text(o,"command_origin"),number(o,"revision"),text(o,"source_session_id")};
    }
    if(name=="tempest-peer/channels/query") return ChannelQuery{strings(o,"channels"),strings(o,"fields")};
    if(name=="tempest-peer/argus/query") return ArgusQuery{text(o,"event_id"),text(o,"status"),text(o,"type"),
        text(o,"owner_node"),number(o,"since"),number(o,"until"),static_cast<size_t>(number(o,"limit")),object(o,"filters")};
    if(name=="tempest-peer/argus/action") {
        const auto kind=number(o,"kind",4);
        if(kind>4 || text(o,"event_id").empty()) throw TEMPEST::RemoteError("Invalid ARGUS action.","invalid_arguments");
        return ArgusAction{static_cast<ArgusAction::Kind>(kind),text(o,"event_id"),text(o,"operation"),object(o,"payload")};
    }
    throw TEMPEST::RemoteError("Unknown Engine Protocol operation: "+name,"unsupported");
}
Object result(const ChannelQueryResult& r) {
    Array a; for(const auto& c:r.channels) a.emplace_back(Object{{"owner_node",c.owner_node},{"channel",c.channel},
        {"channel_data",c.channel_data},{"revision",c.revision},{"source_session_id",c.source_session_id}});
    return {{"channels",a}};
}
Object result(const ArgusQueryResult& r) {
    Array a; for(const auto& e:r.events) a.emplace_back(event(e));
    return {{"events",a},{"count",static_cast<uint64_t>(r.count)},{"total_count",static_cast<uint64_t>(r.total_count)}};
}
Object result(const ArgusActionResult& r) { return {{"event_id",r.event_id},{"result",r.result}}; }
ChannelQueryResult channelResult(const Object& o) {
    ChannelQueryResult r;
    for(const auto& v:array(o,"channels")) { const auto& c=v.object(); r.channels.push_back({text(c,"owner_node"),text(c,"channel"),
        object(c,"channel_data"),number(c,"revision"),text(c,"source_session_id")}); } return r;
}
ArgusQueryResult argusResult(const Object& o) {
    ArgusQueryResult r; for(const auto& e:array(o,"events")) r.events.push_back(event(e.object()));
    r.count=static_cast<size_t>(number(o,"count",r.events.size())); r.total_count=static_cast<size_t>(number(o,"total_count",r.count)); return r;
}
ArgusActionResult actionResult(const Object& o) { return {text(o,"event_id"),object(o,"result")}; }
std::string topic(const EngineTelemetry& t) {
    return std::visit([](const auto& v)->std::string {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,ChannelTelemetry>) return v.kind==ChannelTelemetry::Kind::Upsert
            ? "tempest-peer/channels/updated" : v.kind==ChannelTelemetry::Kind::Remove
            ? "tempest-peer/channels/removed" : "tempest-peer/channels/samples";
        if constexpr(std::is_same_v<T,ChannelSnapshotTelemetry>) return "tempest-peer/channels/snapshot";
        if constexpr(std::is_same_v<T,FixedGenerationTelemetry>) return "tempest-peer/channels/generation";
        return "tempest-peer/argus/events";
    },t);
}
Object payload(const EngineTelemetry& t) {
    return std::visit([](const auto& v)->Object {
        using T=std::decay_t<decltype(v)>;
        Object o;
        if constexpr(std::is_same_v<T,ChannelTelemetry>) {
            o=arguments(v.kind==ChannelTelemetry::Kind::Upsert ? EngineRequest{v.upsert}
                : v.kind==ChannelTelemetry::Kind::Remove ? EngineRequest{v.remove} : EngineRequest{v.bulk});
        } else if constexpr(std::is_same_v<T,ChannelSnapshotTelemetry>) {
            o={{"owner_node",v.owner_node},{"channels",v.channels}};
        } else if constexpr(std::is_same_v<T,FixedGenerationTelemetry>) {
            Array writes;
            for(const auto& w:v.writes) {
                Object a{{"channel",w.channel},{"field",w.field},{"value",w.value},{"revision",w.revision}};
                if(w.timestamp) a["timestamp"]=*w.timestamp; writes.emplace_back(a);
            }
            o={{"owner_node",v.owner_node},{"generation",v.generation},{"source_timestamp",v.source_timestamp},{"writes",writes}};
        } else o={{"kind",static_cast<int>(v.kind)},{"owner_node",v.owner_node},{"event",event(v.event)}};
        o["route"]=strings(v.route); return o;
    },t);
}
EngineTelemetry telemetry(const std::string& name,const Object& o) {
    if(name=="tempest-peer/channels/snapshot")
        return ChannelSnapshotTelemetry{text(o,"owner_node"),object(o,"channels"),strings(o,"route")};
    if(name=="tempest-peer/channels/updated"||name=="tempest-peer/channels/removed"||name=="tempest-peer/channels/samples") {
        ChannelTelemetry t; t.route=strings(o,"route");
        if(name=="tempest-peer/channels/updated") t.upsert=std::get<ChannelUpsert>(request("tempest-peer/channels/upsert",o));
        else if(name=="tempest-peer/channels/removed") { t.kind=ChannelTelemetry::Kind::Remove;t.remove=std::get<ChannelRemove>(request("tempest-peer/channels/remove",o)); }
        else { t.kind=ChannelTelemetry::Kind::BulkUpsert;t.bulk=std::get<ChannelBulkUpsert>(request("tempest-peer/channels/upsert-bulk",o)); }
        return t;
    }
    if(name=="tempest-peer/channels/generation") {
        FixedGenerationTelemetry t{text(o,"owner_node"),number(o,"generation"),number(o,"source_timestamp"),{},strings(o,"route")};
        for(const auto& v:array(o,"writes")) { const auto& w=v.object(); channelName(w);
            FixedGenerationWrite a{text(w,"channel"),text(w,"field","value"),value(w,"value"),{},number(w,"revision")};
            if(find(w,"timestamp")) a.timestamp=number(w,"timestamp"); t.writes.push_back(std::move(a)); }
        return t;
    }
    if(name=="tempest-peer/argus/events") {
        const auto kind=number(o,"kind"); if(kind>2) throw TEMPEST::Error("Invalid ARGUS event kind.");
        return ArgusEventTelemetry{static_cast<ArgusEventTelemetry::Kind>(kind),text(o,"owner_node"),event(object(o,"event")),strings(o,"route")};
    }
    throw TEMPEST::Error("Unknown Engine Protocol topic: "+name);
}
} // namespace DARTWIC::EngineContract
