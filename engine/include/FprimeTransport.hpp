#pragma once

#include <tempest/Transport.h>
#include <nlohmann/json.hpp>
#include <memory>

namespace TEMPEST { class Peer; }
namespace DARTWIC { class EngineProtocol; }

namespace FPrimeBridge {

// Ground-side adapter for an unchanged F Prime ComCcsds/TcpClient link.
class FprimeTransport final : public TEMPEST::Transport {
public:
    FprimeTransport(const nlohmann::json& config, TEMPEST::Peer& peer, DARTWIC::EngineProtocol& protocol);
    ~FprimeTransport() override;
    void start() override;
    void stop() override;
    std::vector<TEMPEST::TransportPath> diagnostics() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FPrimeBridge
