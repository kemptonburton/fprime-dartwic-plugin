#ifndef SDK_API_H
#define SDK_API_H

#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "channel_references.h"

namespace TEMPEST { class Transport; class Peer; }

namespace DARTWIC::Modules {
    class BaseModule;
}

namespace DARTWIC::API {
    /** Portable shared-asset path below workspace/global_data. @dartwic-reference @category Workspace Assets */
    inline std::string workspaceAssetPath(const std::string& asset_namespace, const std::string& filename) {
        const auto safe_segment = [](const std::string& value) {
            if (value.empty() || value.find("..") != std::string::npos) return false;
            const auto alphanumeric = [](char c) {return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');};
            if (!alphanumeric(value.front())) return false;
            for (char c : value) if (!alphanumeric(c) && c != '_' && c != '-' && c != '.') return false;
            return true;
        };
        if (!safe_segment(asset_namespace) || !safe_segment(filename) || filename.find('.') == std::string::npos)
            throw std::invalid_argument("Asset namespace and filename must be safe path segments, with a file extension.");
        return "assets/" + asset_namespace + "/" + filename;
    }
    /** TEMPEST transport interface used by custom engine connections. */
    using Transport = TEMPEST::Transport;
    using TransportPtr = std::shared_ptr<TEMPEST::Transport>;

    /**
     * Execution shape used by a registered task type.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    enum class TaskStructure {
        Unknown,
        Periodic,
        StateMachine,
        Sequence,
        Timeline,
        Worker
    };

    /**
     * Addressable fields stored for each RAPID channel.
     *
     * @dartwic-reference
     * @category Channels
     */
    enum class ChannelField {
        VALUE,
        COMMANDED_BY,
        TIMESTAMP,
        UNITS,
        STALE_TIMEOUT,
        RECORD_MODE,
        DATA_FRAME,
        CONTROL_POLICY,
        CONTROL_OWNER,
        ACTIVE_CONTROLLER,
        LINKED_CALCULATION_SCRIPTS,
        VALUE_OPTIONS,
        STARTUP_VALUE
    };

    /**
     * Controls when channel values are recorded to persistence.
     *
     * @dartwic-reference
     * @category Channels
     */
    enum class RecordMode {
        OnValueChange,
        Never,
        EveryValue
    };

    /**
     * Authority policy applied to commandable channels.
     *
     * @dartwic-reference
     * @category Channel Authority
     */
    enum class ControlPolicy {
        Free,
        Automatic,
        ManualOverride,
        ObserveOnly
    };

    struct ChannelValueOption {
        double value = 0.0;
        std::string label;
    };

    struct ChannelCalculationLink {
        std::string script;
        std::string relationship;
        uint32_t line_number = 0;
    };

    struct StartupValue {
        bool enabled = false;
        double value = 0.0;
    };

    enum class ChannelStorage {
        Dynamic,
        Fixed
    };

    /**
     * Stable, pre-resolved address of a fixed RAPID channel.
     *
     * Handles are intentionally opaque to plugins. Resolve them during task setup and
     * reuse them in high-frequency callbacks to avoid channel-name lookup overhead.
     * @dartwic-reference
     * @category Channels
     */
    struct FixedChannelHandle {
        uint32_t slot = 0;
        uint64_t binding_generation = 0;

        [[nodiscard]] bool valid() const noexcept { return binding_generation != 0; }
    };

    /**
     * Ordered channel names and handles used by the fixed-channel batch APIs.
     * @dartwic-reference
     * @category Channels
     */
    struct FixedChannelBatch {
        std::vector<std::string> channels;
        std::vector<FixedChannelHandle> handles;

        [[nodiscard]] size_t size() const noexcept { return handles.size(); }
        [[nodiscard]] bool empty() const noexcept { return handles.empty(); }
    };

    /**
     * Value types accepted by channel read, write, and authority APIs.
     *
     * @dartwic-reference
     * @category Channels
     */
    using ChannelValue = std::variant<double, int, std::string, bool, RecordMode, ControlPolicy,
        std::vector<ChannelValueOption>, std::vector<ChannelCalculationLink>, StartupValue>;

    /**
     * Handler for a plugin-defined TEMPEST extension operation.
     *
     * Plugin-defined operations are not part of the official DARTWIC operation catalog.
     *
     * @dartwic-reference
     * @category Operations
     */
    using OperationHandler = std::function<nlohmann::json(const nlohmann::json& payload)>;

    /** A typed argument in a peer-visible command descriptor.
     * @dartwic-reference
     * @category Operations
     */
    struct OperationArgumentDefinition {
        std::string name;
        std::string type;
        std::string description;
        bool required = false;
        std::optional<nlohmann::json> default_value;
        std::vector<nlohmann::json> choices;
    };
    /** Registers an executable plugin operation and its operator-facing metadata.
     * @dartwic-reference
     * @category Operations
     */
    struct OperationDefinition {
        std::string id;
        std::string name;
        std::string description;
        std::string category;
        std::vector<OperationArgumentDefinition> arguments;
        OperationHandler handler;
        bool allow_viewers = false;
    };

    /** Declares an operator-visible telemetry topic before its first publication.
     * @dartwic-reference
     * @category Operations
     */
    struct TelemetryDefinition {
        std::string id;
        std::string name;
        std::string description;
        std::string delivery;
    };

    /**
     * Native callback exposed to DCode through the plugin SDK.
     *
     * @dartwic-reference
     * @category DCode
     */
    using DCodeFunctionHandler = std::function<nlohmann::json(const nlohmann::json& payload)>;

    /**
     * Describes one input or output in a plugin-provided DCode function.
     *
     * @dartwic-reference
     * @category DCode
     */
    struct DCodeFunctionArgument {
        std::string name;
        std::string type;
        std::string doc;
        bool required = false;
    };

    struct TaskTypeDefinition;

    /**
     * Identity, structure, presentation, and defaults for a plugin task type.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    struct TaskTypeMetadata {
        std::string task_type;
        TaskStructure structure = TaskStructure::Unknown;
        std::string icon_url;
        std::string exposed_from;
        std::string expected_plugin_id;
        nlohmann::json default_arguments = nlohmann::json::object();
    };

    /**
     * Live task context passed to plugin task lifecycle callbacks.
     *
     * Runtime context values persist for the lifetime of one task runtime and can be
     * used to share plugin-owned state between start, task, end, and cleanup callbacks.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    class TaskRuntime {
    public:
        virtual ~TaskRuntime() = default;

        virtual const std::string& getTaskName() const = 0;
        virtual const std::string& getTaskType() const = 0;
        virtual const nlohmann::json& getMetadata() const = 0;
        virtual const nlohmann::json& getArguments() const = 0;
        virtual double getElapsedSeconds() const = 0;
        virtual bool isStopRequested() const = 0;

        virtual void setRuntimeContext(const std::string& key, std::shared_ptr<void> value) = 0;
        virtual std::shared_ptr<void> getRuntimeContext(const std::string& key) const = 0;
        virtual void removeRuntimeContext(const std::string& key) = 0;
        virtual void clearRuntimeContext() = 0;
        // Keep new virtual functions appended so plugins built against the previous
        // TaskRuntime vtable retain the indices of all existing functions.
        /** Declares this task's fixed snapshot inputs from on_configure. Every declared name
         * must refer to fixed storage when configuration completes; missing/dynamic inputs
         * fail task preparation. An empty list requests an empty fixed input snapshot.
         * Calls from other callbacks throw rather than changing an active plan. */
        virtual void setFixedInputChannels(std::vector<std::string> channels) = 0;

        /**
         * Records one completed logical iteration of a long-running worker task.
         *
         * Worker callbacks own their internal loop, so the engine cannot infer
         * individual iterations from callback returns. Plugins should call this
         * once after each successful worker iteration. The engine samples the
         * counter to publish the task's worker-rate diagnostic.
         */
        virtual void recordWorkerCycle() = 0;

        template <typename T>
        void setTypedRuntimeContext(const std::string& key, const std::shared_ptr<T>& value) {
            setRuntimeContext(key, std::static_pointer_cast<void>(value));
        }

        template <typename T>
        std::shared_ptr<T> getTypedRuntimeContext(const std::string& key) const {
            return std::static_pointer_cast<T>(getRuntimeContext(key));
        }
    };

    /**
     * Callback used for task start and end lifecycle phases.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    using TaskLifecycleFunction = std::function<void(const TaskTypeDefinition&, TaskRuntime&)>;
    /**
     * Callback used for repeated task execution with elapsed seconds.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    using TaskLoopFunction = std::function<void(const TaskTypeDefinition&, TaskRuntime&, double)>;
    using TaskMissedFunction = std::function<void(const TaskTypeDefinition&, TaskRuntime&, uint64_t, double)>;
    /**
     * Callback used to release runtime state after task execution ends.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    using TaskCleanupFunction = std::function<void(TaskRuntime&)>;

    /**
     * Complete registration definition for a plugin task type.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     */
    struct TaskTypeDefinition {
        TaskTypeMetadata metadata;
        TaskLifecycleFunction on_configure;
        TaskLifecycleFunction on_start;
        TaskLoopFunction on_task;
        TaskMissedFunction on_missed;
        TaskLifecycleFunction on_end;
        TaskCleanupFunction cleanup;
        // Suppress timing warnings; scheduling, measurements and callback errors remain active.
        void setDisableWarnings(bool disabled = true) {
            metadata.default_arguments["disable_warnings"] = disabled;
        }
    };

    /**
     * Lightweight identity returned when enumerating live module instances.
     *
     * @dartwic-reference
     * @category Modules
     */
    struct ModuleInstanceSummary {
        std::string name;
        std::string plugin_id;
        std::string module_type_id;
        std::string resource_path;
    };

    /**
     * Declares a module type that a plugin can instantiate.
     *
     * @dartwic-reference
     * @category Modules
     */
    struct ModuleTypeDefinition {
        std::string id;
        std::string name;
        std::string config_path = "module_config.json";
        std::string default_parameters_path = "default_parameters.json";
    };

    /** One operator-selectable peer with a fixed protocol and transport.
     * @dartwic-reference
     * @category TEMPEST
     */
    struct PeerDefinition {
        std::string id;
        std::string name;
        uint64_t version = 1;
        nlohmann::json default_config = nlohmann::json::object();
        std::string protocol_id = "tempest.peer";
        uint64_t protocol_version = 1;
        bool engine_protocol = false;
        std::function<TransportPtr(const nlohmann::json& config)> create;
        std::function<void(TEMPEST::Peer& peer)> configure;
    };

    /**
     * Lifecycle callbacks and optional target frequency for a plugin-owned loop.
     *
     * The loop is controlled by CAESAR and stops with the engine.
     *
     * @dartwic-reference
     * @category Tasks and Loops
     * @example dartwic->registerLoop("heartbeat", "Heartbeat", {
     *     .on_loop = []() { publishHeartbeat(); },
     *     .target_frequency_hz = 10.0,
     * });
     */
    struct PluginLoopDefinition {
        std::function<void()> on_start;
        std::function<void()> on_loop;
        std::function<void()> on_end;
        std::optional<double> target_frequency_hz;
    };

    /**
     * Host API available to engine plugins and their module instances.
     *
     * Registration functions qualify local identifiers as `<plugin-id>.<local-id>`.
     * Channel authority calls require an active task or plugin loop controller.
     *
     * @dartwic-reference
     * @category Lifecycle
     */
    class SDK_API {
    public:
        SDK_API() = default;
        virtual ~SDK_API() = default;

        /**
         * Reads a typed field and returns the supplied default when the field is unavailable.
         * @dartwic-reference
         * @category Channels
         * @param channel Flat channel name.
         * @param field Field to read.
         * @param default_value Value used when the field is unavailable.
         * @returns The numeric field value.
         */
        virtual double queryChannelField(const std::string& channel,
            ChannelField field,
            std::optional<ChannelValue> default_value) = 0;

        /**
         * Creates a missing channel with an initial field value; an existing channel is left unchanged.
         *
         * This is a no-op for an existing channel, including its storage class.
         * To promote existing storage, use upsertChannelField with Fixed storage during configuration.
         * @dartwic-reference
         * @category Channels
         * @param channel Flat channel name.
         * @param field Field to insert.
         * @param value Initial field value.
         * @param storage Storage used for a newly created channel; defaults to dynamic.
         */
        virtual void insertChannelField(const std::string& channel,
            ChannelField field,
            ChannelValue value,
            ChannelStorage storage = ChannelStorage::Dynamic) = 0;

        /**
         * Creates or replaces a field on a RAPID channel.
         * @dartwic-reference
         * @category Channels
         * @param channel Flat channel name.
         * @param field Field to write.
         * @param value New field value.
         * @param storage Storage used only when creating or promoting the channel; defaults to dynamic.
         */
        virtual void upsertChannelField(const std::string& channel,
            ChannelField field,
            ChannelValue value,
            ChannelStorage storage = ChannelStorage::Dynamic) = 0;

        /**
         * Creates a channel in fixed RAPID storage, or promotes an existing dynamic channel.
         *
         * Call during startup configuration or a task's on_configure callback. The engine's
         * declared configuration transaction permits layout updates after active task snapshots
         * drain, even when the layout is sealed. Direct layout changes outside that boundary
         * remain restricted. This writes initial_value; ordinary subsequent field writes
         * preserve fixed storage. Do not recreate the channel in every acquisition callback.
         *
         * @dartwic-reference
         * @category Channels
         * @param channel Flat channel name.
         * @param initial_value Initial numeric value; defaults to zero.
         */
        void createFixedChannel(const std::string& channel, double initial_value = 0.0);

        /**
         * Removes a channel and its associated field data.
         * @dartwic-reference
         * @category Channels
         * @param channel Channel to remove.
         * @returns Whether a channel was removed.
         */
        virtual bool removeChannel(const std::string& channel) = 0;

        /**
         * Registers a plugin-local module type and returns its qualified identifier.
         * @dartwic-reference
         * @category Modules
         * @param definition Module type metadata and file paths.
         * @returns The plugin-qualified module type identifier.
         */
        virtual std::string registerModuleType(ModuleTypeDefinition definition) = 0;
        /**
         * Registers a plugin-qualified peer definition with one transport factory.
         * @dartwic-reference
         * @category TEMPEST
         * @param definition Local ID, display name, editable defaults, protocol, transport, and handlers.
         * @returns The plugin-qualified peer ID used by tempest/peers/connect.
         */
        virtual std::string registerPeer(PeerDefinition definition) = 0;
        /**
         * Registers a plugin-local task type and returns its qualified identifier.
         * @dartwic-reference
         * @category Tasks and Loops
         * @param local_id Identifier unique within the plugin.
         * @param name Operator-facing task type name.
         * @param definition Task metadata and lifecycle callbacks.
         * @returns The plugin-qualified task type identifier.
         */
        virtual std::string registerTaskType(std::string local_id, std::string name, TaskTypeDefinition definition) = 0;
        /**
         * Registers a plugin extension operation; it does not become an official DARTWIC operation.
         * @dartwic-reference
         * @category Operations
         * @param definition Local ID, display metadata, argument fields, and handler.
         * @returns The plugin-qualified operation identifier.
         */
        virtual std::string registerOperation(OperationDefinition definition) = 0;
        // Convenience for operations without argument metadata. Delegates to the
        // registered definition and does not add a virtual slot to the SDK ABI.
        std::string registerOperation(std::string local_id, std::string name, OperationHandler handler) {
            OperationDefinition definition;
            definition.id = std::move(local_id);
            definition.name = std::move(name);
            definition.handler = std::move(handler);
            return registerOperation(std::move(definition));
        }
        /**
         * Registers a plugin-local telemetry topic for the runtime catalog.
         * The plugin namespace supplies the qualified topic and operator category.
         * @dartwic-reference
         * @category Operations
         * @param definition Local topic ID and operator-facing metadata.
         * @returns The plugin-qualified topic.
         */
        virtual std::string registerTelemetry(TelemetryDefinition definition) = 0;
        /**
         * Calls a registered TEMPEST operation on the current node or a connected peer.
         * Payloads use JSON at the engine SDK boundary; peers use Value.
         * A remote timeout reports unknown completion and never retries the operation.
         * @dartwic-reference
         * @category Operations
         * @param node Current engine node name or connected remote node name.
         * @param operation Fully qualified operation name, such as fprime/command.
         * @param payload Operation arguments.
         * @returns The operation result; throws on local failure, disconnection, timeout, or remote failure.
         */
        virtual nlohmann::json callTempest(const std::string& node, const std::string& operation,
                                           const nlohmann::json& payload) {
            throw std::runtime_error("TEMPEST peer calls are unavailable in this SDK host.");
        }
        /**
         * Publishes telemetry on a plugin-qualified topic to connected clients and peers.
         * Delivery is best effort; subscribers may miss samples while disconnected.
         * @dartwic-reference
         * @category Operations
         * @param topic Plugin-local topic name.
         * @param payload Object containing the telemetry values.
         */
        virtual void publishTelemetry(const std::string& topic, const nlohmann::json& payload) {
            throw std::runtime_error("TEMPEST telemetry is unavailable in this SDK host.");
        }
        /**
         * Registers a native DCode function together with its editor-facing argument documentation.
         * @dartwic-reference
         * @category DCode
         * @param local_id Identifier unique within the plugin.
         * @param name Operator-facing function name.
         * @param handler JSON function callback.
         * @param doc Function documentation shown by DCode tooling.
         * @param input_arguments Structured input documentation.
         * @param output_arguments Structured output documentation.
         * @returns The plugin-qualified DCode function identifier.
         */
        virtual std::string registerDCodeFunction(
            std::string local_id,
            std::string name,
            DCodeFunctionHandler handler,
            std::string doc = {},
            std::vector<DCodeFunctionArgument> input_arguments = {},
            std::vector<DCodeFunctionArgument> output_arguments = {}
        ) = 0;
        /**
         * Registers a CAESAR-controlled plugin loop and returns its qualified identifier.
         * @dartwic-reference
         * @category Tasks and Loops
         * @param local_id Identifier unique within the plugin.
         * @param name Operator-facing loop name.
         * @param definition Loop lifecycle and target frequency.
         * @returns The plugin-qualified loop identifier.
         */
        virtual std::string registerLoop(std::string local_id, std::string name, PluginLoopDefinition definition) = 0;

        /**
         * Raises an operator-visible error and returns the created event identifier.
         * @dartwic-reference
         * @category Events
         * @param message_title Short error title.
         * @param message_description Detailed error description.
         * @param tags Searchable event tags.
         * @param resolution Suggested operator resolution.
         * @param auto_acknowledge Non-zero to acknowledge automatically.
         * @returns The created ARGUS event identifier.
         */
        virtual int consoleError(
            std::string message_title,
            std::string message_description,
            std::vector<std::string> tags,
            std::string resolution,
            int auto_acknowledge = 0
        ) = 0;

        /**
         * Returns a live module instance by configured instance name.
         * @dartwic-reference
         * @category Modules
         * @param instance_name Configured module instance name.
         * @returns The live module instance, or null when it is unavailable.
         */
        virtual std::shared_ptr<Modules::BaseModule> getModuleInstance(const std::string& instance_name) = 0;
        /**
         * Lists live module instances, optionally limited to one plugin.
         * @dartwic-reference
         * @category Modules
         * @param plugin_id Optional canonical plugin identifier.
         * @returns Matching live module summaries.
         */
        virtual std::vector<ModuleInstanceSummary> getModuleInstances(const std::string& plugin_id = "") = 0;

        /**
         * Writes timestamped numeric samples to one channel in a single call.
         * @dartwic-reference
         * @category Channels
         * @param channel Channel receiving the samples.
         * @param data Value and Unix-nanosecond timestamp pairs.
         */
        virtual void upsertChannelValueBulk(const std::string& channel,
            const std::vector<std::pair<double, uint64_t>>& data) = 0;

        /**
         * Calls a registered native DCode function with a JSON payload.
         * @dartwic-reference
         * @category DCode
         * @param function_name Qualified or built-in function name.
         * @param payload Function arguments encoded as JSON.
         * @returns Function result encoded as JSON.
         */
        virtual nlohmann::json callDCodeFunction(const std::string& function_name, const nlohmann::json& payload) = 0;

        /**
         * Commands a channel while using the active task or loop as controller.
         * @dartwic-reference
         * @category Channel Authority
         * @param channel Flat channel name.
         * @param value Commanded value.
         */
        virtual void commandChannel(const std::string& channel, ChannelValue value) = 0;
        /**
         * Establishes observe-only authority for the active task or loop, optionally writing its value.
         * @dartwic-reference
         * @category Channel Authority
         * @param channel Flat channel name.
         * @param value Optional value; omitting it preserves the current numeric value.
         */
        virtual void setChannel(const std::string& channel, std::optional<ChannelValue> value = std::nullopt) = 0;
        /**
         * Claims channel authority for the active task or loop controller.
         * @dartwic-reference
         * @category Channel Authority
         * @param channel Flat channel name.
         */
        virtual void claimChannel(const std::string& channel) = 0;
        /**
         * Releases authority previously claimed by the active controller.
         * @dartwic-reference
         * @category Channel Authority
         * @param channel Flat channel name.
         */
        virtual void freeChannel(const std::string& channel) = 0;

        /**
         * Creates or refreshes a correlated ARGUS event from a JSON declaration.
         *
         * Supported fields include type, title, description, resolution, system,
         * subsystem, channels, actions, payload, correlation_key, and
         * auto_acknowledge_seconds. A stable correlation_key updates one event
         * instead of creating a new event for each heartbeat.
         *
         * Optional `graphs` accepts an array of graph groups. Each group may be an array of shorthand
         * strings or an object with a `series` array and optional `title` / `window_seconds` fields.
         * Shorthand series use `|channel|`, `>value`, `>=value`, `<value`, `<=value`, or `=value` and
         * may append `@1` through `@5` to select a Y axis. Object series accept `channel_reference`
         * (or `expression`) plus `y_axis`, `label`, and `color`. For example:
         * `{"graphs":[["|temperature|@1",">100@1"],["|pressure|@1","=50@2"]]}`.
         * @dartwic-reference
         * @category Events
         * @param event Event declaration to create or refresh.
         * @returns The complete accepted ARGUS event record.
         */
        virtual nlohmann::json recordEvent(nlohmann::json event) { return nlohmann::json::object(); }

        /**
         * Updates the lifecycle status of an ARGUS event by event identifier.
         * @param event_id Event identifier to update.
         * @param status New lifecycle status.
         */
        virtual bool updateEventStatus(const std::string& event_id, const std::string& status) {
            (void)event_id;
            (void)status;
            return false;
        }

        /**
         * Resolves fixed channels during configuration to avoid repeated name lookup in callbacks.
         * This does not make the complete callback, write wrapper, or commit allocation-free.
         * Throws when a name is missing or does not refer to fixed storage.
         * @dartwic-reference
         * @category Channels
         * @param channels Ordered fixed-channel names to resolve.
         */
        virtual FixedChannelBatch resolveFixedChannels(const std::vector<std::string>& channels) = 0;

        /**
         * Reads a coherent task-input snapshot into caller-owned storage.
         *
         * Declare these inputs with TaskRuntime::setFixedInputChannels during configuration.
         * Outside a task snapshot, reads observe live values instead. Undeclared inputs in an
         * ordinary partial snapshot also use live fallback, so declare every fixed input.
         * Re-resolve after configuration changes; stale handles return the supplied fallback.
         * @dartwic-reference
         * @category Channels
         * @param batch Previously resolved fixed-channel batch.
         * @param destination Caller-owned output span with one element per channel.
         * @param default_value Value used when a fixed-channel value is unavailable.
         */
        virtual void queryFixedChannelValues(const FixedChannelBatch& batch,
            std::span<double> destination,
            double default_value = 0.0) = 0;

        /**
         * Stages one value per fixed channel in the current task transaction.
         *
         * Preserves ordinary command-authority checks. This does not claim channel ownership.
         * Outside a task transaction, values are written immediately through the ordinary
         * path; the call alone does not establish a coherent multi-channel transaction.
         * Reuse the batch and value buffer. Staging, attribution, commit, inline reactions,
         * and recording still have costs beyond resolved-handle access.
         * @dartwic-reference
         * @category Channels
         * @param batch Previously resolved fixed-channel batch.
         * @param values Input span with one value per channel.
         * @param timestamp Optional Unix-epoch timestamp in nanoseconds.
         */
        virtual void upsertFixedChannelValues(const FixedChannelBatch& batch,
            std::span<const double> values,
            std::optional<uint64_t> timestamp = std::nullopt) = 0;

        /**
         * Opens or updates a named interface workflow without blocking the plugin loop.
         * The returned object contains a request_id which can be queried for its typed JSON result.
         * @param ui_id Plugin-local interface workflow identifier.
         * @param payload Workflow request payload.
         * @param options Optional workflow settings; severity is message (default), warning, or error.

         * @dartwic-reference
         * @category Interface UI and Notifications
         */
        virtual nlohmann::json requestInterfaceUi(
            const std::string& ui_id,
            nlohmann::json payload,
            nlohmann::json options = nlohmann::json::object()) {
            (void)ui_id;
            (void)payload;
            (void)options;
            return nlohmann::json::object();
        }

        /**
         * Returns the current status and result of a named interface workflow request.
         * @param request_id Interface workflow request identifier.
         */
        virtual nlohmann::json getInterfaceUiRequest(const std::string& request_id) {
            (void)request_id;
            return nlohmann::json::object();
        }

        /**
         * Announces a device found by a plugin-owned discovery loop.
         * @param candidate Discovered-device identity and connection metadata.
         */
        virtual nlohmann::json announceDiscoveredDevice(nlohmann::json candidate) {
            (void)candidate;
            return nlohmann::json::object();
        }

        /**
         * Returns whether an engine-scoped notification owned by this plugin is muted.
         * The host qualifies the plugin-local notification ID before reading global engine state.
         * Calling this method also marks the mute rule as relevant for expiry purposes.
         * @param notification_id Stable plugin-local notification identity.
         */
        virtual bool isNotificationMuted(const std::string& notification_id) {
            (void)notification_id;
            return false;
        }

        /**
         * Appends text to an ARGUS log stream without creating an operator event.
         * Plugin calls use a plugin-qualified stream name, such as `ethercat/Bus`.
         * The engine supplies the node, session, timestamp, and sequence. Writes
         * are asynchronous and return false if the bounded queue is full.
         * @dartwic-reference
         * @category Logs
         * @param stream Plugin-local stream name, such as `Bus`.
         * @param text Text to append; a trailing newline is optional.
         * @param channel `stdout` or `stderr` for console-style coloring.
         * @param level `info`, `warning`, or `error` for filtering.
         * @returns Whether ARGUS accepted the text for writing.
         * @example api.writeLog("Bus", "Device connected", "stdout", "info");
         */
        virtual bool writeLog(const std::string& stream, const std::string& text,
            const std::string& channel = "stdout", const std::string& level = "info") {
            (void)stream;
            (void)text;
            (void)channel;
            (void)level;
            return false;
        }

        /** Optional channel references held only in plugin-private memory.
         * Register during onPluginLoaded. The callback runs during a Project
         * audit, never in a task loop. Add exact references or scan structured
         * plugin data with the sink. Ordinary task data, fixed bindings,
         * project files, and channel origins are found automatically.
         * New virtual methods belong at the end of SDK_API so plugins compiled
         * against an earlier SDK keep the same virtual method positions.
         */
        virtual void registerChannelReferenceSource(std::string local_id,
            std::function<void(ChannelReferenceSink&)> collect) {
            (void)local_id;
            (void)collect;
            throw std::runtime_error("Channel reference sources are unavailable in this SDK host.");
        }

        /**
         * Calls a saved pinned operation on the current engine or a connected remote node once.
         * @dartwic-reference
         * @category Operations
         * @param node Current engine node name or connected remote node name.
         * @param preset Stable pinned-operation ID or unambiguous label for that node.
         * @param overrides Object merged over saved operation arguments.
         * @returns Operation result; throws if the preset is missing, ambiguous, or invocation fails.
         */
        virtual nlohmann::json callPinnedTempest(const std::string& node, const std::string& preset,
                                                  const nlohmann::json& overrides = nlohmann::json::object()) {
            throw std::runtime_error("Pinned TEMPEST peer calls are unavailable in this SDK host.");
        }

        /** Reads a shared text/JSON asset from the selected engine workspace.
         * @dartwic-reference
         * @category Workspace Assets
         * @param node Target engine node name.
         * @param asset_namespace Shared asset namespace.
         * @param filename Asset filename including extension.
         * @returns Text content of the asset.
         */
        std::string readWorkspaceAsset(const std::string& node, const std::string& asset_namespace, const std::string& filename) {
            return callTempest(node, "dartwic/get-file", {{"rootDir", "global_data_directory"},
                {"filePath", workspaceAssetPath(asset_namespace, filename)}}).at("content").get<std::string>();
        }

        /** Saves a shared text/JSON asset and returns its portable reference. Keep binary data in a base64 package.
         * @dartwic-reference
         * @category Workspace Assets
         * @param node Target engine node name.
         * @param asset_namespace Shared asset namespace.
         * @param filename Asset filename including extension.
         * @param content Text or JSON package to save.
         * @returns Workspace-relative asset reference.
         */
        std::string saveWorkspaceAsset(const std::string& node, const std::string& asset_namespace,
                                      const std::string& filename, const std::string& content) {
            const auto path = workspaceAssetPath(asset_namespace, filename);
            callTempest(node, "dartwic/save-file", {{"rootDir", "global_data_directory"}, {"path", path}, {"content", content}});
            return path;
        }

        /** Resolves a local path below a named root.
         * @dartwic-reference
         * @category Storage
         * @param scope installation, instance, workspace, project, settings, runtime or cache.
         * @param relative Relative path below the selected root.
         * @param project Project name; empty selects the active project.
         * @returns Absolute path on the engine computer.
         */
        virtual std::string storagePath(const std::string& scope, const std::string& relative,
                                        const std::string& project = "") {
            throw std::runtime_error("Storage path helpers are unavailable in this host.");
        }
        std::filesystem::path installationPath(const std::string& path = "") { return storagePath("installation", path); }
        std::filesystem::path instancePath(const std::string& path = "") { return storagePath("instance", path); }
        std::filesystem::path workspacePath(const std::string& path = "") { return storagePath("workspace", path); }
        std::filesystem::path projectPath(const std::string& path = "", const std::string& project = "") { return storagePath("project", path, project); }
        std::filesystem::path projectSettingsPath(const std::string& path = "", const std::string& project = "") { return storagePath("settings", path, project); }
        std::filesystem::path projectRuntimePath(const std::string& path = "", const std::string& project = "") { return storagePath("runtime", path, project); }
        std::filesystem::path projectCachePath(const std::string& path = "", const std::string& project = "") { return storagePath("cache", path, project); }
        /** Returns effective policy plus its workspace/project overrides and sources.
         * @dartwic-reference
         * @category Storage
         * @param defaults Caller defaults overlaid by portable policy.
         * @param project Project name; empty selects the active project.
         * @returns Effective settings, stored layers, leaf sources and revision.
         */
        virtual nlohmann::json readEffectiveSettings(const nlohmann::json& defaults = nlohmann::json::object(), const std::string& project = "") {
            throw std::runtime_error("Settings helpers are unavailable in this host.");
        }
        /** Merges a portable settings override; reset contains JSON pointers to remove.
         * @dartwic-reference
         * @category Storage
         * @param scope workspace or project.
         * @param patch Intentional settings overrides.
         * @param reset JSON pointers to remove from this scope.
         * @param project Project name; empty selects the active project.
         * @param revision Optional revision token to reject stale edits.
         * @returns Updated settings snapshot without caller defaults.
         */
        virtual nlohmann::json writeSettingsOverride(const std::string& scope, const nlohmann::json& patch,
            const nlohmann::json& reset = nlohmann::json::array(), const std::string& project = "", const std::string& revision = "") {
            throw std::runtime_error("Settings helpers are unavailable in this host.");
        }
        nlohmann::json resetSettingsOverride(const std::string& scope, const nlohmann::json& pointers,
            const std::string& project = "", const std::string& revision = "") {
            return writeSettingsOverride(scope, nlohmann::json::object(), pointers, project, revision);
        }
        /** Reads JSON below a storage root.
         * @dartwic-reference
         * @category Storage
         * @param scope Named storage root.
         * @param path Relative JSON filename.
         * @param missing Value returned when the file does not exist.
         * @param project Project name; empty selects the active project.
         * @returns Parsed JSON; corrupt and unreadable files throw.
         */
        virtual nlohmann::json readStorageJson(const std::string& scope, const std::string& path,
            const nlohmann::json& missing = nullptr, const std::string& project = "") {
            throw std::runtime_error("JSON storage helpers are unavailable in this host.");
        }
        /** Atomically saves JSON below a storage root.
         * @dartwic-reference
         * @category Storage
         * @param scope Named storage root.
         * @param path Relative JSON filename.
         * @param value JSON document to save.
         * @param project Project name; empty selects the active project.
         */
        virtual void writeStorageJson(const std::string& scope, const std::string& path,
            const nlohmann::json& value, const std::string& project = "") {
            throw std::runtime_error("JSON storage helpers are unavailable in this host.");
        }

    };

    inline void SDK_API::createFixedChannel(const std::string& channel, double initial_value) {
        upsertChannelField(
            channel,
            ChannelField::VALUE,
            ChannelValue{initial_value},
            ChannelStorage::Fixed
        );
    }
}

#endif //SDK_API_H
