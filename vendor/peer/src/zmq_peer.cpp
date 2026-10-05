#include <tempest/ZmqPeer.h>
#include <tempest/JsonValue.h>
#include <zmq.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace TEMPEST {
namespace {
using JsonObject=nlohmann::json;
std::atomic<uint64_t> tokens{1};
std::string token(){return std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+":"+std::to_string(tokens++);}
std::string endpoint(const std::string& host,int port){return "tcp://"+host+":"+(port ? std::to_string(port) : "*");}
std::string peerTopic(const std::string& id){return "tempest/peer/"+id+"/";}
uint64_t nowMs(){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());}
Message decode(const JsonObject& j){
    Message m;
    const auto kind=j.value("kind",std::string{"request"});
    if(kind=="request")m.kind=Message::Kind::Request;
    else if(kind=="response")m.kind=Message::Kind::Response;
    else if(kind=="telemetry")m.kind=Message::Kind::Telemetry;
    else throw Error("Invalid TEMPEST message kind.");
    m.request_id=j.value("requestId",std::string{});
    m.name=j.value(m.kind==Message::Kind::Telemetry?"topic":"name",std::string{});
    m.payload=Json::toObject(j.at("payload"));
    m.error=j.value("error",false);m.error_code=j.value("code",std::string{});
    return m;
}
struct Runtime;
struct Slot {
    std::string id=token();
    std::string remote_id;
    std::string remote_node;
    std::string operations;
    std::string telemetry;
    std::string password;
    TransportCallbacks callbacks;
    bool active=false;
    uint64_t last_send_ms=0,last_receive_ms=0,dropped=0,bytes_sent=0,bytes_received=0,replaced=0;
    std::string last_error;
    std::unordered_map<std::string,std::string> reply_routes;
};
struct Delivery {std::shared_ptr<Slot> slot;Message message;std::shared_ptr<const std::string> serialized_payload;};
struct SocketSet {
    std::unique_ptr<zmq::socket_t> dealer;
    std::string endpoint;
    std::string telemetry;
    std::string targeted_subscription;
};
struct Runtime:std::enable_shared_from_this<Runtime>{
    static constexpr size_t send_batch=64;
    static constexpr size_t receive_batch=64;
    static constexpr size_t receive_per_peer=8;
    bool standalone=false;
    ZmqPeerConfig local;
    ZmqPeerHub::Config config;
    ZmqPeerHub::RouterSender router_sender;
    ZmqPeerHub::Publisher publisher;
    ZmqPeerHub::AcceptHandler accept;
    std::mutex mutex;
    std::condition_variable cv;
    bool running=false;
    bool stopped=false;
    std::thread thread;
    std::thread telemetry_thread;
    struct IncomingTelemetry { std::string topic, wire; };
    std::deque<IncomingTelemetry> telemetry_inbox;
    uint64_t incoming_dropped=0,incoming_replaced=0;
    std::atomic<uint64_t> owned_bytes_sent=0,owned_bytes_received=0;
    std::condition_variable telemetry_cv;
    std::unordered_map<std::string,std::shared_ptr<Slot>> slots;
    std::unordered_map<std::string,std::string> closed_remote_ids;
    std::unordered_set<std::string> closed_local_ids;
    std::deque<Delivery> outbox;
    std::string operations_endpoint,telemetry_endpoint;

    void start(){
        auto ready=std::make_shared<std::promise<void>>();auto result=ready->get_future();
        {std::lock_guard lock(mutex);if(running)return;if(stopped)throw Error("Transport runtime was stopped.");running=true;}
        auto self=shared_from_this();
        telemetry_thread=std::thread([self]{self->telemetryLoop();});
        thread=std::thread([self,ready]{self->run(ready);});
        result.get();
    }
    void stop(){
        {std::lock_guard lock(mutex);running=false;stopped=true;outbox.clear();telemetry_inbox.clear();}
        cv.notify_all();
        telemetry_cv.notify_all();
        if(thread.joinable())thread.join();
        if(telemetry_thread.joinable())telemetry_thread.join();
    }
    ~Runtime(){stop();}
    void enqueue(std::shared_ptr<Slot> slot,Message message,
                 std::shared_ptr<const std::string> serialized_payload={}){
        // Validate adapter-specific encoding on the caller, so an invalid
        // value cannot terminate the socket worker and every other peer.
        if (!serialized_payload) Json::validateObject(message.payload);
        std::lock_guard lock(mutex);
        if(!running||!slot->active)throw DisconnectedError("ZeroMQ peer transport is stopped.");
        // These payloads are complete snapshots. Replace an unsent older frame
        // rather than allowing a slow socket to accumulate obsolete state.
        if (serialized_payload) {
            for (auto& delivery : outbox) {
                if (delivery.slot == slot && delivery.serialized_payload &&
                    delivery.message.name == message.name) {
                    delivery.message = std::move(message);
                    delivery.serialized_payload = std::move(serialized_payload);
                    ++slot->replaced;
                    return;
                }
            }
        }
        const bool control=message.kind==Message::Kind::Response
            || (message.kind==Message::Kind::Request
                && (message.name=="tempest/register-peer" || message.name=="tempest/heartbeat"));
        if(outbox.size()>=config.max_queued_messages){
            if(!control){++slot->dropped;slot->last_error="ZeroMQ peer send queue is full.";throw QueueFullError(slot->last_error);}
            auto discard=outbox.end();
            for(auto it=outbox.end();it!=outbox.begin();){
                --it;
                if(it->message.kind==Message::Kind::Telemetry){discard=it;break;}
            }
            if(discard==outbox.end()){++slot->dropped;slot->last_error="ZeroMQ peer send queue is full.";throw QueueFullError(slot->last_error);}
            ++discard->slot->dropped;
            discard->slot->last_error="Telemetry discarded to admit a control message.";
            outbox.erase(discard);
        }
        if(control)outbox.push_front({std::move(slot),std::move(message),std::move(serialized_payload)});
        else outbox.push_back({std::move(slot),std::move(message),std::move(serialized_payload)});
        cv.notify_one();
    }
    std::shared_ptr<Slot> add(ZmqPeerConfig connection){
        auto slot=std::make_shared<Slot>();slot->operations=endpoint(connection.host,connection.port);
        slot->telemetry=endpoint(connection.host,connection.port+1);slot->password=std::move(connection.password);
        std::lock_guard lock(mutex);
        if(slots.size()>=config.max_peers)throw QueueFullError("TEMPEST peer limit reached.");
        slots.emplace(slot->id,slot);return slot;
    }
    bool receive(std::string route,const std::string& wire,std::shared_ptr<Slot> known={});
    void queueTelemetry(std::string topic, std::string wire) {
        std::lock_guard lock(mutex);
        if (!running) return;
        const bool snapshot = topic.ends_with("/tempest-peer/channels/snapshot") ||
            topic.ends_with("/tempest-peer/argus/board-snapshot");
        if (snapshot) for (auto& pending : telemetry_inbox) {
            if (pending.topic == topic) { pending.wire = std::move(wire); ++incoming_replaced; return; }
        }
        if (telemetry_inbox.size() >= config.max_queued_messages) { ++incoming_dropped; return; }
        telemetry_inbox.push_back({std::move(topic), std::move(wire)});
        telemetry_cv.notify_one();
    }
    void telemetryLoop() {
        std::unique_lock lock(mutex);
        while (running) {
            telemetry_cv.wait(lock, [&]{ return !running || !telemetry_inbox.empty(); });
            if (!running) break;
            auto incoming = std::move(telemetry_inbox.front()); telemetry_inbox.pop_front();
            lock.unlock();
            try {
                auto value = Json::parseValue(incoming.wire);
                auto& envelope = std::get<Value::Object>(value.storage());
                if (envelope.at("kind").string() != "telemetry") throw Error("Invalid telemetry kind.");
                const auto& sender = envelope.at(envelope.contains("clientId") ? "clientId" : "sender").string();
                const auto& target = envelope.at("targetPeer").string();
                auto name = envelope.at("topic").string();
                Message message{Message::Kind::Telemetry,
                    envelope.contains("requestId") ? envelope.at("requestId").string() : std::string{}, std::move(name),
                    std::move(std::get<Value::Object>(envelope.at("payload").storage())),
                    envelope.contains("error") && std::get<bool>(envelope.at("error").storage()),
                    envelope.contains("code") ? envelope.at("code").string() : std::string{}};
                std::function<void(Message)> callback;
                {
                    std::lock_guard state_lock(mutex);
                    auto found = slots.find(target);
                    if (found != slots.end() && found->second->active && found->second->remote_id == sender) {
                        auto& slot = *found->second;
                        slot.last_receive_ms = nowMs(); slot.bytes_received += incoming.wire.size();
                        callback = slot.callbacks.on_message;
                    }
                }
                if (callback) callback(std::move(message));
            } catch (...) { /* Malformed telemetry cannot stop operations or other peers. */ }
            lock.lock();
        }
    }
    void run(const std::shared_ptr<std::promise<void>>& ready){
        bool initialized=false;
        try{
            zmq::context_t context(1);
            zmq::socket_t sub(context,zmq::socket_type::sub);
            sub.set(zmq::sockopt::linger,0);
            sub.set(zmq::sockopt::rcvhwm,4096);
            std::unique_ptr<zmq::socket_t> router,pub;
            if(standalone){
                router=std::make_unique<zmq::socket_t>(context,zmq::socket_type::router);
                pub=std::make_unique<zmq::socket_t>(context,zmq::socket_type::pub);
                router->set(zmq::sockopt::linger,0);pub->set(zmq::sockopt::linger,0);
                router->bind(endpoint(local.bind_host,local.operations_port));pub->bind(endpoint(local.bind_host,local.telemetry_port));
                auto advertised=[&](zmq::socket_t& socket){
                    const auto bound=socket.get(zmq::sockopt::last_endpoint);
                    return "tcp://"+local.advertised_host+bound.substr(bound.rfind(':'));
                };
                operations_endpoint=advertised(*router);telemetry_endpoint=advertised(*pub);
            }else{operations_endpoint=config.operations_endpoint;telemetry_endpoint=config.telemetry_endpoint;}
            initialized=true;ready->set_value();
            std::unordered_map<std::string,SocketSet> sockets;
            std::unordered_map<std::string,size_t> connections;
            std::unordered_map<std::string,size_t> subscriptions;
            auto detach=[&](const SocketSet& set){
                auto release=[&](const std::string& filter){
                    auto subscription=subscriptions.find(filter);
                    if(subscription!=subscriptions.end() && --subscription->second==0){
                        sub.set(zmq::sockopt::unsubscribe,subscription->first);subscriptions.erase(subscription);
                    }
                };
                release(set.targeted_subscription);
                auto connection=connections.find(set.telemetry);
                if(connection!=connections.end() && --connection->second==0){
                    sub.disconnect(connection->first);connections.erase(connection);
                }
            };
            while(true){
                std::vector<std::shared_ptr<Slot>> active;
                std::deque<Delivery> sends;
                {std::lock_guard lock(mutex);if(!running)break;
                    for(const auto& [id,slot]:slots)if(slot->active)active.push_back(slot);
                    for(size_t i=0;i<send_batch&&!outbox.empty();++i){
                        sends.push_back(std::move(outbox.front()));outbox.pop_front();
                    }}
                std::unordered_set<std::string> active_ids;
                active_ids.reserve(active.size());
                for(const auto& slot:active)active_ids.insert(slot->id);
                for(const auto& slot:active){
                    std::string operations, telemetry;
                    { std::lock_guard lock(mutex); operations=slot->operations; telemetry=slot->telemetry;
                    }
                    auto existing=sockets.find(slot->id);
                    if(existing!=sockets.end() && (existing->second.endpoint!=operations || existing->second.telemetry!=telemetry)) {
                        detach(existing->second);
                        sockets.erase(existing);
                    }
                    if(!sockets.contains(slot->id)){
                        SocketSet set;set.endpoint=operations;set.telemetry=telemetry;
                        set.targeted_subscription=peerTopic(slot->id);
                        set.dealer=std::make_unique<zmq::socket_t>(context,zmq::socket_type::dealer);
                        set.dealer->set(zmq::sockopt::linger,0);set.dealer->set(zmq::sockopt::immediate,1);
                        set.dealer->set(zmq::sockopt::sndhwm,256);set.dealer->set(zmq::sockopt::rcvhwm,256);
                        set.dealer->set(zmq::sockopt::routing_id,slot->id);set.dealer->connect(operations);
                        sockets.emplace(slot->id,std::move(set));
                        const auto& installed=sockets.at(slot->id);
                        if(subscriptions[installed.targeted_subscription]++==0)
                            sub.set(zmq::sockopt::subscribe,installed.targeted_subscription);
                        if(connections[telemetry]++==0)sub.connect(telemetry);
                    }
                }
                for(auto it=sockets.begin();it!=sockets.end();){
                    if(!active_ids.contains(it->first)) {
                        detach(it->second);
                        it=sockets.erase(it);
                    } else ++it;
                }
                for(auto& delivery:sends){
                    auto slot=delivery.slot;auto& m=delivery.message;
                    if(m.deadline<std::chrono::steady_clock::now())continue;
                    std::string target,route;
                    {std::lock_guard lock(mutex);if(!slot->active)continue;target=slot->remote_id;
                        if(m.kind==Message::Kind::Response){auto it=slot->reply_routes.find(m.request_id);
                            if(it!=slot->reply_routes.end()){route=std::move(it->second);slot->reply_routes.erase(it);}}}
                    if(m.name=="tempest/register-peer" && m.kind==Message::Kind::Request){
                        m.payload["operations_endpoint"]=operations_endpoint;
                        m.payload["telemetry_endpoint"]=telemetry_endpoint;
                        m.payload["serverPassword"]=slot->password;
                    }
                    JsonObject j{{"kind",m.kind==Message::Kind::Request?"request":m.kind==Message::Kind::Response?"response":"telemetry"},
                        {"requestId",m.request_id},{"name",m.name},{"topic",m.name},
                        {"error",m.error},{"code",m.error_code},
                        {"clientId",slot->id},{"sender",slot->id},{"targetPeer",target}};
                    auto wire=j.dump();
                    wire.pop_back();
                    wire += ",\"payload\":";
                    wire += delivery.serialized_payload ? *delivery.serialized_payload : Json::fromObject(m.payload).dump();
                    wire.push_back('}');
                    bool transmitted=false, dedicated=false;
                    if(m.kind==Message::Kind::Telemetry){
                        if(target.empty())continue;
                        const auto topic=peerTopic(target)+m.name;
                        if(pub){auto head=pub->send(zmq::buffer(topic),zmq::send_flags::sndmore);
                            auto body=pub->send(zmq::buffer(wire),zmq::send_flags::dontwait);transmitted=head&&body;}
                        else {publisher(topic,wire);transmitted=true;}
                    }else if(!route.empty()){
                        if(router){auto head=router->send(zmq::buffer(route),zmq::send_flags::sndmore);
                            auto body=router->send(zmq::buffer(wire),zmq::send_flags::dontwait);transmitted=head&&body;}
                        else {router_sender(route,wire);transmitted=true;}
                    }else{
                        dedicated=true;
                        const auto it=sockets.find(slot->id);
                        // No retry: a failed request eventually reaches its one
                        // deadline. Registration/heartbeats are independently retried.
                        const auto accepted=it!=sockets.end()
                            ? it->second.dealer->send(zmq::buffer(wire),zmq::send_flags::dontwait) : std::nullopt;
                        if(it!=sockets.end() && !accepted) {
                            // EAGAIN means the socket accepted no bytes. Retain
                            // this unsent message only until its original deadline.
                            std::lock_guard lock(mutex);
                            if (outbox.size() < config.max_queued_messages) {
                                if(m.name=="tempest/register-peer" || m.name=="tempest/heartbeat") outbox.push_front(std::move(delivery));
                                else outbox.push_back(std::move(delivery));
                            }
                        } else transmitted=accepted.has_value();
                    }
                    if(transmitted){if(dedicated)owned_bytes_sent+=wire.size();std::lock_guard lock(mutex);slot->last_send_ms=nowMs();slot->bytes_sent+=wire.size();}
                }
                if(router){
                    zmq::message_t route,body;
                    for(size_t i=0;i<receive_batch && router->recv(route,zmq::recv_flags::dontwait);++i){
                        if(!router->get(zmq::sockopt::rcvmore))continue;
                        (void)router->recv(body);
                        try{receive(route.to_string(),body.to_string(),active.empty()?nullptr:active.front());}catch(...){}
                    }
                }
                for(const auto& slot:active){
                    auto it=sockets.find(slot->id);if(it==sockets.end())continue;
                    zmq::message_t body;
                    for(size_t i=0;i<receive_per_peer && it->second.dealer->recv(body,zmq::recv_flags::dontwait);++i){
                        owned_bytes_received+=body.size();
                        try{receive({},body.to_string(),slot);}catch(...){}
                    }
                }
                zmq::message_t topic,body;
                const auto telemetry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2};
                for(size_t i=0;i<receive_batch && sub.recv(topic,zmq::recv_flags::dontwait);++i){
                    if(!sub.get(zmq::sockopt::rcvmore))continue;
                    (void)sub.recv(body);
                    owned_bytes_received+=body.size();
                    queueTelemetry(topic.to_string(), body.to_string());
                    // Bound inbox work too, so a telemetry flood cannot starve
                    // registration replies, commands, or heartbeat sends.
                    if (std::chrono::steady_clock::now() >= telemetry_deadline) break;
                }
                std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds{2},[&]{return !running||!outbox.empty();});
            }
        }catch(...){
            if(!initialized){ready->set_exception(std::current_exception());}
            std::vector<std::function<void(ConnectionState,std::string)>> callbacks;
            {std::lock_guard lock(mutex);running=false;for(auto& [_,slot]:slots)
                if(slot->active&&slot->callbacks.on_state)callbacks.push_back(slot->callbacks.on_state);}
            telemetry_cv.notify_all();
            for(auto& cb:callbacks)cb(ConnectionState::Disconnected,"ZeroMQ peer I/O failed.");
        }
    }
};
class ZmqConnection final:public Transport,public SerializedTelemetryTransport{
public:
    ZmqConnection(std::shared_ptr<Runtime> runtime,std::shared_ptr<Slot> slot,bool owns=false)
        :runtime_(std::move(runtime)),slot_(std::move(slot)),owns_(owns){}
    ~ZmqConnection()override{stop();}
    void start(TransportCallbacks callbacks)override{
        runtime_->start();
        std::function<void(ConnectionState,std::string)> state;
        {std::lock_guard lock(runtime_->mutex);slot_->callbacks=std::move(callbacks);slot_->active=true;state=slot_->callbacks.on_state;}
        if(state)state(ConnectionState::Connected,{});
    }
    void send(Message message)override{runtime_->enqueue(slot_,std::move(message));}
    bool sendSerializedTelemetry(std::string topic,std::string encoding,
        std::shared_ptr<const std::string> payload) override {
        if (encoding != "application/json") return false;
        if (!payload) throw std::invalid_argument("A serialized telemetry payload is required.");
        runtime_->enqueue(slot_, {Message::Kind::Telemetry, {}, std::move(topic), {}}, std::move(payload));
        return true;
    }
    void stop()override{
        {std::lock_guard lock(runtime_->mutex);
            // A remote still running its heartbeat loop must not recreate a
            // connection we deliberately closed. A new connection has a new id.
            if(slot_->active)runtime_->closed_local_ids.insert(slot_->id);
            if(slot_->active && !slot_->remote_id.empty())runtime_->closed_remote_ids[slot_->remote_id]=slot_->id;
            slot_->active=false;slot_->callbacks={};slot_->reply_routes.clear();runtime_->slots.erase(slot_->id);
            std::erase_if(runtime_->outbox,[this](const auto& pending){return pending.slot==slot_;});
            const auto topic_prefix=peerTopic(slot_->id);
            std::erase_if(runtime_->telemetry_inbox,[&](const auto& pending){return pending.topic.starts_with(topic_prefix);});}
        if(owns_)runtime_->stop();
    }
    std::vector<TransportPath> diagnostics() const override {
        std::lock_guard lock(runtime_->mutex);
        const auto state=slot_->active && runtime_->running ? "open" : "closed";
        const auto depth=std::count_if(runtime_->outbox.begin(),runtime_->outbox.end(),
            [this](const Delivery& item){return item.slot==slot_;});
        std::vector<TransportPath> paths;
        paths.push_back({slot_->id+"/dealer", "DEALER outgoing commands",slot_->operations,state,false,
            slot_->last_receive_ms,slot_->last_send_ms,static_cast<size_t>(depth),slot_->dropped,slot_->last_error,"tempest/zeromq"});
        paths.back().bytes_sent=slot_->bytes_sent;
        paths.back().bytes_received=slot_->bytes_received;
        paths.back().byte_counters_available=true;
        paths.push_back({slot_->id+"/sub", "SUB incoming telemetry",slot_->telemetry,state,true,
            slot_->last_receive_ms,0,0,0,{},"tempest/zeromq"});
        paths.push_back({"shared/router", "ROUTER incoming commands",runtime_->operations_endpoint,
            runtime_->running ? "open" : "closed",true,0,0,0,0,{},"tempest/zeromq"});
        paths.push_back({"shared/pub", "PUB outgoing telemetry",runtime_->telemetry_endpoint,
            runtime_->running ? "open" : "closed",true,0,0,0,0,{},"tempest/zeromq"});
        return paths;
    }
private:
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<Slot> slot_;
    bool owns_;
};
bool Runtime::receive(std::string route,const std::string& wire,std::shared_ptr<Slot> known){
    // Contiguous pointers avoid per-character checked std::string iterators
    // (and their shared Debug iterator locks) for large telemetry frames.
    const auto j=JsonObject::parse(wire.data(), wire.data() + wire.size());
    const auto name=j.value("name",std::string{});
    const auto sender=j.value("clientId",j.value("sender",std::string{}));
    const auto target=j.value("targetPeer",std::string{});
    auto slot=std::move(known);
    {std::lock_guard lock(mutex);
        if(!slot && !target.empty()){auto it=slots.find(target);if(it!=slots.end())slot=it->second;}
        if(!slot)for(auto& [_,candidate]:slots)if(!sender.empty()&&candidate->remote_id==sender){slot=candidate;break;}}
    const bool registration=name=="tempest/register-peer"&&j.value("kind",std::string{})=="request";
    if(!slot&&!registration)return false;
    if(registration){
        const auto& args=j.at("payload");
        const auto password=args.value("serverPassword",std::string{});
        const auto remote_node=args.at("node_name").get<std::string>();
        std::string closed_local_id;
        {std::lock_guard lock(mutex);
            if(!slot){
                auto closed=closed_remote_ids.find(sender);
                if(closed!=closed_remote_ids.end())closed_local_id=closed->second;
                else if(closed_local_ids.contains(target))closed_local_id=target;
            }}
        if(!closed_local_id.empty()){
            if(!route.empty()&&router_sender){
                auto denied=j;denied["kind"]="response";denied["error"]=true;denied["code"]="peer_disconnected";
                denied["clientId"]=closed_local_id;denied["sender"]=closed_local_id;denied["targetPeer"]=sender;
                denied["payload"]={{"error","This peer connection was disconnected. Add the peer again to reconnect."}};
                router_sender(route,denied.dump());
            }
            return true;
        }
        // A restarted application reuses its logical connection, even when
        // its socket identity and automatically allocated ports have changed.
        if(!slot) {
            std::lock_guard lock(mutex);
            for(const auto& [_,candidate]:slots)
                if(candidate->active && candidate->remote_node==remote_node) {slot=candidate;break;}
        }
        if(password!=(slot?slot->password:config.password)){
            if(!route.empty()&&router_sender){
                auto denied=j;denied["kind"]="response";denied["error"]=true;denied["code"]="authentication_failed";
                denied["payload"]={{"error","Invalid peer password."}};router_sender(route,denied.dump());
            }
            return true;
        }
        if(!slot){
            auto created=std::make_shared<Slot>();
            created->operations=args.at("operations_endpoint").get<std::string>();
            created->telemetry=args.at("telemetry_endpoint").get<std::string>();
            if(!created->operations.starts_with("tcp://")||!created->telemetry.starts_with("tcp://"))
                throw Error("Peer endpoints must be TCP endpoints.");
            created->password=config.password;created->remote_id=sender;
            {std::lock_guard lock(mutex);if(slots.size()>=config.max_peers)throw QueueFullError("Peer limit reached.");slots.emplace(created->id,created);}
            slot=created;
            try {
                if(!accept)throw Error("Incoming peers are unavailable.");
                accept(std::make_shared<ZmqConnection>(shared_from_this(),slot),
                    PeerDeclaration{args.at("peer_id").get<std::string>(), args.at("peer_version").get<uint64_t>(),
                                    args.at("protocol_id").get<std::string>(), args.at("protocol_version").get<uint64_t>()});
            } catch(const std::exception& error) {
                {std::lock_guard lock(mutex);slot->active=false;slots.erase(slot->id);}
                if(!route.empty()&&router_sender){
                    auto denied=j;denied["kind"]="response";denied["error"]=true;
                    denied["code"]="peer_definition_rejected";
                    denied["payload"]={{"error",error.what()}};
                    router_sender(route,denied.dump());
                }
                return true;
            }
        }
        const auto operations=args.at("operations_endpoint").get<std::string>();
        const auto telemetry=args.at("telemetry_endpoint").get<std::string>();
        if(!operations.starts_with("tcp://") || !telemetry.starts_with("tcp://")) throw Error("Peer endpoints must be TCP endpoints.");
        { std::lock_guard lock(mutex); slot->remote_node=remote_node;slot->operations=operations;
            slot->telemetry=telemetry; }
    }
    auto message=decode(j);
    std::function<void(Message)> callback;
    {std::lock_guard lock(mutex);
        if(!slot||!slot->active)return true;
        if(!target.empty()&&target!=slot->id&&message.kind!=Message::Kind::Response&&!registration)return true;
        if (!registration && !slot->remote_id.empty() && sender != slot->remote_id) return true;
        if(!sender.empty())slot->remote_id=sender;
        slot->last_receive_ms=nowMs();
        slot->bytes_received+=wire.size();
        if(!route.empty()&&message.kind==Message::Kind::Request){
            if(slot->reply_routes.size()>=4096)throw QueueFullError("Peer reply routing limit reached.");
            slot->reply_routes[message.request_id]=std::move(route);
        }
        callback=slot->callbacks.on_message;}
    if(callback)callback(std::move(message));
    return true;
}
}

TransportPtr makeZmqTransport(ZmqPeerConfig config){
    auto runtime=std::make_shared<Runtime>();runtime->standalone=true;runtime->local=config;
    runtime->config.max_queued_messages=config.max_queued_messages;runtime->config.password=config.password;
    return std::make_shared<ZmqConnection>(runtime,runtime->add(std::move(config)),true);
}
struct ZmqPeerHub::Impl{std::shared_ptr<Runtime> runtime;};
ZmqPeerHub::ZmqPeerHub(Config config,RouterSender router,Publisher publisher,AcceptHandler accept):impl_(std::make_shared<Impl>()){
    impl_->runtime=std::make_shared<Runtime>();impl_->runtime->config=std::move(config);
    impl_->runtime->router_sender=std::move(router);impl_->runtime->publisher=std::move(publisher);impl_->runtime->accept=std::move(accept);
}
ZmqPeerHub::~ZmqPeerHub(){stop();}
void ZmqPeerHub::start(){impl_->runtime->start();}
void ZmqPeerHub::stop(){impl_->runtime->stop();}
bool ZmqPeerHub::receive(std::string identity,std::string wire){return impl_->runtime->receive(std::move(identity),wire);}
TransportPtr ZmqPeerHub::connect(ZmqPeerConfig config){return std::make_shared<ZmqConnection>(impl_->runtime,impl_->runtime->add(std::move(config)));}
nlohmann::json ZmqPeerHub::diagnostics() const {
    auto& runtime=*impl_->runtime;std::lock_guard lock(runtime.mutex);
    auto owned_paths=nlohmann::json::array();
    for (const auto& [id,slot]:runtime.slots) owned_paths.push_back(id+"/dealer");
    return {{"bytes_sent",runtime.owned_bytes_sent.load()},{"bytes_received",runtime.owned_bytes_received.load()},{"owned_paths",owned_paths},
        {"queues",{
            {{"id","peer_outgoing"},{"depth",runtime.outbox.size()},{"capacity",runtime.config.max_queued_messages}},
            {{"id","peer_incoming"},{"depth",runtime.telemetry_inbox.size()},{"capacity",runtime.config.max_queued_messages},
                {"dropped_total",runtime.incoming_dropped},{"replaced_snapshots_total",runtime.incoming_replaced}}
        }}};
}
} // namespace TEMPEST
