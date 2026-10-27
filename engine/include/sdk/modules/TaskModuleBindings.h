#pragma once
#include "BaseModule.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace DARTWIC::API {
/** Typed task argument access during configuration. Missing values use the default;
 * malformed values throw with the argument name. Not intended for periodic callbacks.
 * @dartwic-reference @category Tasks and Loops */
template<class T> T taskArgument(const TaskRuntime& runtime, const std::string& key, T fallback = T{}) {
    const auto& arguments = runtime.getArguments();
    if (!arguments.contains(key)) return fallback;
    try { return arguments.at(key).get<T>(); }
    catch (const std::exception& error) { throw std::runtime_error("Invalid task argument `" + key + "`: " + error.what()); }
}

/** Resolves and retains a module at configuration time; rejects unavailable or
 * incompatible instances. The retained object owns lifetime, not device locking.
 * @dartwic-reference @category Modules */
template<class T> std::shared_ptr<T> taskModule(SDK_API* api, const TaskRuntime& runtime,
    const std::string& plugin_id, const std::string& module_type_id) {
    const auto name = taskArgument<std::string>(runtime, "module_instance_name");
    auto module = std::dynamic_pointer_cast<T>(api->getModuleInstance(name));
    const auto qualified = module_type_id.find('.') == std::string::npos ? plugin_id + "." + module_type_id : module_type_id;
    if (!module || module->getPluginId() != plugin_id ||
        (module->getModuleTypeId() != module_type_id && module->getModuleTypeId() != qualified))
        throw std::runtime_error("Module `" + name + "` is missing or incompatible with " + qualified + ".");
    return module;
}

/** Plugin-owned, configuration-only task/module association helper.
 *
 * bind wraps existing configure/cleanup callbacks without changing SDK ABI. Limits count
 * configured tasks, including stopped ones. Claims commit after successful
 * preparation and release on task destruction; ordinary cycles never enter it.
 * Device access remains synchronized by the module. @dartwic-reference @category Modules */
class TaskModuleBindings {
    using Key = std::pair<std::string, std::string>;
    struct State {
        std::mutex mutex;
        std::map<Key, std::set<std::string>> claims;
        std::map<std::string, Key> tasks;
        void release(const std::string& task) {
            std::scoped_lock lock(mutex);
            if (const auto found = tasks.find(task); found != tasks.end()) {
                auto claim = claims.find(found->second);
                if (claim != claims.end()) { claim->second.erase(task); if (claim->second.empty()) claims.erase(claim); }
                tasks.erase(found);
            }
        }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    /** Wraps a definition before registerTaskType. max_per_type=0 permits any
     * count; otherwise the slot is per module and runtime task type. prepare
     * receives the typed module and runtime. Throws before preparation when full.
     * Shared ownership keeps helper state alive through registered callbacks. */
    template<class Module, class Prepare>
    void bind(TaskTypeDefinition& definition, SDK_API* api, std::string plugin_id,
        std::string module_type_id, size_t max_per_type, Prepare prepare) {
        auto state = state_;
        auto previous_configure = definition.on_configure;
        auto previous_cleanup = definition.cleanup;
        definition.on_configure = [=](const TaskTypeDefinition& type, TaskRuntime& runtime) {
            auto module = taskModule<Module>(api, runtime, plugin_id, module_type_id);
            const auto task = runtime.getTaskName();
            const Key next{taskArgument<std::string>(runtime, "module_instance_name"), runtime.getTaskType()};
            bool inserted = false;
            {
                std::scoped_lock lock(state->mutex);
                auto& owners = state->claims[next];
                if (!owners.contains(task) && max_per_type && owners.size() >= max_per_type)
                    throw std::runtime_error("Module `" + next.first + "` already has " + next.second + " task `" + *owners.begin() + "`.");
                inserted = owners.insert(task).second;
            }
            try {
                if (previous_configure) previous_configure(type, runtime);
                prepare(module, runtime);
            } catch (...) {
                if (inserted) { std::scoped_lock lock(state->mutex); auto& owners = state->claims[next]; owners.erase(task); if (owners.empty()) state->claims.erase(next); }
                throw;
            }
            {
                std::scoped_lock lock(state->mutex);
                const auto old = state->tasks.find(task);
                if (old != state->tasks.end() && old->second != next) {
                    auto& owners = state->claims[old->second]; owners.erase(task);
                    if (owners.empty()) state->claims.erase(old->second);
                }
                state->tasks[task] = next;
            }
        };
        definition.cleanup = [=](TaskRuntime& runtime) {
            state->release(runtime.getTaskName());
            if (previous_cleanup) previous_cleanup(runtime);
        };
    }
};
}
