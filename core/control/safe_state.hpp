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
 * @param uncovered_out optional count of actuating outputs no setpoint covers.
 */
SafeStateKind planned_safe_state_kind(const registry::DeviceRegistry &registry, const runtime::SafetyConfig &safety,
                                      size_t *uncovered_out = nullptr);

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
