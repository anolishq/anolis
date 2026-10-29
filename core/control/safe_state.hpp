#pragma once

/**
 * @file safe_state.hpp
 * @brief Runtime software safe-state controller for POST /v0/estop.
 *
 * Owns the actuation latch and the declared safe-state ladder. Works whether or
 * not the automation layer is enabled: it reads the profile's `safety` config
 * directly and drives actuators through CallRouter, independent of the
 * automation-gated mode-transition-hook path.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "control/i_actuation_latch.hpp"
#include "control/latch_store.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"

namespace anolis {

namespace automation {
class ModeManager;
}
namespace provider {
class ProviderRegistry;
}

namespace control {

class CallRouter;

/** @brief Which rung of the safe-state ladder applies (or None). */
enum class SafeStateKind { None, Hooks, Setpoints, Zero };

/**
 * @brief What the e-stop ladder would drive for this config against this
 *        inventory, without needing a controller.
 *
 * The single answer to "does this machine have a working software safe state".
 * Callers outside the controller (the refuse-hookless gate, diagnostics) must
 * use this rather than inspecting `safety.safe_state` themselves: the setpoints
 * rung counts only when it covers EVERY actuating output, so a config with
 * setpoints that cover nothing still plans `None` and drives nothing. A second,
 * weaker answer to this question is precisely how a machine gets reported as
 * safe while its e-stop does nothing (#251, #258).
 *
 * How many functions the active rung cannot drive is a separate question,
 * answered by `EstopCapability::uncovered_actuating_functions`.
 */
SafeStateKind planned_safe_state_kind(const registry::DeviceRegistry &registry, const runtime::SafetyConfig &safety);

const char *safe_state_kind_to_string(SafeStateKind kind);

/**
 * @brief ACTUATE functions that no call in `calls` drives with a dispatchable
 *        request, as "<device_handle>/<function_name>".
 *
 * A function is covered when a call targets it (same device; id and name, where
 * given, agree) AND that call passes `CallRouter::validate_call` -- the same
 * dry run the startup preflight uses. Naming a function is not enough: a call
 * that cannot dispatch drives nothing (#253). Covered means driven, not driven
 * to a safe value: a hook that sets a duty to 100 covers that function.
 */
std::vector<std::string> uncovered_actuating_functions(const registry::DeviceRegistry &registry,
                                                       const CallRouter &call_router,
                                                       const std::vector<runtime::ModeTransitionCallConfig> &calls);

/**
 * @brief Why a safe-state call did not succeed (#616).
 *
 * `Config`: the call could not be planned from the config on these devices,
 * and fails the same way on every run: the zero rung could not build its
 * arguments, or the call was refused before it reached the provider because
 * its device or function does not exist or an argument is invalid. `Run`:
 * anything else, such as a provider that failed the call, which a later run
 * may not repeat.
 */
enum class FailureCause {
    Run,
    Config,
};

const char *failure_cause_to_string(FailureCause cause);

/** @brief Per-call outcome recorded while running the ladder. */
struct CallOutcome {
    std::string device_handle;
    std::string function;
    bool success = false;
    std::string error;
    FailureCause cause = FailureCause::Run;  // meaningful only when !success
};

/**
 * @brief A call the startup run left out because its provider reports the
 *        target device missing (anolishq/anolis#461).
 */
struct SkippedAction {
    std::string device_handle;
    std::string function;
    std::string reason;  // the provider's own message for the missing device
};

/**
 * @brief Devices their provider reports missing at startup, by handle, with
 *        the provider's reason. Keys are "<provider_id>/<device_id>".
 */
using AbsentDevices = std::map<std::string, std::string>;

/** @brief Result of a POST /v0/estop trigger. */
struct EstopResult {
    SafeStateKind kind = SafeStateKind::None;
    std::vector<CallOutcome> actions;
    // Calls left out because their device is absent; only drive_safe_state()
    // with an AbsentDevices map fills this. Not in `actions`, so they count
    // for neither side of the verdict.
    std::vector<SkippedAction> skipped;
    // True only when a rung ran, issued at least one call, and every call
    // succeeded. One failed call makes the whole rung not achieved (#256).
    bool safe_state_achieved = false;
    bool fault_requested = false;  // automation machine: FAULT drive attempted
    bool fault_ok = false;
};

/** @brief Snapshot for GET /v0/runtime/status. */
struct EstopCapability {
    bool latched = false;
    SafeStateKind software_safe_state = SafeStateKind::None;  // what a trigger WOULD do now
    std::optional<int64_t> latched_at_epoch_ms;
    size_t uncovered_actuating_functions = 0;  // ACTUATE functions the active rung cannot drive
    // Set while the latch is one this runtime started with, read from disk (#482): why.
    std::optional<std::string> restored_reason;
};

/** @brief What the `safety.on_startup` run did, for GET /v0/runtime/status (#362). */
struct StartupSafeStateReport {
    runtime::StartupOutputs on_startup = runtime::StartupOutputs::SafeState;
    bool ran = false;  // a rung ran; false for `leave` and for no declared safe state
    SafeStateKind kind = SafeStateKind::None;
    bool safe_state_achieved = false;  // same verdict as EstopResult
    std::vector<CallOutcome> failed_actions;
    std::vector<SkippedAction> skipped_actions;
};

/**
 * @brief Owns the e-stop latch and the declared safe-state ladder.
 *
 * Threading: `is_engaged()` is lock-free (atomic). `trigger()` and `clear()`
 * are serialized by an internal mutex that also guards latch metadata.
 */
class SafeStateController : public IActuationLatch {
public:
    SafeStateController(const registry::DeviceRegistry &registry, CallRouter &call_router,
                        provider::ProviderRegistry &provider_registry, const runtime::SafetyConfig &safety);

    /** @brief Attach the mode manager (nullptr when automation is disabled). */
    void set_mode_manager(automation::ModeManager *mode_manager);

    /**
     * @brief Keep the latch on disk across restarts, and restore it now (#482).
     *
     * A file that is present, or present but unreadable, engages the latch
     * before anything can call in; why is logged and kept for `capability()`.
     * From then on `trigger()` writes the file before it returns and `clear()`
     * removes it. Call once, before the HTTP server opens.
     */
    void attach_latch_store(std::unique_ptr<LatchStore> store);

    // IActuationLatch
    bool is_engaged() const override;
    bool admits_fault_entry_hooks() const override;

    /**
     * @brief Engage the latch, run the safe-state ladder, and (if automation) drive FAULT.
     *
     * When the ladder planned no rung, the `-> FAULT` mode-transition hooks
     * fired by that FAULT entry are let through the latch: nothing else drives
     * those actuators (#251). Under any other rung they are refused, as is
     * every hook on any later entry into FAULT.
     */
    EstopResult trigger(const std::string &reason);

    /**
     * @brief Release the latch. Does NOT change the runtime mode.
     *
     * Refused while the clear guard objects, such as while an observed
     * emergency stop is in force or its status unknown (#483). Returns the
     * reason it was refused, or nullopt once the latch is released.
     */
    std::optional<std::string> clear();

    /**
     * @brief Hold the latch for an observed emergency stop (#483).
     *
     * Engages the latch, persists it, and on an automation machine enters
     * FAULT, without running the ladder: the devices behind the stop may be
     * unpowered, and the hardware stop is what makes them safe. The latch
     * refuses the FAULT entry's actuating hook calls. A latch already held is
     * left as it is, its time and file unchanged, and FAULT is still entered
     * (#547).
     */
    void hold(const std::string &reason);

    /** @brief Consulted by clear(): a reason to refuse it, or nullopt. Set once at startup. */
    void set_clear_guard(std::function<std::optional<std::string>()> guard);

    /** @brief Current latch + planned-safe-state snapshot for the status surface. */
    EstopCapability capability() const;

    /**
     * @brief Re-assert the planned safe state for ONE device, in place.
     *
     * Runs the same rung `trigger()` would -- hooks, else setpoints, else zero
     * -- restricted to calls targeting `device_handle`. Does not engage the
     * latch, does not drive FAULT, does not take `mutex_`: this runs on the
     * poll thread when a lost device answers again (#285), and an e-stop
     * arriving over HTTP must never wait behind it. Per-provider serialization
     * in CallRouter is the only ordering the two need.
     *
     * `result.actions` is empty when the planned rung has nothing for this
     * device, which the caller should log: the device may still be running
     * its last command.
     */
    EstopResult reassert_for(const std::string &device_handle);

    /**
     * @brief Run the planned rung once across every device (#362).
     *
     * The same ladder and verdict as `trigger()`, without engaging the latch
     * or driving FAULT. Used by `safety.on_startup: safe_state` before the
     * HTTP server opens, so nothing else is calling in yet. The outcome is
     * kept for `startup_report()`.
     *
     * A hook or setpoint call whose device is in `absent` is not issued: it
     * goes in `skipped` with the provider's reason (anolishq/anolis#461).
     * Skipped calls are not actions, so a run that skips every call reports
     * `safe_state_achieved` false with no failed actions. Only devices a
     * provider reports missing belong in `absent`: any other handle that does
     * not resolve is still issued and fails.
     */
    EstopResult drive_safe_state(const AbsentDevices &absent = {});

    /** @brief The outcome of `drive_safe_state()`; `ran` is false if it never ran a rung. */
    StartupSafeStateReport startup_report() const;

private:
    // Actuating (handle, spec) pairs across all discovered devices, fail-closed.
    std::vector<std::pair<std::string, registry::FunctionSpec>> actuating_functions() const;
    // Which rung would run now.
    SafeStateKind planned_kind() const;
    // ACTUATE functions the rung `kind` cannot drive (all of them for None).
    size_t uncovered_count(SafeStateKind kind) const;
    // Runs the planned rung. With `only_device` set, only calls whose target is
    // that handle are issued. With `absent` set, calls whose target is in it
    // are skipped instead of issued.
    EstopResult run_ladder(const std::string *only_device = nullptr, const AbsentDevices *absent = nullptr);
    CallOutcome run_call(const runtime::ModeTransitionCallConfig &call);
    CallOutcome run_zero_call(const std::string &device_handle, const registry::FunctionSpec &spec);

    const registry::DeviceRegistry &registry_;
    CallRouter &call_router_;
    provider::ProviderRegistry &provider_registry_;
    const runtime::SafetyConfig &safety_;
    automation::ModeManager *mode_manager_ = nullptr;  // optional

    std::atomic<bool> latched_{false};
    // The thread running trigger()'s FAULT entry under rung none; empty otherwise.
    std::atomic<std::thread::id> fault_entry_hook_thread_{};
    mutable std::mutex mutex_;                                 // serialize trigger()/clear(); guard metadata
    std::optional<int64_t> latched_at_ms_;                     // guarded by mutex_
    std::unique_ptr<LatchStore> latch_store_;                  // optional; set once at startup
    std::optional<std::string> restored_reason_;               // guarded by mutex_
    std::function<std::optional<std::string>()> clear_guard_;  // set once at startup
    StartupSafeStateReport startup_report_;                    // guarded by mutex_
};

}  // namespace control
}  // namespace anolis
