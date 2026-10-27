#include "runtime/actuation_gate.hpp"

#include <algorithm>
#include <sstream>
#include <string>

#include "automation/mode_manager.hpp"
#include "control/provider_value_bridge.hpp"
#include "control/safe_state.hpp"
#include "logging/logger.hpp"
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

}  // namespace

control::CallRequest mode_hook_request(const ModeTransitionCallConfig &call, automation::RuntimeMode next) {
    control::CallRequest request;
    request.device_handle = call.device_handle;
    request.function_id = call.function_id;
    request.function_name = call.function_name;
    request.is_automated = true;
    request.fault_entry_hook = next == automation::RuntimeMode::FAULT;
    for (const auto &[arg_name, arg_value] : call.args) {
        request.args[arg_name] = control::to_provider_value(arg_value);
    }
    return request;
}

// The gate is satisfied only by a declared hook that fires on `AUTO -> FAULT` and
// drives a safe state there (#232).
bool has_fault_safe_state_hook(const AutomationConfig &automation) {
    const auto &hooks = automation.mode_transition_hooks;
    return std::any_of(hooks.before_transition.begin(), hooks.before_transition.end(), hook_covers_auto_to_fault) ||
           std::any_of(hooks.after_transition.begin(), hooks.after_transition.end(), hook_covers_auto_to_fault);
}

std::vector<std::string> fault_hook_uncovered_functions(const registry::DeviceRegistry &registry,
                                                        const control::CallRouter &call_router,
                                                        const AutomationConfig &automation) {
    if (!automation.enabled || !has_fault_safe_state_hook(automation)) {
        return {};
    }
    std::vector<ModeTransitionCallConfig> calls;
    for (const auto *group :
         {&automation.mode_transition_hooks.before_transition, &automation.mode_transition_hooks.after_transition}) {
        for (const auto &hook : *group) {
            if (hook_covers_auto_to_fault(hook)) {
                calls.insert(calls.end(), hook.calls.begin(), hook.calls.end());
            }
        }
    }
    return control::uncovered_actuating_functions(registry, call_router, calls);
}

HooklessAutoGate evaluate_hookless_auto_gate(const registry::DeviceRegistry &registry,
                                             const AutomationConfig &automation, const SafetyConfig &safety) {
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

    // A machine declares its safe state twice over, for two different triggers,
    // and this gate only ever asks for one of them. An operator who follows the
    // sentence above to the letter satisfies the gate and still has a machine
    // whose e-stop ladder drives nothing: it runs only the hooks they just
    // added, which nothing checked as a safe state (#251). Say so here, where
    // they are reading, rather than leaving it to a startup warning they have
    // already scrolled past. Text only: the verdict does not depend on
    // safety.safe_state, because the ladder fires on operator request, never
    // autonomously.
    if (control::planned_safe_state_kind(registry, safety) == control::SafeStateKind::None) {
        msg << ". Note: that hook satisfies this gate but is NOT the e-stop's safe state — POST /v0/estop runs "
               "safety.safe_state, and on this inventory that ladder would drive nothing, so the e-stop would run "
               "only the -> FAULT hooks that match the mode it is pressed in (in IDLE, only after_transition "
               "hooks: the IDLE gate refuses before_transition calls) and report safe_state_achieved=false. "
               "Declare safety.safe_state (setpoints count only if they cover every actuating output)";
    }

    result.message = msg.str();
    return result;
}

void install_hookless_auto_gate(automation::ModeManager &mode_manager, const registry::DeviceRegistry &registry,
                                const AutomationConfig &automation, const SafetyConfig &safety) {
    const auto check = [&registry, &automation, &safety](automation::RuntimeMode /*prev*/, automation::RuntimeMode next,
                                                         std::string &veto_reason) {
        if (next != automation::RuntimeMode::AUTO) {
            return true;
        }
        const auto gate = evaluate_hookless_auto_gate(registry, automation, safety);
        if (gate.refused) {
            veto_reason = gate.message;
            return false;
        }
        return true;
    };
    mode_manager.on_before_mode_change(check);
    mode_manager.add_commit_guard(check);
}

void install_estop_auto_gate(automation::ModeManager &mode_manager, const control::IActuationLatch &latch) {
    const auto check = [&latch](automation::RuntimeMode /*prev*/, automation::RuntimeMode next,
                                std::string &veto_reason) {
        if (next != automation::RuntimeMode::AUTO || !latch.is_engaged()) {
            return true;
        }
        veto_reason =
            "refusing AUTO: the software stop's latch is engaged. Clear it (POST /v0/estop/clear) once the machine "
            "is safe, then enter AUTO.";
        return false;
    };
    mode_manager.on_before_mode_change(check);
    mode_manager.add_commit_guard(check);
}

bool enforce_hookless_gate_in_auto(const registry::DeviceRegistry &registry, const AutomationConfig &automation,
                                   const SafetyConfig &safety, automation::ModeManager &mode_manager,
                                   const std::string &context) {
    // Only autonomous actuation (AUTO) is at risk; cheapest check first, and it
    // skips the O(inventory) scan on the common non-AUTO restart.
    if (mode_manager.current_mode() != automation::RuntimeMode::AUTO) {
        return false;
    }

    // evaluate_hookless_auto_gate already returns non-refused when automation is
    // disabled or nothing actuates, so a same-inventory restart is a no-op here.
    const auto gate = evaluate_hookless_auto_gate(registry, automation, safety);
    if (!gate.refused) {
        return false;
    }

    LOG_ERROR("[Runtime] " << context << " changed the device inventory while in AUTO: " << gate.message);
    std::string err;
    if (!mode_manager.set_mode(automation::RuntimeMode::FAULT, err)) {
        // Unreachable: a FAULT request always ends in FAULT, including when
        // another transition commits during its before-callbacks (#528).
        LOG_ERROR("[Runtime] Failed to force FAULT after in-AUTO gate refusal: " << err);
    } else {
        LOG_ERROR(
            "[Runtime] Forcing FAULT: autonomous actuation halted; manual control and "
            "/v0/estop remain available.");
    }
    return true;
}

}  // namespace runtime
}  // namespace anolis
