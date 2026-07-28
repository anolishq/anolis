#include "control/safe_state.hpp"

#include <chrono>

#include "automation/mode_manager.hpp"
#include "control/call_router.hpp"
#include "control/provider_value_bridge.hpp"
#include "logging/logger.hpp"
#include "provider/provider_registry.hpp"

namespace anolis {
namespace control {

namespace {

int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A zero/neutral Value of the given type for rung-3 zeroing. Non-numeric types
// (string/bytes) return nullopt: we refuse to invent a "zero" for them.
std::optional<anolis::deviceprovider::v1::Value> zero_value(anolis::deviceprovider::v1::ValueType type) {
    anolis::deviceprovider::v1::Value value;
    value.set_type(type);
    switch (type) {
        case anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE:
            value.set_double_value(0.0);
            return value;
        case anolis::deviceprovider::v1::VALUE_TYPE_INT64:
            value.set_int64_value(0);
            return value;
        case anolis::deviceprovider::v1::VALUE_TYPE_UINT64:
            value.set_uint64_value(0);
            return value;
        case anolis::deviceprovider::v1::VALUE_TYPE_BOOL:
            value.set_bool_value(false);
            return value;
        default:
            return std::nullopt;
    }
}

// True if `setpoint` targets the same function as an actuating `spec` on `handle`.
// When both selectors are present they must agree (mirroring CallRouter, which
// requires id and name to resolve to the same function), so a contradictory
// setpoint does not count toward coverage.
bool setpoint_matches(const runtime::ModeTransitionCallConfig &setpoint, const std::string &handle,
                      const registry::FunctionSpec &spec) {
    if (setpoint.device_handle != handle) {
        return false;
    }
    const bool has_id = setpoint.function_id != 0;
    const bool has_name = !setpoint.function_name.empty();
    if (has_id && setpoint.function_id != spec.function_id) {
        return false;
    }
    if (has_name && setpoint.function_name != spec.function_name) {
        return false;
    }
    return has_id || has_name;
}

}  // namespace

const char *safe_state_kind_to_string(SafeStateKind kind) {
    switch (kind) {
        case SafeStateKind::Hooks:
            return "hooks";
        case SafeStateKind::Setpoints:
            return "setpoints";
        case SafeStateKind::Zero:
            return "zero";
        case SafeStateKind::None:
        default:
            return "none";
    }
}

SafeStateController::SafeStateController(const registry::DeviceRegistry &registry, CallRouter &call_router,
                                         provider::ProviderRegistry &provider_registry,
                                         const runtime::SafetyConfig &safety)
    : registry_(registry), call_router_(call_router), provider_registry_(provider_registry), safety_(safety) {}

void SafeStateController::set_mode_manager(automation::ModeManager *mode_manager) { mode_manager_ = mode_manager; }

bool SafeStateController::is_engaged() const { return latched_.load(std::memory_order_acquire); }

std::vector<std::pair<std::string, registry::FunctionSpec>> SafeStateController::actuating_functions() const {
    // The ladder EXECUTES against explicitly-declared actuators only. This is
    // deliberately narrower than the CallRouter latch's fail-closed
    // `is_actuating` (any not-READ function): the latch refuses unknown calls
    // (conservative about ALLOWING), while the ladder must not blindly invoke a
    // CONFIG change or an untagged command to "make it safe" (conservative about
    // ACTING). Untagged actuators are still latched; declare a hook/setpoint to
    // drive them.
    std::vector<std::pair<std::string, registry::FunctionSpec>> out;
    for (const auto &device : registry_.get_all_devices()) {
        for (const auto &[name, spec] : device.capabilities.functions_by_id) {
            (void)name;
            if (spec.category == anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE) {
                out.emplace_back(device.get_handle(), spec);
            }
        }
    }
    return out;
}

SafeStateKind SafeStateController::planned_kind(size_t *uncovered_out) const {
    const auto actuators = actuating_functions();
    size_t uncovered = 0;
    for (const auto &[handle, spec] : actuators) {
        bool covered = false;
        for (const auto &setpoint : safety_.safe_state.setpoints) {
            if (setpoint_matches(setpoint, handle, spec)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            ++uncovered;
        }
    }
    if (uncovered_out != nullptr) {
        *uncovered_out = uncovered;
    }

    if (!safety_.safe_state.hooks.empty()) {
        return SafeStateKind::Hooks;
    }
    // Setpoints apply only if they cover every actuating output (fail closed).
    if (!safety_.safe_state.setpoints.empty() && uncovered == 0) {
        return SafeStateKind::Setpoints;
    }
    if (safety_.safe_state.zero_is_safe) {
        return SafeStateKind::Zero;
    }
    return SafeStateKind::None;
}

CallOutcome SafeStateController::run_call(const runtime::ModeTransitionCallConfig &call) {
    CallRequest request;
    request.device_handle = call.device_handle;
    request.function_id = call.function_id;
    request.function_name = call.function_name;
    request.is_automated = false;
    request.safe_state = true;
    for (const auto &[name, value] : call.args) {
        request.args[name] = to_provider_value(value);
    }

    const auto res = call_router_.execute_call(request, provider_registry_);

    CallOutcome outcome;
    outcome.device_handle = call.device_handle;
    outcome.function = call.function_name.empty() ? std::to_string(call.function_id) : call.function_name;
    outcome.success = res.success;
    if (!res.success) {
        outcome.error = res.error_message;
    }
    return outcome;
}

CallOutcome SafeStateController::run_zero_call(const std::string &device_handle, const registry::FunctionSpec &spec) {
    CallOutcome outcome;
    outcome.device_handle = device_handle;
    outcome.function = spec.function_name.empty() ? std::to_string(spec.function_id) : spec.function_name;

    CallRequest request;
    request.device_handle = device_handle;
    request.function_id = spec.function_id;
    request.function_name = spec.function_name;
    request.is_automated = false;
    request.safe_state = true;
    for (const auto &arg : spec.args) {
        if (!arg.required()) {
            continue;
        }
        auto zero = zero_value(arg.type());
        if (!zero.has_value()) {
            outcome.success = false;
            outcome.error = "cannot zero required non-numeric argument '" + arg.name() + "'";
            return outcome;
        }
        request.args[arg.name()] = *zero;
    }

    // Refuse to bare-invoke a function with nothing to zero: "zero the outputs"
    // means driving a numeric setpoint to 0, not calling a no-argument command
    // (which could energize hardware). Still latches; the operator should
    // declare a hook for such a function.
    if (request.args.empty()) {
        outcome.success = false;
        outcome.error = "cannot zero: no numeric required argument to drive to zero";
        return outcome;
    }

    const auto res = call_router_.execute_call(request, provider_registry_);
    outcome.success = res.success;
    if (!res.success) {
        outcome.error = res.error_message;
    }
    return outcome;
}

EstopResult SafeStateController::run_ladder() {
    EstopResult result;
    result.kind = planned_kind(nullptr);
    switch (result.kind) {
        case SafeStateKind::Hooks:
            for (const auto &call : safety_.safe_state.hooks) {
                result.actions.push_back(run_call(call));
            }
            break;
        case SafeStateKind::Setpoints:
            for (const auto &call : safety_.safe_state.setpoints) {
                result.actions.push_back(run_call(call));
            }
            break;
        case SafeStateKind::Zero:
            for (const auto &[handle, spec] : actuating_functions()) {
                result.actions.push_back(run_zero_call(handle, spec));
            }
            break;
        case SafeStateKind::None:
            break;
    }
    return result;
}

EstopResult SafeStateController::trigger(const std::string &reason) {
    // The mutex is held across the whole ladder so a trigger is atomic with
    // respect to clear() and concurrent triggers. Consequence: capability()
    // (the status surface) and clear() block for the ladder's duration, which
    // is bounded by provider call timeouts. This is acceptable for a rare
    // safety stop; is_engaged() stays lock-free so the CallRouter latch check is
    // never blocked.
    std::lock_guard<std::mutex> lock(mutex_);
    latched_.store(true, std::memory_order_release);
    latched_at_ms_ = now_epoch_ms();
    LOG_WARN("[SafeState] E-stop engaged" << (reason.empty() ? "" : (": " + reason)));

    EstopResult result = run_ladder();

    // On an automation machine, also drive FAULT: any->FAULT is always valid and
    // cannot be vetoed, so it stops the behavior tree and routes the operator to
    // the FAULT->MANUAL recovery path. Actuating FAULT-entry hooks are refused by
    // the latch (the ladder above is the single safe-state authority).
    if (mode_manager_ != nullptr) {
        result.fault_requested = true;
        std::string err;
        result.fault_ok = mode_manager_->set_mode(automation::RuntimeMode::FAULT, err);
        if (!result.fault_ok) {
            LOG_WARN("[SafeState] Could not drive FAULT on e-stop: " << err);
        }
    }

    LOG_INFO("[SafeState] E-stop ladder=" << safe_state_kind_to_string(result.kind)
                                          << " actions=" << result.actions.size());
    return result;
}

void SafeStateController::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latched_.store(false, std::memory_order_release);
    latched_at_ms_.reset();
    LOG_INFO("[SafeState] E-stop latch cleared");
}

EstopCapability SafeStateController::capability() const {
    EstopCapability cap;
    size_t uncovered = 0;
    cap.software_safe_state = planned_kind(&uncovered);
    cap.uncovered_actuating_functions = uncovered;
    {
        // Read latch flag and timestamp together so the pair is consistent
        // against a concurrent clear() (no {latched:true, latched_at:null}).
        std::lock_guard<std::mutex> lock(mutex_);
        cap.latched = latched_.load(std::memory_order_acquire);
        cap.latched_at_epoch_ms = latched_at_ms_;
    }
    return cap;
}

}  // namespace control
}  // namespace anolis
