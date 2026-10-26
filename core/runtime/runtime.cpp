/**
 * @file runtime.cpp
 * @brief Implementation of staged runtime bring-up, supervision, and provider restart flows.
 *
 * Runtime startup is intentionally staged and fail-fast: providers are started,
 * devices are discovered, ownership claims are validated, and the state cache
 * is primed before the main loop begins accepting normal traffic.
 */

#include "runtime.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
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
#include "poll_hint_check.hpp"
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

    // The stop status after discovery and the first poll (#483). That poll is
    // synchronous and bounded by provider timeouts, so this is the bounded
    // wait: a status not readable by now reads unknown, and holds.
    warn_stop_chain_config();
    warn_unjudgeable_expectations();
    update_stop_chain();

    apply_startup_outputs();
    if (!check_startup_deadline("safety.on_startup")) {
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
    // The latch survives a restart: one file per machine under data_dir, keyed
    // by the config file's directory (#482). Restored here, before providers
    // start and long before HTTP opens.
    safe_state_controller_->attach_latch_store(std::make_unique<control::LatchStore>(
        config_.runtime.data_dir.empty() ? "anolis-data" : config_.runtime.data_dir, config_.config_dir));

    // Per-device loss latch (#285). Blocks the behaviour tree from re-commanding
    // a device that went unreachable, until an operator deliberately re-enters
    // AUTO. Owned here rather than by StateCache because `anolis_control` links
    // `anolis_state` and not the reverse; StateCache reaches it through the
    // reachability sink wired below.
    device_loss_latch_ = std::make_unique<control::DeviceLossLatch>();
    call_router_->set_device_loss_latch(device_loss_latch_.get());
    wire_device_loss_latch();
    wire_stop_chain();

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

    const auto all_devices = registry_->get_all_devices();
    std::string ownership_error;
    if (!validate_ownership_claims(all_devices, ownership_error)) {
        LOG_ERROR("[Runtime] " << ownership_error);
        error = ownership_error;
        return false;
    }
    for (const auto &legacy_provider : providers_with_unchecked_legacy_ownership(all_devices)) {
        LOG_WARN("[Runtime] Provider '" << legacy_provider
                                        << "' publishes the old hw.* ownership tags and no anolis.claim; its "
                                           "devices' ownership is not checked. Upgrade the provider.");
    }

    LOG_INFO("[Runtime] Ownership validation passed");

    for (const auto &line : health::unmatched_excluded_signals(config_.health.excluded_signals, *registry_)) {
        LOG_WARN("[Runtime] " << line);
    }

    // Once, at startup: a restarted provider republishing its hints is not news.
    for (const auto &line : poll_hint_mismatches(all_devices, config_.polling.interval_ms)) {
        LOG_WARN("[Runtime] " << line);
    }

    LOG_INFO("[Runtime] All providers started");
    preflight_declared_calls();

    // The firmware command watchdog is what stops a device when the runtime or
    // its provider is lost (#297). Keyed on the SDK-reserved watchdog_armed
    // metric; a device that does not report it makes no claim and stays quiet.
    for (const auto &handle : health::actuating_devices_with_watchdog_disarmed(provider_registry_, *registry_)) {
        LOG_WARN("[Runtime] preflight: " << handle
                                         << " has ACTUATE functions and reports watchdog_armed=false; its firmware "
                                            "watchdog will not stop it if the runtime or its provider is lost");
    }
    return true;
}

control::AbsentDevices Runtime::devices_reported_missing() const {
    control::AbsentDevices absent;
    for (const auto &[provider_id, provider] : provider_registry_.get_all_providers()) {
        anolis::deviceprovider::v1::GetHealthResponse health;
        if (!provider->is_available() || !provider->get_health(health)) {
            continue;
        }
        for (const auto &device : health.devices()) {
            if (device.state() != anolis::deviceprovider::v1::DeviceHealth::STATE_UNREACHABLE ||
                registry_->get_device_copy(provider_id, device.device_id()).has_value()) {
                continue;
            }
            absent[provider_id + "/" + device.device_id()] =
                device.message().empty() ? "reported unreachable by its provider" : device.message();
        }
    }
    return absent;
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
    // a `*->FAULT` hook during an e-stop whose ladder plans a rung (refused by
    // the latch, deliberately -- see safe_state.cpp). Passing preflight means
    // "dispatchable", not "will run".
    //
    // Reports, never refuses. A machine whose safe state is partly undispatchable
    // is still far better up than down, and the operator needs the runtime alive
    // to fix it. The loud ERROR is the point.
    if (call_router_ == nullptr) {
        return;
    }

    size_t checked = 0;
    size_t failed = 0;
    size_t absent_calls = 0;
    // A call to a device its provider reports missing is expected not to
    // dispatch until the device comes up: a warning, not an error (#461).
    const control::AbsentDevices absent = devices_reported_missing();

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
        if (const auto it = absent.find(call.device_handle); it != absent.end()) {
            LOG_WARN("[Runtime] " << where << " " << call.device_handle << "/" << target
                                  << " absent at startup: " << it->second);
            ++absent_calls;
            return;
        }
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

        // The refuse-hookless gate admits AUTO on a hook's from/to filters, so
        // one AUTO->FAULT hook satisfies it for every output, driven or not
        // (#246). Name the ones it leaves alone. A warning, not a refusal: an
        // output can be left alone on purpose.
        for (const auto &function : fault_hook_uncovered_functions(*registry_, *call_router_, config_.automation)) {
            LOG_WARN("[Runtime] preflight: no dispatchable AUTO->FAULT hook call drives "
                     << function << "; a fault in AUTO leaves it as it was");
        }
    }

    // Stated unconditionally: "no software safe state" and "preflight did not run"
    // are otherwise indistinguishable, and the first is the one worth knowing.
    // The `zero` rung is derived from the live registry rather than declared, so
    // it has no calls to dry-run here.
    LOG_INFO("[Runtime] preflight: e-stop ladder would run rung '" << active_rung << "'");
    if (active_rung == "none") {
        // The sharp case (#251): the machine DOES declare a fault safe-state --
        // it had to, or the refuse-hookless gate would not have permitted AUTO --
        // but declared it in the section the e-stop does not read. With no rung
        // the e-stop lets its FAULT entry run those hooks, but the runtime cannot
        // know they make this machine safe, so it reports
        // safe_state_achieved=false. Say that, not just "nothing is declared".
        if (config_.automation.enabled && has_fault_safe_state_hook(config_.automation)) {
            LOG_WARN(
                "[Runtime] preflight: this machine declares '-> FAULT' mode-transition hooks but no "
                "safety.safe_state. POST /v0/estop reads only safety.safe_state, so its ladder will drive NOTHING; "
                "it runs only the '-> FAULT' hooks that match the mode it is pressed in (in IDLE, only "
                "after_transition hooks: the IDLE gate refuses before_transition calls), and reports "
                "safe_state_achieved=false. Declare safety.safe_state to give the e-stop a safe state of its own.");
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
    } else if (absent_calls > 0) {
        LOG_WARN("[Runtime] preflight: " << absent_calls << " of " << checked
                                         << " declared call(s) target devices absent at startup; the rest are "
                                            "dispatchable against the current inventory");
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
        const auto startup_gate = evaluate_hookless_auto_gate(*registry_, config_.automation, config_.safety);
        if (startup_gate.refused) {
            LOG_ERROR("[Runtime] " << startup_gate.message);
            LOG_ERROR(
                "[Runtime] AUTO is refused until safe-state hooks are declared; "
                "manual control and /v0/estop remain available.");
        }
        // Checked before the transition's hooks run, and again under the mode
        // lock as AUTO is written, so a provider restart that commits a hookless
        // inventory meanwhile cannot slip in between (#239).
        install_hookless_auto_gate(*mode_manager_, *registry_, config_.automation, config_.safety);
        // A software stop's latch holds until a deliberate clear, so AUTO waits
        // for it too (#481).
        install_estop_auto_gate(*mode_manager_, *safe_state_controller_);
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
            } else {
                // Operator changes are not persisted (#273), so a value set before
                // a restart is back at its default here. Say so where it shows.
                LOG_INFO("[Runtime] Parameter '" << param_config.name << "' at config default "
                                                 << automation::parameter_value_to_string(param_config.default_value));
            }
        }

        LOG_INFO("[Runtime] Parameter manager initialized with " << parameter_manager_->parameter_count()
                                                                 << " parameters");
    }

    // Re-arm device-loss latches on the deliberate MANUAL -> AUTO transition
    // (#285). Registered as an AFTER-change callback, not a before-change one: a
    // vetoed transition must not clear the latch. Registered BEFORE the
    // after-transition hooks below, which do provider I/O: ModeManager commits
    // the mode before running these callbacks and the tree ticks on the
    // committed mode, so every callback ahead of this one widens the window in
    // which the tree is refused for a device the operator just re-armed.
    if (mode_manager_ != nullptr) {
        mode_manager_->on_mode_change([this](automation::RuntimeMode prev, automation::RuntimeMode next) {
            if (prev != automation::RuntimeMode::MANUAL || next != automation::RuntimeMode::AUTO) {
                return;
            }
            const auto released = device_loss_latch_->release_all();
            for (const auto &handle : released) {
                LOG_WARN("[Runtime] re-armed " << handle << " for automation (MANUAL -> AUTO)");
            }
            if (!released.empty()) {
                LOG_INFO("[Runtime] " << released.size()
                                      << " device loss latch(es) released by operator re-entry into AUTO");
            }
        });
    }

    // A FAULT-entry hook the e-stop let through under rung none (#251) is the
    // only thing driving its actuator, so a failure is reported in the same
    // shape as a ladder action the e-stop did not drive.
    auto warn_estop_hook_not_driven = [this](const control::CallRequest &request, const ModeTransitionCallConfig &call,
                                             const control::CallResult &result) {
        if (request.fault_entry_hook && safe_state_controller_ != nullptr &&
            safe_state_controller_->admits_fault_entry_hooks()) {
            LOG_WARN("[SafeState] E-stop did not drive "
                     << call.device_handle << "/"
                     << (call.function_name.empty() ? std::to_string(call.function_id) : call.function_name) << ": "
                     << result.error_message);
        }
    };

    // Register transition hooks when configured.
    if (mode_manager_ != nullptr) {
        const auto before_hooks = config_.automation.mode_transition_hooks.before_transition;
        const auto after_hooks = config_.automation.mode_transition_hooks.after_transition;

        if (!before_hooks.empty()) {
            mode_manager_->on_before_mode_change([this, before_hooks, hook_mode_matches, warn_estop_hook_not_driven](
                                                     automation::RuntimeMode prev, automation::RuntimeMode next,
                                                     std::string &hook_error) {
                for (const auto &hook : before_hooks) {
                    if (!hook_mode_matches(hook.from, prev) || !hook_mode_matches(hook.to, next)) {
                        continue;
                    }

                    for (const auto &call : hook.calls) {
                        const auto request = mode_hook_request(call, next);
                        const auto result = call_router_->execute_call(request, provider_registry_);
                        if (!result.success) {
                            warn_estop_hook_not_driven(request, call, result);
                            std::stringstream hook_msg;
                            hook_msg << "Before-transition hook failed (" << automation::mode_to_string(prev) << " -> "
                                     << automation::mode_to_string(next) << "): " << call.device_handle << " "
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
                            LOG_INFO(
                                "[Runtime] Before-transition hook ("
                                << automation::mode_to_string(prev) << " -> " << automation::mode_to_string(next)
                                << "): " << call.device_handle << " "
                                << (call.function_name.empty() ? std::to_string(call.function_id) : call.function_name)
                                << " OK");
                        }
                    }
                }
                return true;
            });
        }

        if (!after_hooks.empty()) {
            mode_manager_->on_mode_change([this, after_hooks, hook_mode_matches, warn_estop_hook_not_driven](
                                              automation::RuntimeMode prev, automation::RuntimeMode next) {
                for (const auto &hook : after_hooks) {
                    if (!hook_mode_matches(hook.from, prev) || !hook_mode_matches(hook.to, next)) {
                        continue;
                    }

                    for (const auto &call : hook.calls) {
                        const auto request = mode_hook_request(call, next);
                        const auto result = call_router_->execute_call(request, provider_registry_);
                        if (!result.success) {
                            warn_estop_hook_not_driven(request, call, result);
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
            // Fold the mode change into the open run's event stream (if any). A
            // tick exception's FAULT transition runs on the tick thread, so it goes
            // through the async queue behind the fault that caused it (#282).
            if (run_journal_) {
                run_journal_->record_mode_change(automation::mode_to_string(prev), automation::mode_to_string(next),
                                                 bt_runtime_ != nullptr && bt_runtime_->on_tick_thread());
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
        // Latched devices are reported on GET /v0/runtime/status (#285).
        dependencies.device_loss_latch = device_loss_latch_.get();
        dependencies.stop_chain = stop_chain_.get();
        // Device-liveness staleness thresholds (#220): derived from the poll
        // cadence + device count unless explicitly overridden.
        dependencies.staleness_policy = {config_.polling.interval_ms, config_.health.staleness.warn_after_ms,
                                         config_.health.staleness.stale_after_ms};
        dependencies.staleness_policy.excluded_signals = config_.health.excluded_signals;
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
                health::StalenessPolicy health_policy{config_.polling.interval_ms,
                                                      config_.health.staleness.warn_after_ms,
                                                      config_.health.staleness.stale_after_ms};
                health_policy.excluded_signals = config_.health.excluded_signals;
                health_snapshot_task_ = std::make_unique<telemetry::HealthSnapshotTask>(
                    config_.telemetry.health_interval_ms, influx_config.runtime_name, provider_registry_, *registry_,
                    *state_cache_, supervisor_.get(), *telemetry_sink_, std::move(health_policy));
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

        update_stop_chain();

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

                // Circuit open: no restarts from the budget, but one half-open
                // probe per interval (#326), so an outage that outlasts the
                // budget does not keep the provider down until a runtime restart.
                if (supervisor_->is_circuit_open(id)) {
                    if (supervisor_->should_probe(id)) {
                        LOG_INFO("[Runtime] Circuit open; probing provider: " << id);
                        supervisor_->record_probe_result(id, restart_provider(id, provider_config));
                    }
                    continue;
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

    // After everything that delivers reachability edges has stopped, and
    // while the providers are still up: re-asserts still queued run now.
    if (reissue_worker_) {
        reissue_worker_->stop();
    }

    // Provider cleanup handled by ProviderRegistry destructor
    provider_registry_.clear();
}

void Runtime::wire_device_loss_latch() {
    // The latch applies each edge by its number (#403) and asks for a
    // re-issue of the device's declared safe state when a latched device
    // returns; it is re-asserted ONCE, in whatever mode we are in.
    //
    // A device that rebooted is already at its power-on default and
    // these calls are harmless. A device that returned WITHOUT
    // rebooting -- a bus outage shorter than the firmware watchdog --
    // is still running its last command, and the latch now refuses the
    // tree's next command to it, including a stop. Leaving that running
    // would be worse than the behaviour this latch replaces.
    //
    // The re-assert never runs on the thread that delivered the edge (#427).
    // That can be CallRouter's post-call poll, which holds the provider's
    // lock, and the re-assert goes back through CallRouter to the same
    // provider. The worker holds no provider lock when it starts one.
    reissue_worker_ = std::make_unique<control::ReissueWorker>(
        [this](const std::string &device_handle) { reissue_safe_state_for(device_handle); });
    state_cache_->set_device_reachability_sink(control::make_reachability_sink(*device_loss_latch_, *reissue_worker_));
}

void Runtime::wire_stop_chain() {
    // A stale reading is three poll intervals old, and never under a second:
    // a status that has stopped updating must not keep reading clear.
    const auto stale_after = std::chrono::milliseconds(std::max(1000, 3 * config_.polling.interval_ms));
    stop_chain_ = std::make_unique<control::StopChainMonitor>(config_.safety.stop_chain, *state_cache_, stale_after);
    if (!config_.safety.stop_chain.status.has_value()) {
        if (!config_.safety.stop_chain.devices.empty()) {
            LOG_WARN("[Runtime] safety.stop_chain.devices is set without a status signal; no stop is observed");
        }
        return;
    }
    safe_state_controller_->set_clear_guard([this] { return stop_chain_->clear_refusal(); });
    device_loss_latch_->set_expected_loss([this, signal = config_.safety.stop_chain.status->signal](
                                              const std::string &device_handle) -> std::optional<std::string> {
        if (stop_chain_->snapshot().state != control::StopChainState::InForce ||
            !stop_chain_->in_scope(device_handle)) {
            return std::nullopt;
        }
        return "the emergency stop in force (" + signal + ")";
    });
}

void Runtime::warn_stop_chain_config() const {
    if (!stop_chain_ || !config_.safety.stop_chain.status.has_value()) {
        return;
    }
    const auto &signal = config_.safety.stop_chain.status->signal;
    const std::string device_handle = signal.substr(0, signal.rfind('/'));
    if (stop_chain_->in_scope(device_handle)) {
        LOG_WARN("[Runtime] stop_chain: the status source "
                 << signal
                 << " is a device in its own scope; it goes "
                    "silent with the stop, so a stop cannot be told from a power fault");
    }
    const auto device = registry_->get_device_by_handle_copy(device_handle);
    if (!device.has_value()) {
        LOG_WARN("[Runtime] stop_chain: the status device " << device_handle
                                                            << " is not registered; the stop status reads unknown");
        return;
    }
    const std::string signal_id = signal.substr(signal.rfind('/') + 1);
    const auto status_signal = device->capabilities.signals_by_id.find(signal_id);
    if (status_signal == device->capabilities.signals_by_id.end()) {
        LOG_WARN("[Runtime] stop_chain: " << device_handle << " has no signal " << signal_id
                                          << "; the stop status reads unknown");
    } else if (!status_signal->second.is_default) {
        LOG_WARN("[Runtime] stop_chain: " << signal
                                          << " has no poll hint, so it is never polled and the stop "
                                             "status reads unknown; its provider must set poll_hint_hz");
    }
    for (const auto &[name, spec] : device->capabilities.functions_by_id) {
        (void)name;
        if (registry::is_actuating(spec)) {
            LOG_WARN("[Runtime] stop_chain: the status device "
                     << device_handle << " also has actuating functions (" << spec.function_name
                     << "); a status the runtime can drive would read a software action back "
                        "as a stop");
            break;
        }
    }
}

void Runtime::warn_unjudgeable_expectations() const {
    if (!stop_chain_ || !stop_chain_->configured()) {
        return;
    }
    // An expectation the runtime has nothing to judge by never raises (#484).
    // Only signals with a poll hint are polled.
    for (const auto &[handle, expect] : config_.safety.stop_chain.expectations) {
        const auto device = registry_->get_device_by_handle_copy(handle);
        if (!device.has_value()) {
            continue;  // absent at startup: judged if it registers later
        }
        const auto &signals = device->capabilities.signals_by_id;
        if (expect == StopChainExpect::ReportsEstop) {
            const auto estop = signals.find("estop");
            if (estop == signals.end() || !estop->second.is_default ||
                estop->second.type != anolis::deviceprovider::v1::VALUE_TYPE_BOOL) {
                LOG_WARN("[Runtime] stop_chain: " << handle
                                                  << " expects reports_estop but has no polled bool estop signal; "
                                                     "a mismatch is never raised for it");
            }
        } else if (std::none_of(signals.begin(), signals.end(),
                                [](const auto &entry) { return entry.second.is_default; })) {
            LOG_WARN("[Runtime] stop_chain: " << handle
                                              << " expects power_loss but has no polled signal, so whether it "
                                                 "still answers cannot be seen; a mismatch is never raised for it");
        }
    }
}

void Runtime::update_stop_chain() {
    if (!stop_chain_ || !stop_chain_->configured()) {
        return;
    }
    if (const auto change = stop_chain_->update(); change.has_value()) {
        const auto &[before, after] = *change;
        const std::string state = control::stop_chain_state_to_string(after.state);
        if (after.state == control::StopChainState::Clear) {
            LOG_INFO("[Runtime] Emergency stop clear: " << after.reason
                                                        << ". A held latch stays until a deliberate clear.");
        } else {
            LOG_WARN("[Runtime] Emergency stop " << state << ": " << after.reason);
        }
        if (event_emitter_) {
            events::StopChainEvent event;
            event.event_id = event_emitter_->next_event_id();
            event.previous_state = control::stop_chain_state_to_string(before.state);
            event.new_state = state;
            event.reason = after.reason;
            event.timestamp_ms = after.since_epoch_ms.value_or(0);
            event_emitter_->emit(event);
        }
        if (stop_chain_->holds()) {
            safe_state_controller_->hold("emergency stop " + state + ": " + after.reason);
        }
    }

    // A declared expect contradicted while in force (#484). The hold is the
    // same either way; this is the alarm.
    for (const auto &change : stop_chain_->update_mismatches()) {
        const auto &mismatch = change.mismatch;
        const std::string expect = control::stop_chain_expect_to_string(mismatch.expect);
        if (change.raised) {
            LOG_WARN("[Runtime] Stop not reaching " << mismatch.device << " (expect " << expect
                                                    << "): " << mismatch.reason);
        } else {
            LOG_INFO("[Runtime] Stop reaching " << mismatch.device << " again (expect " << expect
                                                << "): " << mismatch.reason);
        }
        if (event_emitter_) {
            events::StopChainMismatchEvent event;
            event.event_id = event_emitter_->next_event_id();
            event.device = mismatch.device;
            event.expect = expect;
            event.raised = change.raised;
            event.reason = mismatch.reason;
            event.timestamp_ms = mismatch.since_epoch_ms;
            event_emitter_->emit(event);
        }
    }
}

void Runtime::reissue_safe_state_for(const std::string &device_handle) {
    // The controller runs whichever rung the machine actually declares --
    // hooks, else setpoints, else zero -- filtered to this device. Walking
    // safety.safe_state.hooks here would silently do nothing on a machine
    // that declares setpoints or relies on zeroing.
    const auto result = safe_state_controller_->reassert_for(device_handle);

    int issued = 0;
    for (const auto &action : result.actions) {
        if (action.success) {
            ++issued;
        } else {
            LOG_WARN("[Runtime] safe-state re-issue failed for " << action.device_handle << "/" << action.function
                                                                 << ": " << action.error);
        }
    }

    if (issued > 0) {
        LOG_WARN("[Runtime] " << device_handle << " returned while latched; re-asserted " << issued << " "
                              << control::safe_state_kind_to_string(result.kind)
                              << " safe-state call(s), safe_state_achieved="
                              << (result.safe_state_achieved ? "true" : "false")
                              << ". It stays latched until MANUAL -> AUTO.");
    } else {
        LOG_WARN("[Runtime] " << device_handle << " returned while latched, but the planned safe state ("
                              << control::safe_state_kind_to_string(result.kind)
                              << ") drives nothing on it, safe_state_achieved="
                              << (result.safe_state_achieved ? "true" : "false")
                              << " -- if it did not reboot it may still be running its last command.");
    }
}

void Runtime::apply_startup_outputs() {
    // IDLE refuses new control calls but does not stop an output that is already
    // on, and a restarted runtime finds the devices as the last one left them.
    if (config_.safety.on_startup == StartupOutputs::Leave) {
        LOG_WARN("[Runtime] safety.on_startup=leave: outputs were left as they were");
        return;
    }

    // A stop chain in force at boot (#483): the devices behind it may be
    // unpowered, so their calls are skipped and reported; devices outside its
    // scope are powered and still driven. An unknown status runs it all: the
    // status source may be down while the devices are powered.
    control::AbsentDevices skip = devices_reported_missing();
    if (stop_chain_ != nullptr && stop_chain_->snapshot().state == control::StopChainState::InForce) {
        for (const auto &handle : config_.safety.stop_chain.devices) {
            skip.emplace(handle, "stop chain in force");
        }
    }
    const auto result = safe_state_controller_->drive_safe_state(skip);
    for (const auto &skipped : result.skipped) {
        LOG_WARN("[Runtime] safety.on_startup=safe_state skipped " << skipped.device_handle << "/" << skipped.function
                                                                   << ": " << skipped.reason);
    }
    if (result.kind == control::SafeStateKind::None) {
        LOG_WARN("[Runtime] safety.on_startup=safe_state: no software safe state is declared, nothing to run");
        return;
    }

    // A rung that ran and failed leaves outputs in an unknown state: ERROR.
    // The runtime still starts; GET /v0/runtime/status reports it (#362).
    for (const auto &action : result.actions) {
        if (!action.success) {
            LOG_ERROR("[Runtime] safety.on_startup=safe_state did not drive "
                      << action.device_handle << "/" << action.function << ": " << action.error);
        }
    }
    if (result.actions.empty() && !result.skipped.empty()) {
        // Nothing failed: every call was skipped above, its device absent or
        // behind a stop chain in force.
        LOG_WARN("[Runtime] safety.on_startup=safe_state: ladder="
                 << control::safe_state_kind_to_string(result.kind) << " drove nothing; all " << result.skipped.size()
                 << " call(s) target devices absent at startup or behind the stop chain, safe_state_achieved=false");
    } else if (result.safe_state_achieved) {
        LOG_INFO("[Runtime] safety.on_startup=safe_state: ladder=" << control::safe_state_kind_to_string(result.kind)
                                                                   << " actions=" << result.actions.size()
                                                                   << " safe_state_achieved=true");
    } else {
        LOG_ERROR("[Runtime] safety.on_startup=safe_state: ladder=" << control::safe_state_kind_to_string(result.kind)
                                                                    << " actions=" << result.actions.size()
                                                                    << " safe_state_achieved=false");
    }
}

bool Runtime::restart_provider(const std::string &provider_id, const provider::ProviderConfig &provider_config) {
    int timeout_ms = provider_config.restart_policy.timeout_ms;

    LOG_INFO("[Runtime] Restarting provider: " << provider_id);
    LOG_DEBUG("[Runtime]   Command: " << provider_config.command);

    // Stop the old process before starting its replacement (#326). A provider
    // can be unavailable and still running -- wedged on a hung bus -- and
    // starting a second one beside it would put two processes on one bus. The
    // stopped handle stays registered, so a failed restart is retried as before.
    if (auto old_provider = provider_registry_.get_provider(provider_id)) {
        old_provider->stop();
    }

    // Start the timeout clock only now: timeout_ms bounds the new process coming
    // up, not the old one going down, which can take shutdown_timeout_ms (#379).
    auto restart_start_time = std::chrono::steady_clock::now();

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
    if (!validate_ownership_claims_after_provider_replacement(registry_->get_all_devices(), provider_id,
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
    // mode). A commit landing during an in-flight MANUAL->AUTO transition is
    // caught by the gate's commit guard, which re-runs under the mode lock as
    // AUTO is written (#239): either it sees this inventory and refuses AUTO,
    // or AUTO was already written and this re-check sees it.
    if (mode_manager_ && config_.automation.enabled) {
        enforce_hookless_gate_in_auto(*registry_, config_.automation, config_.safety, *mode_manager_,
                                      "provider '" + provider_id + "' restart");
    }
#endif

    return true;
}

}  // namespace runtime
}  // namespace anolis
