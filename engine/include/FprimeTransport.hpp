#pragma once

#include <tempest/Transport.h>
#include <nlohmann/json.hpp>
#include <memory>

namespace FPrimeBridge {

// Ground-side adapter for an unchanged F Prime ComCcsds/TcpClient link.
class FprimeTransport final : public TEMPEST::Transport {
public:
    explicit FprimeTransport(const nlohmann::json& config);
    ~FprimeTransport() override;
    void start(TEMPEST::TransportCallbacks callbacks) override;
    void send(TEMPEST::Message message) override;
    void stop() override;
    std::vector<TEMPEST::TransportPath> diagnostics() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FPrimeBridge
