#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace DARTWIC::API {
// Describes resources created by the owning module or suggested task lifecycle.
// This is presentation metadata, not a request to create channels or loops.
// Names may contain {instance}, resolved by the shared setup UI.
struct DiscoveryChannel {
    std::string name;
    std::string label;
    std::string direction;
};
struct DiscoveryLoop {
    std::string name;
    std::string label;
};
struct DiscoveryResources {
    std::vector<DiscoveryChannel> channels;
    std::vector<DiscoveryLoop> loops;
    nlohmann::json toJson() const {
        auto result = nlohmann::json{{"channels", nlohmann::json::array()}, {"loops", nlohmann::json::array()}};
        for (const auto& channel : channels) result["channels"].push_back({
            {"name", channel.name}, {"label", channel.label}, {"direction", channel.direction}});
        for (const auto& loop : loops) result["loops"].push_back({{"name", loop.name}, {"label", loop.label}});
        return result;
    }
};
}
