#include "FprimeTransport.hpp"
#include <plugins/BasePlugin.h>
#include <tempest/Peer.h>

namespace FPrimeBridge {
class Plugin final : public DARTWIC::Plugins::BasePlugin {
public:
    using BasePlugin::BasePlugin;
    void onPluginLoaded() override {
        dartwic->registerPeer({
            .id = "fprime_ccsds",
            .name = "F Prime ComCcsds",
            .version = 1,
            .default_config = {
                {"node_name", "HADRON_SITL"},
                {"bind_host", "127.0.0.1"},
                {"port", 50101},
                {"dictionary", "SITLDeploymentTopologyDictionary.json"},
                {"spacecraft_id", 68},
                {"virtual_channel_id", 1},
                {"tm_frame_size", 1024}
            },
            .protocol_id = "tempest.engine",
            .protocol_version = 1,
            .engine_protocol = true,
            .create = [](const nlohmann::json& config) {
                return std::make_shared<FprimeTransport>(config);
            }
        });
    }
};
}

DARTWIC_PLUGIN_EXPORT DARTWIC::Plugins::BasePlugin* createPlugin(
    nlohmann::json config, DARTWIC::API::SDK_API* api) {
    return new FPrimeBridge::Plugin(std::move(config), api);
}
