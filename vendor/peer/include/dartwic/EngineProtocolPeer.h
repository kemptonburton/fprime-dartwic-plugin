#pragma once
#include <dartwic/EngineProtocol.h>
#include <tempest/ZmqPeer.h>
#include <memory>
#include <stdexcept>

namespace DARTWIC {
/** Named application identity and default link settings. Custom transports own
 * their endpoint settings. EngineProtocolPeer always supplies the shared Engine
 * contract. */
struct EngineProtocolPeerConfig {
  std::string node_name;
  std::string host = "127.0.0.1";
  int port = 7200;
  std::string password = "admin";
  std::string peer_id = "tempest.engine";
};
/** Shared EngineProtocol + TEMPEST Peer composition used by the Engine and C++ applications.
 * The configuration constructor owns a standalone Peer and stops it on destruction.
 * The shared-Peer constructor attaches to an existing server-owned Peer without creating
 * a transport or taking over its lifecycle. Register handlers before the owner starts
 * the Peer, and stop/join the Peer before destroying application handler state.
 * Commands wait for a response or throw; timeouts can leave the remote outcome unknown.
 * Telemetry is best effort. No channel/event/log storage is supplied or required. */
class EngineProtocolPeer {
public:
  explicit EngineProtocolPeer(EngineProtocolPeerConfig config,
                      TEMPEST::TransportPtr transport = {})
      : peer_(std::make_shared<TEMPEST::Peer>(TEMPEST::PeerConfig{
               .node_name = config.node_name,
               .peer_id = config.peer_id,
               .protocol_id = "tempest.engine",
               .protocol_version = EngineContract::version},
              transport ? std::move(transport)
                        : TEMPEST::makeZmqTransport({.host = config.host,
                                                    .port = config.port,
                                                    .password = config.password}))),
        owns_lifecycle_(true), protocol_(*peer_) {}
  /** Attach to a nonnull server Peer before it starts. Retains the Peer,
   * preserves its transport and fallbacks, and leaves start/stop with the server.
   * The host validates the registered protocol identity and version. Throws invalid_argument for a null Peer. */
  explicit EngineProtocolPeer(std::shared_ptr<TEMPEST::Peer> peer)
      : peer_(requireEnginePeer(std::move(peer))), protocol_(*peer_) {}
  ~EngineProtocolPeer() { if (owns_lifecycle_) stop(); }
  EngineProtocolPeer(const EngineProtocolPeer &) = delete;
  EngineProtocolPeer &operator=(const EngineProtocolPeer &) = delete;
  /** The same underlying Peer, for custom messaging and diagnostics. */
  TEMPEST::Peer &peer() { return *peer_; }
  const TEMPEST::Peer &peer() const { return *peer_; }
  void start() { peer_->start(); }
  void stop() { peer_->stop(); }
  bool ready() const { return peer_->ready(); }
  void waitUntilReady(
      std::chrono::milliseconds timeout = std::chrono::seconds(5)) const {
    peer_->waitUntilReady(timeout);
  }
  std::string sessionId() const { return peer_->sessionId(); }
  std::string remoteSession() const { return peer_->remoteSession(); }
  std::string remoteNode() const { return peer_->remoteNode(); }
  void onStateChanged(
      std::function<void(TEMPEST::ConnectionState, std::string)> handler) {
    peer_->onStateChanged(std::move(handler));
  }
  EngineProtocol::Channels &channels() { return protocol_.channels(); }
  EngineProtocol::Argus &argus() { return protocol_.argus(); }
  EngineProtocol::Logs &logs() { return protocol_.logs(); }
  void registerOperation(std::string name,
                         TEMPEST::Peer::OperationHandler handler) {
    peer_->registerOperation(std::move(name), std::move(handler));
  }
  void registerOperation(TEMPEST::OperationDescriptor descriptor,
                         TEMPEST::Peer::OperationHandler handler) {
    peer_->registerOperation(std::move(descriptor), std::move(handler));
  }
  void onTelemetry(std::string topic, TEMPEST::Peer::TelemetryHandler handler) {
    peer_->onTelemetry(std::move(topic), std::move(handler));
  }
  void publish(std::string topic, Value::Object payload) {
    peer_->publish(std::move(topic), std::move(payload));
  }
  void registerTelemetry(TEMPEST::TelemetryDescriptor descriptor) {
    peer_->registerTelemetry(std::move(descriptor));
  }
  Value::Object call(std::string name, Value::Object arguments = {},
                     std::chrono::milliseconds timeout = {}) {
    return peer_->call(std::move(name), std::move(arguments), timeout);
  }

private:
  static std::shared_ptr<TEMPEST::Peer> requireEnginePeer(std::shared_ptr<TEMPEST::Peer> peer) {
    if (!peer)
      throw std::invalid_argument("EngineProtocolPeer requires a nonnull Peer.");
    return peer;
  }
  std::shared_ptr<TEMPEST::Peer> peer_;
  bool owns_lifecycle_ = false;
  EngineProtocol protocol_;
};
} // namespace DARTWIC
