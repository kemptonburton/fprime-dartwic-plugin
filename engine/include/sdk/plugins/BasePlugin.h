#ifndef BASEPLUGIN_H
#define BASEPLUGIN_H

#include <modules/BaseModule.h>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#ifdef _WIN32
#define DARTWIC_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define DARTWIC_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace DARTWIC::Plugins {
    /**
     * Root object implemented by an engine plugin.
     *
     * Registration and runtime operations are both exposed by API::SDK_API.
     *
     * @dartwic-reference
     * @category Lifecycle
     */
    class BasePlugin {
    public:
        virtual ~BasePlugin() = default;
        BasePlugin(nlohmann::json cfg, API::SDK_API* drtw) : config(std::move(cfg)), dartwic(drtw) {}

        /** Registers the plugin's module types, task types, loops, functions, and extension operations. */
        virtual void onPluginLoaded() {}

        /** Creates a configured module instance for one of the plugin's registered module types. */
        virtual Modules::BaseModule* createModule(
            const std::string& module_type_id,
            nlohmann::json cfg,
            API::SDK_API* drtw
        ) {
            (void)module_type_id;
            (void)cfg;
            (void)drtw;
            return nullptr;
        }

        /** Stores the canonical plugin identifier assigned by the host. */
        void setPluginId(const std::string& plugin_id_value) {
            plugin_id = plugin_id_value;
        }

        /** Returns the canonical plugin identifier assigned by the host. */
        const std::string& getPluginId() const {
            return plugin_id;
        }

        nlohmann::json config = nlohmann::json::object();
        API::SDK_API* dartwic = nullptr;

    protected:
        std::string plugin_id;
    };
}

#endif // BASEPLUGIN_H
