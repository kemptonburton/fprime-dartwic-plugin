#pragma once
#include <tempest/Peer.h>
#include <nlohmann/json_fwd.hpp>

namespace TEMPEST {
/** Default ZeroMQ connection and advertised endpoints. Zero local ports choose available ports. Use a reachable advertised_host for remote machines. */
struct ZmqPeerConfig {
    std::string host = "127.0.0.1";
    int port = 7200;
    std::string password = "admin";
    std::string bind_host = "127.0.0.1";
    std::string advertised_host = "127.0.0.1";
    int operations_port = 0;
    int telemetry_port = 0;
    std::size_t max_queued_messages = 4096;
};
/** Explicit connection contract declared before an incoming peer is constructed. */
struct PeerDeclaration {
    std::string peer_id;
    uint64_t peer_version = 1;
    std::string protocol_id;
    uint64_t protocol_version = 1;
};
/** Create the optional standalone ZeroMQ transport with four socket roles and JSON encoding. Ordinary TEMPEST clients do not acquire these extra roles. */
TransportPtr makeZmqTransport(ZmqPeerConfig config = {});

/** Reuse an engine server ROUTER/PUB and own one shared SUB plus one DEALER per logical peer connection. */
class ZmqPeerHub {
public:
    struct Config {
        std::string operations_endpoint;
        std::string telemetry_endpoint;
        std::string password;
        std::size_t max_peers = 128;
        std::size_t max_queued_messages = 4096;
    };
    using AcceptHandler = std::function<void(TransportPtr, PeerDeclaration)>;
    using RouterSender = std::function<void(std::string identity, std::string wire)>;
    using Publisher = std::function<void(std::string topic, std::string wire)>;
    ZmqPeerHub(Config config, RouterSender router, Publisher publisher, AcceptHandler accept);
    ~ZmqPeerHub();
    void start();
    void stop();
    // Called for frames received on the server's existing ROUTER. Returns
    // false for ordinary clients, which continue through their usual path.
    bool receive(std::string identity, std::string wire);
    TransportPtr connect(ZmqPeerConfig config);
    nlohmann::json diagnostics() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace TEMPEST
