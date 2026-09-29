#pragma once

#include <plugins/BasePlugin.h>

class StarterEnginePlugin final : public DARTWIC::Plugins::BasePlugin {
public:
    StarterEnginePlugin(nlohmann::json cfg, DARTWIC::API::SDK_API* api)
        : BasePlugin(std::move(cfg), api) {}

    void onPluginLoaded() override {
        dartwic->registerModuleType({.id = "starter", .name = "Starter Module"});
    }
};
