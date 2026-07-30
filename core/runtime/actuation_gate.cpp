#include "runtime/actuation_gate.hpp"

#include <algorithm>
#include <sstream>

#include "registry/device_registry.hpp"
#include "runtime/config.hpp"

namespace anolis {
namespace runtime {

namespace {

// Autonomous actuation only happens in AUTO, so the fault that must be caught is
// the `AUTO -> FAULT` transition. A hook runs on it iff BOTH of its filters match
// (mirrors runtime.cpp dispatch: hook_mode_matches(from, prev) &&
// hook_mode_matches(to, next), where empty/"*" is a wildcard). The loader
// validates from/to to MANUAL/AUTO/IDLE/FAULT/"*"/empty (config.cpp), so plain
// compares suffice. A hook that covers only, e.g., AUTO->MANUAL or IDLE->FAULT is
// NOT an autonomous FAULT safe-state path.
bool hook_covers_auto_to_fault(const ModeTransitionHookConfig &hook) {
    const bool from_ok = hook.from.empty() || hook.from == "*" || hook.from == "AUTO";
    const bool to_ok = hook.to.empty() || hook.to == "*" || hook.to == "FAULT";
    return from_ok && to_ok;
}

// The gate is satisfied only by a declared hook that fires on `AUTO -> FAULT` and
// drives a safe state there (#232).
bool has_fault_safe_state_hook(const AutomationConfig &automation) {
    const auto &hooks = automation.mode_transition_hooks;
    return std::any_of(hooks.before_transition.begin(), hooks.before_transition.end(), hook_covers_auto_to_fault) ||
           std::any_of(hooks.after_transition.begin(), hooks.after_transition.end(), hook_covers_auto_to_fault);
}

}  // namespace

HooklessAutoGate evaluate_hookless_auto_gate(const registry::DeviceRegistry &registry,
                                             const AutomationConfig &automation) {
    HooklessAutoGate result;

    // Only AUTO-capable configs are gated.
    if (!automation.enabled) {
        return result;
    }

    // A hook that drives actuators to a safe state on ->FAULT is the autonomous
    // safe-state path and satisfies the gate. (safety.safe_state is the manual
    // e-stop path only, and a non-FAULT hook is not a fault-safe path.)
    if (has_fault_safe_state_hook(automation)) {
        return result;
    }

    // Collect actuating outputs, fail-closed: any function not explicitly
    // read-only counts (registry::is_actuating). No actuators -> nothing unsafe
    // can happen in AUTO, so a hookless automation config is allowed.
    for (const auto &device : registry.get_all_devices()) {
        for (const auto &[name, spec] : device.capabilities.functions_by_id) {
            (void)name;
            if (registry::is_actuating(spec)) {
                result.actuating_functions.push_back(device.get_handle() + "/" + spec.function_name);
            }
        }
    }

    if (result.actuating_functions.empty()) {
        return result;
    }

    result.refused = true;
    std::ostringstream msg;
    msg << "refusing AUTO: automation is enabled with actuating outputs but no safe-state hook "
           "covers the AUTO->FAULT transition. Declare an automation.mode_transition_hooks entry "
           "matching AUTO->FAULT (from: AUTO/\"*\"/omitted and to: FAULT/\"*\"/omitted) with calls "
           "that drive actuators to a safe state. Actuating outputs: ";
    for (size_t i = 0; i < result.actuating_functions.size(); ++i) {
        msg << (i != 0 ? ", " : "") << result.actuating_functions[i];
    }
    result.message = msg.str();
    return result;
}

}  // namespace runtime
}  // namespace anolis
