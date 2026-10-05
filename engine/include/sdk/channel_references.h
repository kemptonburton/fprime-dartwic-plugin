#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <regex>
#include <nlohmann/json.hpp>

namespace DARTWIC::API {

// A small, read-only result builder for plugin data that is invisible to the
// engine's automatic task, file, and channel-origin scans.
class ChannelReferenceSink {
public:
    explicit ChannelReferenceSink(std::vector<std::string> requested)
        : requested_(std::move(requested)) {}

    void addExact(const std::string& channel, const std::string& kind,
        const std::string& source, const std::string& location,
        const std::string& role, const std::string& required_action = {},
        const std::string& operation = {},
        nlohmann::json operation_payload = nlohmann::json::object(),
        nlohmann::json change = nlohmann::json::object()) {
        if (std::find(requested_.begin(), requested_.end(), channel) == requested_.end()) return;
        auto reference = nlohmann::json{{"channel", channel}, {"kind", kind},
            {"source", source}, {"location", location}, {"role", role},
            {"uncertain", false}};
        if (!required_action.empty()) reference["required_action"] = required_action;
        if (!operation.empty()) {
            reference["operation"] = operation;
            reference["operation_payload"] = std::move(operation_payload);
        }
        if (!change.empty()) reference["change"] = std::move(change);
        references_.push_back(std::move(reference));
    }

    // A current resource owns or publishes this channel. The callback should
    // report only resources that still exist, including configured offline peers.
    void addOwner(const std::string& channel, const std::string& kind,
        const std::string& source, const std::string& role,
        nlohmann::json change = nlohmann::json::object()) {
        if (std::find(requested_.begin(), requested_.end(), channel) == requested_.end()) return;
        auto owner = nlohmann::json{{"channel", channel}, {"kind", kind},
            {"source", source}, {"role", role}};
        if (!change.empty()) owner["change"] = std::move(change);
        owners_.push_back(std::move(owner));
    }

    // Recursively checks strings and channel-named JSON fields. Plain strings
    // outside channel fields are possible matches, not asserted bindings.
    void scanJson(const std::string& kind, const std::string& source,
        const nlohmann::json& value, const std::string& pointer = {}) {
        scanJsonValue(kind, source, value, pointer, false);
    }

    void markIncomplete(std::string reason) {
        complete_ = false;
        error_ = std::move(reason);
    }

    // Absence of addOwner is authoritative only for these resource inventories.
    // Report every owned channel requested by this audit before marking complete.
    void completeOwnership(const std::string& kind, const std::string& source) {
        ownership_coverage_.push_back({{"kind", kind}, {"source", source}});
    }

    nlohmann::json report() const {
        return {{"complete", complete_}, {"references", references_},
            {"owners", owners_}, {"ownership_coverage", ownership_coverage_}, {"error", error_}};
    }

private:
    std::vector<std::string> requested_;
    nlohmann::json references_ = nlohmann::json::array();
    nlohmann::json owners_ = nlohmann::json::array();
    nlohmann::json ownership_coverage_ = nlohmann::json::array();
    bool complete_ = true;
    std::string error_;

    static std::string pointerSegment(std::string value) {
        size_t pos = 0;
        while ((pos = value.find('~', pos)) != std::string::npos) {
            value.replace(pos, 1, "~0"); pos += 2;
        }
        pos = 0;
        while ((pos = value.find('/', pos)) != std::string::npos) {
            value.replace(pos, 1, "~1"); pos += 2;
        }
        return value;
    }

    static bool templateMayMatch(const std::string& pattern, const std::string& channel) {
        const auto open = pattern.find('{');
        const auto close = pattern.rfind('}');
        if (open == std::string::npos || close == std::string::npos || close < open) return false;
        const auto prefix = pattern.substr(0, open);
        const auto suffix = pattern.substr(close + 1);
        return channel.size() >= prefix.size() + suffix.size() &&
            channel.starts_with(prefix) && channel.ends_with(suffix);
    }

    void scanJsonValue(const std::string& kind, const std::string& source,
        const nlohmann::json& value, const std::string& pointer, bool channel_field) {
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            for (const auto& channel : requested_) {
                const auto token = "|" + channel + "|";
                const bool exact = text == channel || text.find(token) != std::string::npos;
                bool possible = !exact && channel_field && templateMayMatch(text, channel);
                if (!exact && !possible) {
                    static const std::regex pipe_reference(R"(\|([^|]+)\|)");
                    for (std::sregex_iterator ref(text.begin(), text.end(), pipe_reference), end;
                         ref != end; ++ref) {
                        if (templateMayMatch((*ref)[1].str(), channel)) { possible = true; break; }
                    }
                }
                if (!exact && !possible) continue;
                references_.push_back({{"channel", channel}, {"kind", kind},
                    {"source", source}, {"location", pointer},
                    {"role", possible ? "templated_reference" : "configured_reference"},
                    {"uncertain", possible || (text == channel && !channel_field)},
                    {"matched_text", text}});
            }
        } else if (value.is_array()) {
            for (size_t index = 0; index < value.size(); ++index)
                scanJsonValue(kind, source, value[index], pointer + "/" + std::to_string(index), channel_field);
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                const auto& key = it.key();
                const bool named = key == "channel" || key == "channels" ||
                    key == "channel_name" || key == "channel_names" ||
                    key == "target_channel" || key == "dependency_channels" ||
                    key.ends_with("_channel") || key.ends_with("_channels");
                scanJsonValue(kind, source, it.value(), pointer + "/" + pointerSegment(key),
                    channel_field || named);
            }
        }
    }
};

} // namespace DARTWIC::API
