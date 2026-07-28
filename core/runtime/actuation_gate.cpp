#include "runtime/actuation_gate.hpp"

#include <sstream>

#include "runtime/config.hpp"
#include "registry/device_registry.hpp"

namespace anolis {
namespace runtime {

HooklessAutoGate evaluate_hookless_auto_gate(const registry::DeviceRegistry &registry,
                                             const AutomationConfig &automation) {
    HooklessAutoGate result;

    // Only AUTO-capable configs are gated.
    if (!automation.enabled) {
        return result;
    }

    // A declared mode-transition hook is the autonomous *->FAULT safe-state path
    // and satisfies the gate. (safety.safe_state is the manual e-stop path only.)
    if (!automation.mode_transition_hooks.before_transition.empty() ||
        !automation.mode_transition_hooks.after_transition.empty()) {
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
    msg << "refusing AUTO: automation is enabled with actuating outputs but no safe-state "
           "mode_transition_hooks are declared. Declare automation.mode_transition_hooks "
           "(e.g. a *->FAULT hook that drives actuators to a safe state). Actuating outputs: ";
    for (size_t i = 0; i < result.actuating_functions.size(); ++i) {
        msg << (i != 0 ? ", " : "") << result.actuating_functions[i];
    }
    result.message = msg.str();
    return result;
}

}  // namespace runtime
}  // namespace anolis
