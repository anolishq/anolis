/**
 * @file runtime.cpp
 * @brief Implementation of staged runtime bring-up, supervision, and provider restart flows.
 *
 * Runtime startup is intentionally staged and fail-fast: providers are started,
 * devices are discovered, ownership claims are validated, and the state cache
 * is primed before the main loop begins accepting normal traffic.
 */

#include "runtime.hpp"

#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

#if ANOLIS_ENABLE_AUTOMATION
#include "automation/bt_runtime.hpp"
#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "automation/parameter_types.hpp"
#endif
#include "control/provider_value_bridge.hpp"
#include "health/health_snapshot.hpp"
#include "logging/logger.hpp"
#include "ownership_validation.hpp"
#include "provider/provider_handle.hpp"  // Required for instantiation
#include "provider/provider_supervisor.hpp"
#include "runtime/actuation_gate.hpp"
#include "signal_handler.hpp"

namespace anolis {
namespace runtime {

Runtime::Runtime(const RuntimeConfig &config) : config_(config) {}

Runtime::~Runtime() { shutdown(); }

bool Runtime::initialize(std::string &error) {
    LOG_INFO("[Runtime] Initializing Anolis Core");
    const auto startup_begin = std::chrono::steady_clock::now();
    const int startup_timeout_ms = config_.runtime.startup_timeout_ms;

    // Startup timeout is enforced across the whole bring-up sequence rather
    // than per subsystem so partially-live runtimes fail fast instead of
    // limping forward after one slow stage.
    auto check_startup_deadline = [&](const char *stage) -> bool {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - startup_begin).count();
        if (elapsed_ms < startup_timeout_ms) {
            return true;
        }

        error = "Runtime startup timeout exceeded after '" + std::string(stage) + "' (" + std::to_string(elapsed_ms) +
                "ms >= " + std::to_string(startup_timeout_ms) + "ms)";
        return false;
    };

    if (!init_core_services(error)) {
        return false;
    }
    if (!check_startup_deadline("init_core_services")) {
        return false;
    }

    if (!init_providers(error)) {
        return false;
    }
    if (!check_startup_deadline("init_providers")) {
        return false;
    }

    if (!state_cache_->initialize()) {
        error = "State cache initialization failed: " + state_cache_->last_error();
        return false;
    }
    if (!check_startup_deadline("state_cache.initialize")) {
        return false;
    }

    // Prime state cache once so initial HTTP calls observe a full snapshot
    state_cache_->poll_once(provider_registry_);
    if (!check_startup_deadline("state_cache.poll_once")) {
        return false;
    }

    if (!init_automation(error)) {
        return false;
    }
    if (!check_startup_deadline("init_automation")) {
        return false;
    }

    // Telemetry is started before HTTP so the sink handle can be injected into the
    // HTTP server's dependencies (the /v0/telemetry/status health surface). The
    // sink only needs the event emitter (from init_core_services); nothing in HTTP
    // init depends on telemetry.
    if (!init_telemetry(error)) {
        return false;
    }
    if (!check_startup_deadline("init_telemetry")) {
        return false;
    }

    // Run registry before HTTP so its handle can be injected into the server deps
    // (the /v0/runs surface + run_id on /v0/automation/status).
    if (!init_runs(error)) {
        return false;
    }
    if (!check_startup_deadline("init_runs")) {
        return false;
    }

    if (!init_http(error)) {
        return false;
    }
    if (!check_startup_deadline("init_http")) {
        return false;
    }

    LOG_INFO("[Runtime] Initialization complete");
    return true;
}

bool Runtime::init_core_services(std::string & /*error*/) {
    // Create registry
    registry_ = std::make_unique<registry::DeviceRegistry>();

    // Create event emitter
    // Subscriber budget:
    //   32  SSE clients   (HttpServer::MAX_SSE_CLIENTS)
    //    1  telemetry sink (InfluxSink, when enabled)
    //   ─────────────────
    //   33  total
    // Using 33 keeps MAX_SSE_CLIENTS accurate: 32 SSE connections are always
    // available regardless of whether telemetry is enabled.
    static constexpr size_t kMaxEventSubscribers = 33;
    event_emitter_ = std::make_shared<events::EventEmitter>(100, kMaxEventSubscribers);
    LOG_INFO("[Runtime] Event emitter created (max " << event_emitter_->max_subscribers() << " subscribers)");

    // Create state cache
    state_cache_ = std::make_unique<state::StateCache>(*registry_, config_.polling.interval_ms);

    // Wire event emitter to state cache
    state_cache_->set_event_emitter(event_emitter_);

    // Create call router
    call_router_ = std::make_unique<control::CallRouter>(*registry_, *state_cache_);

    // Create the e-stop safe-state controller and register its latch with the
    // call router so actuating calls are refused while an e-stop is latched.
    safe_state_controller_ =
        std::make_unique<control::SafeStateController>(*registry_, *call_router_, provider_registry_, config_.safety);
    call_router_->set_actuation_latch(safe_state_controller_.get());

    // Create provider supervisor
    supervisor_ = std::make_unique<provider::ProviderSupervisor>();
    LOG_INFO("[Runtime] Provider supervisor created");

    return true;
}

bool Runtime::init_providers(std::string &error) {
    // Start all providers and discover
    for (const auto &provider_config : config_.providers) {
        LOG_INFO("[Runtime] Starting provider: " << provider_config.id);
        LOG_DEBUG("[Runtime]   Command: " << provider_config.command);

        auto provider = std::make_shared<provider::ProviderHandle>(
            provider_config.id, provider_config.command, provider_config.args, provider_config.timeout_ms,
            provider_config.hello_timeout_ms, provider_config.ready_timeout_ms, config_.runtime.shutdown_timeout_ms);

        if (!provider->start()) {
            error = "Failed to start provider '" + provider_config.id + "': " + provider->last_error();
            return false;
        }

        LOG_INFO("[Runtime] Provider " << provider_config.id << " started");

        // Register provider with supervisor
        supervisor_->register_provider(provider_config.id, provider_config.restart_policy);

        // Discover devices
        if (!registry_->discover_provider(provider_config.id, *provider)) {
            error = "Discovery failed for provider '" + provider_config.id + "': " + registry_->last_error();
            return false;
        }

        provider_registry_.add_provider(provider_config.id, provider);
    }

    std::string ownership_error;
    if (!validate_i2c_ownership_claims(registry_->get_all_devices(), ownership_error)) {
        LOG_ERROR("[Runtime] " << ownership_error);
        error = ownership_error;
        return false;
    }

    LOG_INFO("[Runtime] Ownership validation passed for discovered I2C devices");
    LOG_INFO("[Runtime] All providers started");
    preflight_declared_calls();
    return true;
}

void Runtime::preflight_declared_calls() const {
    // Every call the config declares is dispatched at a moment when nobody is
    // watching a terminal: a mode transition, or an operator pulling the e-stop.
    // A call that cannot resolve or type-check stays invisible until then. This
    // dry-runs each one against the live registry, now that providers have
    // reported their capabilities.
    //
    // `--check-config` deliberately cannot do this: it exits before any provider
    // starts, so no ArgSpec exists to validate against.
    //
    // WHAT THIS DOES NOT CHECK. `validate_call` resolves the device and function
    // and validates argument types and ranges. It performs NO runtime gating --
    // IDLE/AUTO mode gating and the e-stop actuation latch all live in
    // `execute_call`. So a call can pass here and still be refused when it fires:
    // notably an `after_transition` hook into IDLE (blocked by the IDLE gate) or
    // a `*->FAULT` hook during an e-stop (refused by the latch, deliberately --
    // see safe_state.cpp). Passing preflight means "dispatchable", not "will run".
    //
    // Reports, never refuses. A machine whose safe state is partly undispatchable
    // is still far better up than down, and the operator needs the runtime alive
    // to fix it. The loud ERROR is the point.
    if (call_router_ == nullptr) {
        return;
    }

    size_t checked = 0;
    size_t failed = 0;

    // Which rung `POST /v0/estop` would actually run. The ladder runs exactly one
    // (safe_state.cpp), so a broken call in an unselected rung is worth a warning
    // but is not the emergency an ERROR implies.
    const std::string active_rung =
        safe_state_controller_ != nullptr
            ? control::safe_state_kind_to_string(safe_state_controller_->capability().software_safe_state)
            : "none";

    const auto check = [&](const std::string &where, bool active, const ModeTransitionCallConfig &call) {
        control::CallRequest request;
        request.device_handle = call.device_handle;
        request.function_id = call.function_id;
        request.function_name = call.function_name;
        // `validate_call` reads neither flag; left at their defaults rather than
        // set to values that would imply this dry run models real gating.

        for (const auto &[name, value] : call.args) {
            request.args[name] = control::to_provider_value(value);
        }

        ++checked;
        std::string err;
        if (call_router_->validate_call(request, err)) {
            return;
        }

        const std::string target =
            call.function_name.empty() ? ("#" + std::to_string(call.function_id)) : call.function_name;
        if (active) {
            ++failed;
            LOG_ERROR("[Runtime] " << where << " cannot be dispatched: " << call.device_handle << "/" << target << " - "
                                   << err);
        } else {
            LOG_WARN("[Runtime] " << where << " cannot be dispatched: " << call.device_handle << "/" << target << " - "
                                  << err << " (not the active e-stop rung; declared but unused)");
        }
    };

    for (const auto &call : config_.safety.safe_state.hooks) {
        check("safety.safe_state.hooks", active_rung == "hooks", call);
    }
    for (const auto &call : config_.safety.safe_state.setpoints) {
        check("safety.safe_state.setpoints", active_rung == "setpoints", call);
    }

    // Mode-transition hooks only ever fire on an automation machine.
    if (config_.automation.enabled) {
        const auto check_group = [&](const std::vector<ModeTransitionHookConfig> &hooks, const char *group) {
            for (const auto &hook : hooks) {
                const std::string where = std::string("automation.mode_transition_hooks.") + group + " (" +
                                          (hook.from.empty() ? "*" : hook.from) + "->" +
                                          (hook.to.empty() ? "*" : hook.to) + ")";
                for (const auto &call : hook.calls) {
                    check(where, true, call);
                }
            }
        };
        check_group(config_.automation.mode_transition_hooks.before_transition, "before_transition");
        check_group(config_.automation.mode_transition_hooks.after_transition, "after_transition");
    }

    // Stated unconditionally: "no software safe state" and "preflight did not run"
    // are otherwise indistinguishable, and the first is the one worth knowing.
    // The `zero` rung is derived from the live registry rather than declared, so
    // it has no calls to dry-run here.
    LOG_INFO("[Runtime] preflight: e-stop ladder would run rung '" << active_rung << "'");
    if (active_rung == "none") {
        // The sharp case (#251): the machine DOES declare a fault safe-state --
        // it had to, or the refuse-hookless gate would not have permitted AUTO --
        // but declared it in the section the e-stop does not read. Pressing e-stop
        // is then worse than not pressing it: the latch engages before FAULT is
        // entered, so it suppresses the very hooks that reaching FAULT by any
        // other route would have run. Say that, not just "nothing is declared".
        if (config_.automation.enabled && has_fault_safe_state_hook(config_.automation)) {
            LOG_WARN(
                "[Runtime] preflight: this machine declares '-> FAULT' mode-transition hooks but no "
                "safety.safe_state. POST /v0/estop reads only safety.safe_state, so it will drive NOTHING -- and "
                "because it latches actuation before entering FAULT, it also SUPPRESSES those hooks. Reaching FAULT "
                "any other way (a fault condition, POST /v0/mode {\"mode\":\"FAULT\"}) runs them; the e-stop does "
                "not. Declare safety.safe_state to give the e-stop a safe state of its own.");
        } else {
            LOG_WARN(
                "[Runtime] preflight: no software safe-state is declared; POST /v0/estop will latch without driving "
                "outputs");
        }
    }

    if (failed > 0) {
        LOG_ERROR("[Runtime] " << failed << " of " << checked
                               << " declared call(s) could not be dispatched against the device inventory as it "
                                  "stands now. They will fail at the moment they are needed.");
    } else if (checked > 0) {
        // Deliberately a snapshot, not a guarantee: a provider restart can
        // republish a different inventory (see restart_provider) and invalidate
        // every statement made here.
        LOG_INFO("[Runtime] preflight: all " << checked
                                             << " declared call(s) dispatchable against the current inventory");
    }
}

bool Runtime::init_automation(std::string &error) {
#if ANOLIS_ENABLE_AUTOMATION
    auto parameter_value_to_provider_value = [](const automation::ParameterValue &input) {
        return control::to_provider_value(input);
    };

    auto hook_mode_matches = [](const std::string &filter, automation::RuntimeMode actual) {
        if (filter.empty() || filter == "*") {
            return true;
        }
        return filter == automation::mode_to_string(actual);
    };

    // Create ModeManager and wire to CallRouter if automation enabled
    if (config_.automation.enabled) {
        mode_manager_ = std::make_unique<automation::ModeManager>(automation::RuntimeMode::IDLE);

        std::string policy_str =
            (config_.automation.manual_gating_policy == GatingPolicy::BLOCK) ? "BLOCK" : "OVERRIDE";
        call_router_->set_mode_manager(mode_manager_.get(), policy_str);

        // Refuse-hookless (D6): if automation is enabled with actuating outputs
        // but no safe-state mode_transition_hooks, veto any transition INTO AUTO
        // so autonomous actuation cannot start without a declared safe-state
        // path. Enforced at set_mode (the single choke point into AUTO) and
        // re-evaluated against the live registry, so it holds for the process
        // lifetime and across provider restarts. Manual control and /v0/estop
        // stay available; the veto never blocks FAULT (mode_manager honors that).
        const auto startup_gate = evaluate_hookless_auto_gate(*registry_, config_.automation);
        if (startup_gate.refused) {
            LOG_ERROR("[Runtime] " << startup_gate.message);
            LOG_ERROR(
                "[Runtime] AUTO is refused until safe-state hooks are declared; "
                "manual control and /v0/estop remain available.");
        }
        mode_manager_->on_before_mode_change(
            [this](automation::RuntimeMode /*prev*/, automation::RuntimeMode next, std::string &veto_reason) {
                if (next != automation::RuntimeMode::AUTO) {
                    return true;
                }
                const auto gate = evaluate_hookless_auto_gate(*registry_, config_.automation);
                if (gate.refused) {
                    veto_reason = gate.message;
                    return false;
                }
                return true;
            });
    }

    // Create and initialize ParameterManager BEFORE HTTP server
    if (config_.automation.enabled) {
        LOG_INFO("[Runtime] Creating parameter manager");
        parameter_manager_ = std::make_unique<automation::ParameterManager>();

        // Load parameters from config
        for (const auto &param_config : config_.automation.parameters) {
            std::optional<std::vector<std::string>> allowed =
                param_config.allowed_values.empty() ? std::nullopt : std::make_optional(param_config.allowed_values);

            if (!parameter_manager_->define(param_config.name, param_config.type, param_config.default_value,
                                            param_config.min, param_config.max, allowed)) {
                LOG_WARN("[Runtime] Failed to define parameter: " << param_config.name);
            }
        }

        LOG_INFO("[Runtime] Parameter manager initialized with " << parameter_manager_->parameter_count()
                                                                 << " parameters");
    }

    // Register transition hooks when configured.
    if (mode_manager_ != nullptr) {
        const auto before_hooks = config_.automation.mode_transition_hooks.before_transition;
        const auto after_hooks = config_.automation.mode_transition_hooks.after_transition;

        if (!before_hooks.empty()) {
            mode_manager_->on_before_mode_change(
                [this, before_hooks, hook_mode_matches, parameter_value_to_provider_value](
                    automation::RuntimeMode prev, automation::RuntimeMode next, std::string &hook_error) {
                    for (const auto &hook : before_hooks) {
                        if (!hook_mode_matches(hook.from, prev) || !hook_mode_matches(hook.to, next)) {
                            continue;
                        }

                        for (const auto &call : hook.calls) {
                            control::CallRequest request;
                            request.device_handle = call.device_handle;
                            request.function_id = call.function_id;
                            request.function_name = call.function_name;
                            request.is_automated = true;

                            for (const auto &[arg_name, arg_value] : call.args) {
                                request.args[arg_name] = parameter_value_to_provider_value(arg_value);
                            }

                            const auto result = call_router_->execute_call(request, provider_registry_);
                            if (!result.success) {
                                std::stringstream hook_msg;
                                hook_msg << "Before-transition hook failed (" << automation::mode_to_string(prev)
                                         << " -> " << automation::mode_to_string(next) << "): " << call.device_handle
                                         << " "
                                         << (call.function_name.empty() ? std::to_string(call.function_id)
                                                                        : call.function_name)
                                         << " - " << result.error_message;
                                if (hook.fail_on_error) {
                                    hook_error = hook_msg.str();
                                    return false;
                                }
                                LOG_WARN("[Runtime] " << hook_msg.str());
                            } else {
                                // Safe-state hooks are load-bearing during operator sessions:
                                // success must leave evidence, not just failures (#188).
                                LOG_INFO("[Runtime] Before-transition hook ("
                                         << automation::mode_to_string(prev) << " -> "
                                         << automation::mode_to_string(next) << "): " << call.device_handle << " "
                                         << (call.function_name.empty() ? std::to_string(call.function_id)
                                                                        : call.function_name)
                                         << " OK");
                            }
                        }
                    }
                    return true;
                });
        }

        if (!after_hooks.empty()) {
            mode_manager_->on_mode_change([this, after_hooks, hook_mode_matches, parameter_value_to_provider_value](
                                              automation::RuntimeMode prev, automation::RuntimeMode next) {
                for (const auto &hook : after_hooks) {
                    if (!hook_mode_matches(hook.from, prev) || !hook_mode_matches(hook.to, next)) {
                        continue;
                    }

                    for (const auto &call : hook.calls) {
                        control::CallRequest request;
                        request.device_handle = call.device_handle;
                        request.function_id = call.function_id;
                        request.function_name = call.function_name;
                        request.is_automated = true;

                        for (const auto &[arg_name, arg_value] : call.args) {
                            request.args[arg_name] = parameter_value_to_provider_value(arg_value);
                        }

                        const auto result = call_router_->execute_call(request, provider_registry_);
                        if (!result.success) {
                            std::stringstream hook_msg;
                            hook_msg << "After-transition hook failed (" << automation::mode_to_string(prev) << " -> "
                                     << automation::mode_to_string(next) << "): " << call.device_handle << " "
                                     << (call.function_name.empty() ? std::to_string(call.function_id)
                                                                    : call.function_name)
                                     << " - " << result.error_message;
                            if (hook.fail_on_error) {
                                LOG_ERROR("[Runtime] " << hook_msg.str());
                            } else {
                                LOG_WARN("[Runtime] " << hook_msg.str());
                            }
                        } else {
                            LOG_INFO(
                                "[Runtime] After-transition hook ("
                                << automation::mode_to_string(prev) << " -> " << automation::mode_to_string(next)
                                << "): " << call.device_handle << " "
                                << (call.function_name.empty() ? std::to_string(call.function_id) : call.function_name)
                                << " OK");
                        }
                    }
                }
            });
        }
    }

    // Register mode change callback to emit telemetry events (only if automation enabled)
    if (mode_manager_) {
        mode_manager_->on_mode_change([this](automation::RuntimeMode prev, automation::RuntimeMode next) {
            if (event_emitter_) {
                events::ModeChangeEvent event;
                event.event_id = event_emitter_->next_event_id();
                event.previous_mode = automation::mode_to_string(prev);
                event.new_mode = automation::mode_to_string(next);
                event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();

                event_emitter_->emit(event);
                LOG_INFO("[Runtime] Mode change event emitted: " << event.previous_mode << " -> " << event.new_mode);
            }
            // Fold the mode change into the open run's event stream (if any).
            if (run_journal_) {
                if (auto open_id = run_journal_->open_run_id()) {
                    run_journal_->append_event(*open_id, runs::RunEventCategory::ModeChange, "",
                                               nlohmann::json{{"previous_mode", automation::mode_to_string(prev)},
                                                              {"new_mode", automation::mode_to_string(next)}},
                                               0);
                }
            }
        });
    }

    // Register callback to emit parameter change events (if parameter manager exists)
    if (parameter_manager_) {
        parameter_manager_->on_parameter_change([this](const std::string &name,
                                                       const automation::ParameterValue &old_value,
                                                       const automation::ParameterValue &new_value) {
            if (event_emitter_) {
                events::ParameterChangeEvent event;
                event.event_id = event_emitter_->next_event_id();
                event.parameter_name = name;
                event.old_value_str = automation::parameter_value_to_string(old_value);
                event.new_value_str = automation::parameter_value_to_string(new_value);
                event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();

                event_emitter_->emit(event);
                LOG_INFO("[Runtime] Parameter '" << name << "' changed: " << event.old_value_str << " -> "
                                                 << event.new_value_str);
            }
            // Fold the parameter change into the open run's event stream (if any).
            if (run_journal_) {
                if (auto open_id = run_journal_->open_run_id()) {
                    run_journal_->append_event(
                        *open_id, runs::RunEventCategory::ParameterChange, name,
                        nlohmann::json{{"parameter_name", name},
                                       {"old_value", automation::parameter_value_to_string(old_value)},
                                       {"new_value", automation::parameter_value_to_string(new_value)}},
                        0);
                }
            }
        });
    }

    // Create and initialize BTRuntime if enabled
    if (config_.automation.enabled) {
        LOG_INFO("[Runtime] Creating BT runtime");
        bt_runtime_ =
            std::make_unique<automation::BTRuntime>(*state_cache_, *call_router_, provider_registry_, *mode_manager_,
                                                    parameter_manager_.get()  // Pass parameter manager
            );

        // Set event emitter for error notifications
        if (event_emitter_) {
            bt_runtime_->set_event_emitter(event_emitter_);
        }

        if (!bt_runtime_->load_tree(config_.automation.behavior_tree)) {
            error = "Failed to load behavior tree: " + config_.automation.behavior_tree;
            return false;
        }

        LOG_INFO("[Runtime] Behavior tree loaded: " << config_.automation.behavior_tree);
    } else {
        LOG_INFO("[Runtime] Automation disabled in config");
    }

    return true;
#else
    (void)error;
    if (config_.automation.enabled) {
        LOG_WARN("[Runtime] Automation configured but disabled at build time (ANOLIS_ENABLE_AUTOMATION=OFF)");
    } else {
        LOG_INFO("[Runtime] Automation disabled in config");
    }
    return true;
#endif
}

bool Runtime::init_http(std::string &error) {
    // Create and start HTTP server if enabled
    if (config_.http.enabled) {
        LOG_INFO("[Runtime] Creating HTTP server");
        http::HttpServerDependencies dependencies(*registry_, *state_cache_, *call_router_, provider_registry_);
        dependencies.supervisor = supervisor_.get();          // optional: supervision snapshots
        dependencies.event_emitter = event_emitter_;          // optional: SSE emitter
        dependencies.telemetry_sink = telemetry_sink_.get();  // optional: telemetry health surface
        dependencies.run_journal = run_journal_.get();        // optional: run registry
#if ANOLIS_ENABLE_AUTOMATION
        dependencies.mode_manager = mode_manager_.get();            // optional: automation mode
        dependencies.parameter_manager = parameter_manager_.get();  // optional: runtime parameters
        dependencies.bt_runtime = bt_runtime_.get();                // optional: automation runtime
        // The e-stop controller drives FAULT on automation machines. Without
        // automation there is no mode manager, so it stays latch + ladder only.
        safe_state_controller_->set_mode_manager(mode_manager_.get());
#endif
        // Expose the e-stop controller to the HTTP surface (POST /v0/estop and
        // the status snapshot) regardless of automation.
        dependencies.safe_state = safe_state_controller_.get();
        // Device-liveness staleness thresholds (#220): derived from the poll
        // cadence + device count unless explicitly overridden.
        dependencies.staleness_policy = {config_.polling.interval_ms, config_.health.staleness.warn_after_ms,
                                         config_.health.staleness.stale_after_ms};
        http_server_ =
            std::make_unique<http::HttpServer>(config_.http, config_.polling.interval_ms, std::move(dependencies));

        std::string http_error;
        if (!http_server_->start(http_error)) {
            error = "HTTP server failed to start: " + http_error;
            return false;
        }
        LOG_INFO("[Runtime] HTTP server started on " << config_.http.bind << ":" << config_.http.port);
    } else {
        LOG_INFO("[Runtime] HTTP server disabled in config");
    }
    return true;
}

bool Runtime::init_telemetry(std::string & /*error*/) {
    // Start telemetry sink if enabled
    if (config_.telemetry.enabled) {
        LOG_INFO("[Runtime] Creating telemetry sink");

        telemetry::InfluxConfig influx_config;
        influx_config.enabled = true;
        influx_config.url = config_.telemetry.influx_url;
        influx_config.org = config_.telemetry.influx_org;
        influx_config.bucket = config_.telemetry.influx_bucket;
        influx_config.token = config_.telemetry.influx_token;
        influx_config.runtime_name = config_.runtime.name.empty() ? "default" : config_.runtime.name;
        influx_config.batch_size = config_.telemetry.batch_size;
        influx_config.flush_interval_ms = config_.telemetry.flush_interval_ms;
        influx_config.queue_size = config_.telemetry.queue_size;
        influx_config.max_retry_buffer_size = config_.telemetry.max_retry_buffer_size;

        telemetry_sink_ = std::make_unique<telemetry::InfluxSink>(influx_config);

        if (!telemetry_sink_->start(event_emitter_)) {
            LOG_WARN("[Runtime] Telemetry sink failed to start");
            // Don't fail runtime initialization - telemetry is optional
        } else {
            LOG_INFO("[Runtime] Telemetry sink started");

            // Health snapshot ingestion (#203) rides the running sink.
            if (config_.telemetry.health_interval_ms > 0) {
                health_snapshot_task_ = std::make_unique<telemetry::HealthSnapshotTask>(
                    config_.telemetry.health_interval_ms, influx_config.runtime_name, provider_registry_, *registry_,
                    *state_cache_, supervisor_.get(), *telemetry_sink_,
                    health::StalenessPolicy{config_.polling.interval_ms, config_.health.staleness.warn_after_ms,
                                            config_.health.staleness.stale_after_ms});
                health_snapshot_task_->start();
            }
        }
    } else {
        LOG_INFO("[Runtime] Telemetry disabled in config");
    }
    return true;
}

bool Runtime::init_runs(std::string & /*error*/) {
    const std::string data_dir = config_.runtime.data_dir.empty() ? "anolis-data" : config_.runtime.data_dir;
    const std::string runtime_name = config_.runtime.name.empty() ? "default" : config_.runtime.name;

    run_journal_ = std::make_unique<runs::RunJournal>(data_dir, runtime_name, config_.polling.interval_ms);

    std::string runs_error;
    if (!run_journal_->initialize(runs_error)) {
        // Non-fatal: the runtime stays up without the run registry; /v0/runs then
        // reports UNAVAILABLE rather than blocking startup on a data-dir problem.
        LOG_WARN("[Runtime] Run registry unavailable: " << runs_error);
        run_journal_.reset();
    } else {
        LOG_INFO("[Runtime] Run registry ready (data_dir=" << data_dir << ")");
#if ANOLIS_ENABLE_AUTOMATION
        // Wire engine faults into the journal here (init_runs runs after
        // init_automation, so the journal now exists). enqueue_fault is
        // non-blocking, so the tick thread never fsyncs.
        if (bt_runtime_) {
            runs::RunJournal *journal = run_journal_.get();
            bt_runtime_->set_fault_sink(
                [journal](const std::string &locus, const std::string &message, int64_t occurred_at_epoch_ms) {
                    journal->enqueue_fault(locus, message, static_cast<uint64_t>(occurred_at_epoch_ms));
                });
        }
#endif
    }
    return true;
}

void Runtime::run() {
    LOG_INFO("[Runtime] Starting main loop");
    running_ = true;

    // start_polling() requires initialize() to have run first.
    // initialize() already primes one synchronous poll_once() snapshot.
    state_cache_->start_polling(provider_registry_);

    LOG_INFO("[Runtime] State cache polling active");

    // Start BT tick loop if automation enabled
#if ANOLIS_ENABLE_AUTOMATION
    if (bt_runtime_) {
        if (!bt_runtime_->start(config_.automation.tick_rate_hz)) {
            LOG_WARN("[Runtime] BT runtime failed to start");
        } else {
            LOG_INFO("[Runtime] BT runtime started (tick rate: " << config_.automation.tick_rate_hz << " Hz)");
        }
    }
#endif

    LOG_INFO("[Runtime] Press Ctrl+C to exit");

    // Main loop: polling, provider health monitoring, crash recovery
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check for shutdown signal
        if (anolis::runtime::SignalHandler::is_shutdown_requested()) {
            LOG_INFO("[Runtime] Signal received, stopping...");
            running_ = false;
            break;
        }

        // Check provider health and handle restarts
        for (const auto &provider_config : config_.providers) {
            const std::string &id = provider_config.id;
            auto provider = provider_registry_.get_provider(id);

            if (!provider) {
                continue;  // Provider not initialized (shouldn't happen)
            }

            // Check if provider is available
            if (!provider->is_available()) {
                // Provider crashed or unavailable

                // Check if circuit breaker is open
                if (supervisor_->is_circuit_open(id)) {
                    continue;  // Circuit open, no more attempts
                }

                // The main loop runs every 100ms, so outage accounting is split
                // into "new crash observed" and "restart window open". That
                // prevents attempt_count from increasing on every loop while a
                // provider remains down.
                // Mark that we've detected this crash (only records once per crash)
                if (supervisor_->mark_crash_detected(id)) {
                    // This is a new crash - record it and schedule restart
                    if (!supervisor_->record_crash(id)) {
                        // Max attempts exceeded - circuit breaker opened
                        continue;
                    }
                }

                // Check if we should attempt restart (backoff period elapsed)
                if (supervisor_->should_restart(id)) {
                    // Clear crash detected flag before restart attempt
                    // (so next crash will be properly detected)
                    supervisor_->clear_crash_detected(id);

                    // Attempt restart
                    LOG_INFO("[Runtime] Attempting to restart provider: " << id);
                    if (restart_provider(id, provider_config)) {
                        LOG_INFO("[Runtime] Provider restarted successfully: " << id);
                    } else {
                        LOG_ERROR("[Runtime] Failed to restart provider: " << id);
                        // Don't mark crash detected - will retry on next iteration
                    }
                }
            } else {
                // Provider is healthy: record heartbeat for timing/observability.
                supervisor_->record_heartbeat(id);
                // Reset attempts only after the provider has remained healthy for the
                // configured stability window (avoids clearing crash streak too early).
                if (supervisor_->should_mark_recovered(id)) {
                    supervisor_->record_success(id);
                }
            }
        }
    }

    LOG_INFO("[Runtime] Shutting down");
    state_cache_->stop_polling();
}

void Runtime::shutdown() {
    // Stop BT runtime first
#if ANOLIS_ENABLE_AUTOMATION
    if (bt_runtime_) {
        LOG_INFO("[Runtime] Stopping BT runtime");
        bt_runtime_->stop();
    }
#endif

    // Stop HTTP server
    if (http_server_) {
        LOG_INFO("[Runtime] Stopping HTTP server");
        http_server_->stop();
    }

    // Stop health ingestion before its sink
    if (health_snapshot_task_) {
        LOG_INFO("[Runtime] Stopping health snapshot task");
        health_snapshot_task_->stop();
    }

    // Stop telemetry sink
    if (telemetry_sink_) {
        LOG_INFO("[Runtime] Stopping telemetry sink");
        telemetry_sink_->stop();
    }

    if (state_cache_) {
        state_cache_->stop_polling();
    }

    // Provider cleanup handled by ProviderRegistry destructor
    provider_registry_.clear();
}

bool Runtime::restart_provider(const std::string &provider_id, const provider::ProviderConfig &provider_config) {
    // Start timeout clock (timeout starts after backoff completes, when restart attempt begins)
    auto restart_start_time = std::chrono::steady_clock::now();
    int timeout_ms = provider_config.restart_policy.timeout_ms;

    LOG_INFO("[Runtime] Restarting provider: " << provider_id);
    LOG_DEBUG("[Runtime]   Command: " << provider_config.command);

    // Replacement is a two-phase operation: bring up and inspect the new
    // provider off to the side first, then publish the new device inventory and
    // provider handle only after restart timeout checks and ownership
    // validation pass. Failures deliberately leave the current unavailable entry
    // in place so supervision can continue retrying against the same provider ID.
    auto provider = std::make_shared<provider::ProviderHandle>(
        provider_id, provider_config.command, provider_config.args, provider_config.timeout_ms,
        provider_config.hello_timeout_ms, provider_config.ready_timeout_ms, config_.runtime.shutdown_timeout_ms);

    if (!provider->start()) {
        LOG_ERROR("[Runtime] Failed to start provider '" << provider_id << "': " << provider->last_error());
        return false;
    }

    // Check timeout after start()
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restart_start_time)
            .count();
    if (elapsed_ms >= timeout_ms) {
        LOG_ERROR("[Runtime] Restart timeout exceeded for provider '" << provider_id << "' after start() ("
                                                                      << elapsed_ms << "ms >= " << timeout_ms << "ms)");
        return false;
    }

    LOG_INFO("[Runtime] Provider " << provider_id << " process started");

    std::vector<registry::RegisteredDevice> replacement_devices;
    if (!registry_->inspect_provider_devices(provider_id, *provider, replacement_devices)) {
        LOG_ERROR("[Runtime] Discovery failed for provider '" << provider_id << "': " << registry_->last_error());
        return false;
    }

    // Check timeout after device inspection.
    elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restart_start_time)
            .count();
    if (elapsed_ms >= timeout_ms) {
        LOG_ERROR("[Runtime] Restart timeout exceeded for provider '" << provider_id << "' after discovery ("
                                                                      << elapsed_ms << "ms >= " << timeout_ms << "ms)");
        return false;
    }

    std::string ownership_error;
    if (!validate_i2c_ownership_claims_after_provider_replacement(registry_->get_all_devices(), provider_id,
                                                                  replacement_devices, ownership_error)) {
        LOG_ERROR("[Runtime] " << ownership_error);
        return false;
    }

    registry_->commit_provider_devices(provider_id, std::move(replacement_devices), true);

    // Rebuild the cache poll plan before swapping the live provider handle so
    // subsequent reads and post-call refreshes observe the replacement device
    // inventory immediately after publication.
    state_cache_->rebuild_poll_configs(provider_id);

    // Swap registry entry only after replacement provider startup + discovery succeeds.
    provider_registry_.add_provider(provider_id, provider);

    LOG_INFO("[Runtime] Provider " << provider_id << " restarted and devices rediscovered");

    // A restarted provider can republish a different inventory — renamed devices,
    // renamed functions, changed argument types — any of which silently
    // invalidates the boot-time preflight. Re-run it against what is actually
    // registered now, for the same reason the hookless gate is re-checked below.
    preflight_declared_calls();

#if ANOLIS_ENABLE_AUTOMATION
    // #233: a restarted provider can publish an inventory with new actuating
    // functions that have no AUTO->FAULT safe-state hook, bypassing the
    // entry-time gate. Re-check while in AUTO; a refusal forces FAULT (which
    // cannot be vetoed). Restart still succeeded, so this returns true regardless
    // (returning false would make the supervisor retry-restart a healthy
    // provider). The AUTO-check/set_mode race is benign (FAULT is legal from any
    // mode). A narrow residual TOCTOU remains — a commit landing during an
    // in-flight MANUAL->AUTO transition's callbacks can be missed by both this
    // re-check (still MANUAL) and the entry gate (evaluated the pre-commit
    // registry); tracked in #239. This still closes the permanent in-AUTO
    // fail-open.
    if (mode_manager_ && config_.automation.enabled) {
        enforce_hookless_gate_in_auto(*registry_, config_.automation, *mode_manager_,
                                      "provider '" + provider_id + "' restart");
    }
#endif

    return true;
}

}  // namespace runtime
}  // namespace anolis
