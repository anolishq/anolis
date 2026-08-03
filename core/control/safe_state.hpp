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
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "control/i_actuation_latch.hpp"
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

const char *safe_state_kind_to_string(SafeStateKind kind);

/** @brief Per-call outcome recorded while running the ladder. */
struct CallOutcome {
    std::string device_handle;
    std::string function;
    bool success = false;
    std::string error;
};

/** @brief Result of a POST /v0/estop trigger. */
struct EstopResult {
    SafeStateKind kind = SafeStateKind::None;
    std::vector<CallOutcome> actions;
    bool fault_requested = false;  // automation machine: FAULT drive attempted
    bool fault_ok = false;
};

/** @brief Snapshot for GET /v0/runtime/status. */
struct EstopCapability {
    bool latched = false;
    SafeStateKind software_safe_state = SafeStateKind::None;  // what a trigger WOULD do now
    std::optional<int64_t> latched_at_epoch_ms;
    size_t uncovered_actuating_functions = 0;  // actuating outputs with no declared setpoint
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

    // IActuationLatch
    bool is_engaged() const override;

    /** @brief Engage the latch, run the safe-state ladder, and (if automation) drive FAULT. */
    EstopResult trigger(const std::string &reason);

    /** @brief Release the latch. Does NOT change the runtime mode. */
    void clear();

    /** @brief Current latch + planned-safe-state snapshot for the status surface. */
    EstopCapability capability() const;

    /**
     * @brief True when the ladder would drive nothing — no hooks, no covering
     *        setpoints, no `zero_is_safe`.
     *
     * Exists so a `-> FAULT` mode-transition hook can tell whether the ladder is
     * actually the safe-state authority for this machine. When it is not, those
     * hooks are the only safe-state path there is, and the latch must not refuse
     * them (#251).
     *
     * **Lock-free by construction, and it must stay that way.** `trigger()` holds
     * `mutex_` across the ladder AND the `set_mode(FAULT)` call, so the FAULT
     * hooks execute inside that lock; anything here that took `mutex_` would
     * deadlock. `planned_kind()` reads only the immutable config and the device
     * registry, which is why `capability()` calls it before locking.
     */
    bool ladder_drives_nothing() const;

private:
    // Actuating (handle, spec) pairs across all discovered devices, fail-closed.
    std::vector<std::pair<std::string, registry::FunctionSpec>> actuating_functions() const;
    // Which rung would run now; if uncovered_out is set, reports how many
    // actuating outputs lack a declared setpoint.
    SafeStateKind planned_kind(size_t *uncovered_out) const;
    EstopResult run_ladder();
    CallOutcome run_call(const runtime::ModeTransitionCallConfig &call);
    CallOutcome run_zero_call(const std::string &device_handle, const registry::FunctionSpec &spec);

    const registry::DeviceRegistry &registry_;
    CallRouter &call_router_;
    provider::ProviderRegistry &provider_registry_;
    const runtime::SafetyConfig &safety_;
    automation::ModeManager *mode_manager_ = nullptr;  // optional

    std::atomic<bool> latched_{false};
    mutable std::mutex mutex_;              // serialize trigger()/clear(); guard metadata
    std::optional<int64_t> latched_at_ms_;  // guarded by mutex_
};

}  // namespace control
}  // namespace anolis
