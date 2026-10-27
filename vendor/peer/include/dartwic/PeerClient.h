#pragma once
#include <dartwic/EngineProtocol.h>
#include <tempest/ZmqPeer.h>

namespace DARTWIC {
/** Named application identity and default link settings. Custom transports own
 * their endpoint settings. PeerClient always supplies the shared Engine
 * contract. */
struct PeerClientConfig {
  std::string node_name;
  std::string host = "127.0.0.1";
  int port = 7200;
  std::string password = "admin";
  std::string peer_id = "tempest.engine";
};
/** A Peer and EngineProtocol with optional default ZeroMQ transport.
 * Register handlers before start. Application state must outlive
 * stop/destruction. No channel/event/log storage is supplied or required. */
class PeerClient {
public:
  explicit PeerClient(PeerClientConfig config,
                      TEMPEST::TransportPtr transport = {})
      : peer_({.node_name = config.node_name,
               .peer_id = config.peer_id,
               .protocol_id = "tempest.engine",
               .protocol_version = EngineContract::version},
              transport
                  ? std::move(transport)
                  : TEMPEST::makeZmqTransport({.host = config.host,
                                               .port = config.port,
                                               .password = config.password})),
        protocol_(peer_) {}
  ~PeerClient() { stop(); }
  void start() { peer_.start(); }
  void stop() { peer_.stop(); }
  bool ready() const { return peer_.ready(); }
  void waitUntilReady(
      std::chrono::milliseconds timeout = std::chrono::seconds(5)) const {
    peer_.waitUntilReady(timeout);
  }
  std::string sessionId() const { return peer_.sessionId(); }
  std::string remoteSession() const { return peer_.remoteSession(); }
  std::string remoteNode() const { return peer_.remoteNode(); }
  void onStateChanged(
      std::function<void(TEMPEST::ConnectionState, std::string)> handler) {
    peer_.onStateChanged(std::move(handler));
  }
  EngineProtocol::Channels &channels() { return protocol_.channels(); }
  EngineProtocol::Argus &argus() { return protocol_.argus(); }
  EngineProtocol::Logs &logs() { return protocol_.logs(); }
  void registerOperation(std::string name,
                         TEMPEST::Peer::OperationHandler handler) {
    peer_.registerOperation(std::move(name), std::move(handler));
  }
  void registerOperation(TEMPEST::OperationDescriptor descriptor,
                         TEMPEST::Peer::OperationHandler handler) {
    peer_.registerOperation(std::move(descriptor), std::move(handler));
  }
  void onTelemetry(std::string topic, TEMPEST::Peer::TelemetryHandler handler) {
    peer_.onTelemetry(std::move(topic), std::move(handler));
  }
  void publish(std::string topic, Value::Object payload) {
    peer_.publish(std::move(topic), std::move(payload));
  }
  void registerTelemetry(TEMPEST::TelemetryDescriptor descriptor) {
    peer_.registerTelemetry(std::move(descriptor));
  }
  Value::Object call(std::string name, Value::Object arguments = {},
                     std::chrono::milliseconds timeout = {}) {
    return peer_.call(std::move(name), std::move(arguments), timeout);
  }

private:
  TEMPEST::Peer peer_;
  EngineProtocol protocol_;
};
} // namespace DARTWIC
