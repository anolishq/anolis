#include "control/safe_state.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <thread>

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
// (bool/string/bytes) return nullopt: we refuse to invent a "zero" for them.
// A bool has no knowable safe pole -- false disables an enable flag but
// releases a brake flag -- so a function that needs one is reported, not sent.
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
        default:
            return std::nullopt;
    }
}

// Rung 3's arguments for `spec`: every required argument zeroed. Returns why
// rung 3 cannot drive the function, or an empty string when `args` is complete.
std::string build_zero_args(const registry::FunctionSpec &spec,
                            std::map<std::string, anolis::deviceprovider::v1::Value> &args) {
    for (const auto &arg : spec.args) {
        if (!arg.required()) {
            continue;
        }
        auto zero = zero_value(arg.type());
        if (!zero.has_value()) {
            return "cannot zero required non-numeric argument '" + arg.name() + "'";
        }
        args[arg.name()] = *zero;
    }

    // Refuse to bare-invoke a function with nothing to zero: "zero the outputs"
    // means driving a numeric setpoint to 0, not calling a no-argument command
    // (which could energize hardware). Still latches; the operator should
    // declare a hook for such a function.
    if (args.empty()) {
        return "cannot zero: no numeric required argument to drive to zero";
    }
    return {};
}

CallRequest to_call_request(const runtime::ModeTransitionCallConfig &call) {
    CallRequest request;
    request.device_handle = call.device_handle;
    request.function_id = call.function_id;
    request.function_name = call.function_name;
    request.is_automated = false;
    request.safe_state = true;
    for (const auto &[name, value] : call.args) {
        request.args[name] = to_provider_value(value);
    }
    return request;
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

// The ladder EXECUTES against explicitly-declared actuators only. This is
// deliberately narrower than the CallRouter latch's fail-closed `is_actuating`
// (any not-READ function): the latch refuses unknown calls (conservative about
// ALLOWING), while the ladder must not blindly invoke a CONFIG change or an
// untagged command to "make it safe" (conservative about ACTING). Untagged
// actuators are still latched; declare a hook/setpoint to drive them.
std::vector<std::pair<std::string, registry::FunctionSpec>> actuating_functions_for(
    const registry::DeviceRegistry &registry) {
    std::vector<std::pair<std::string, registry::FunctionSpec>> out;
    for (const auto &device : registry.get_all_devices()) {
        for (const auto &[name, spec] : device.capabilities.functions_by_id) {
            (void)name;
            if (spec.category == anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE) {
                out.emplace_back(device.get_handle(), spec);
            }
        }
    }
    return out;
}

}  // namespace

std::vector<std::string> uncovered_actuating_functions(const registry::DeviceRegistry &registry,
                                                       const CallRouter &call_router,
                                                       const std::vector<runtime::ModeTransitionCallConfig> &calls) {
    std::vector<std::string> out;
    for (const auto &[handle, spec] : actuating_functions_for(registry)) {
        bool covered = false;
        for (const auto &call : calls) {
            std::string err;
            if (setpoint_matches(call, handle, spec) && call_router.validate_call(to_call_request(call), err)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            out.push_back(handle + "/" + spec.function_name);
        }
    }
    return out;
}

const char *safe_state_kind_to_string(SafeStateKind kind) {
    switch (kind) {
        case SafeStateKind::Hooks:
            return "hooks";
        case SafeStateKind::Setpoints:
            return "setpoints";
        case SafeStateKind::Zero:
            return "zero";
        case SafeStateKind::Mixed:
            return "mixed";
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

bool SafeStateController::admits_fault_entry_hooks() const {
    return fault_entry_hook_thread_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

namespace {

// One entry of a worked-out plan, in the order the stop runs them.
struct PlanStep {
    enum class Kind {
        Call,       // a declared hook or setpoint, sent as declared
        Zero,       // rung-3 zero for `spec`, which dispatches
        NotDriven,  // `spec` has no call that dispatches; reported, not sent
    };
    Kind kind = Kind::Call;
    const runtime::ModeTransitionCallConfig *call = nullptr;  // Call
    std::string handle;                                       // Zero, NotDriven
    registry::FunctionSpec spec;                              // Zero, NotDriven
    std::string why;                                          // NotDriven
};

struct WorkedPlan {
    SafeStatePlan summary;
    std::vector<PlanStep> steps;
};

// Whether rung 3's zero call for `spec` would dispatch; why not, when not.
bool zero_dispatches(const CallRouter &call_router, const std::string &handle, const registry::FunctionSpec &spec,
                     std::string &why) {
    CallRequest request;
    request.device_handle = handle;
    request.function_id = spec.function_id;
    request.function_name = spec.function_name;
    request.safe_state = true;
    why = build_zero_args(spec, request.args);
    if (!why.empty()) {
        return false;
    }
    return call_router.validate_call(request, why);
}

// The plan, as plan_safe_state() documents it. Every caller that needs the
// plan, the stop itself included, goes through here, so the status surface,
// the gate, the preflight and the stop cannot answer differently.
WorkedPlan work_out_plan(const registry::DeviceRegistry &registry, const CallRouter &call_router,
                         const runtime::SafetyConfig &safety) {
    WorkedPlan plan;
    const auto &declared = safety.safe_state;
    const auto actuating = actuating_functions_for(registry);
    plan.summary.actuating_functions = actuating.size();

    if (!declared.hooks.empty()) {
        plan.summary.kind = SafeStateKind::Hooks;
        for (const auto &call : declared.hooks) {
            plan.steps.push_back({PlanStep::Kind::Call, &call, {}, {}, {}});
        }
        plan.summary.uncovered = uncovered_actuating_functions(registry, call_router, declared.hooks);
        return plan;
    }
    if (declared.setpoints.empty() && !declared.zero_is_safe) {
        for (const auto &[handle, spec] : actuating) {
            plan.summary.uncovered.push_back(handle + "/" + spec.function_name);
        }
        return plan;  // None: nothing declared
    }

    const auto &setpoints = declared.setpoints;
    std::vector<char> dispatches(setpoints.size(), 0);
    std::vector<char> targets_actuator(setpoints.size(), 0);
    std::vector<std::string> refusal(setpoints.size());
    for (size_t i = 0; i < setpoints.size(); ++i) {
        dispatches[i] = call_router.validate_call(to_call_request(setpoints[i]), refusal[i]) ? 1 : 0;
    }

    // Per output: its setpoint, else its zero, else reported not driven.
    std::vector<PlanStep> per_output;
    bool zeroed = false;
    bool by_setpoint = false;
    for (const auto &[handle, spec] : actuating) {
        bool driven = false;
        std::string why;
        std::vector<size_t> refused;  // this function's setpoints that do not dispatch
        for (size_t i = 0; i < setpoints.size(); ++i) {
            if (!setpoint_matches(setpoints[i], handle, spec)) {
                continue;
            }
            targets_actuator[i] = 1;
            if (dispatches[i] != 0) {
                driven = true;
            } else {
                refused.push_back(i);
                if (why.empty()) {
                    why = std::format("setpoint: {}", refusal[i]);
                }
            }
        }
        if (driven) {
            by_setpoint = true;
            // One function can drive several outputs, one setpoint per index
            // (motor_index 1 and 2). A sibling that dispatches does not drive
            // the output a refused setpoint was declared for, so each refused
            // one is reported not driven, and nothing is sent for it.
            for (const size_t i : refused) {
                per_output.push_back(
                    {PlanStep::Kind::NotDriven, nullptr, handle, spec, std::format("setpoint: {}", refusal[i])});
            }
            continue;
        }
        if (declared.zero_is_safe) {
            std::string zero_why;
            if (zero_dispatches(call_router, handle, spec, zero_why)) {
                per_output.push_back({PlanStep::Kind::Zero, nullptr, handle, spec, {}});
                zeroed = true;
                continue;
            }
            why = why.empty() ? zero_why : std::format("{}; zero: {}", why, zero_why);
        } else if (why.empty()) {
            why = "no setpoint drives it and zero_is_safe is not set";
        }
        per_output.push_back({PlanStep::Kind::NotDriven, nullptr, handle, spec, why});
        plan.summary.uncovered.push_back(handle + "/" + spec.function_name);
    }

    // Setpoints go out in declared order: each that dispatches, and each that
    // targets no registered ACTUATE function, as before. One for an ACTUATE
    // function that does not dispatch is not sent: it is reported not driven
    // above, unless no setpoint for that function dispatches and its zero
    // drives the function instead (#640, owner decision).
    for (size_t i = 0; i < setpoints.size(); ++i) {
        if (dispatches[i] != 0 || targets_actuator[i] == 0) {
            plan.steps.push_back({PlanStep::Kind::Call, &setpoints[i], {}, {}, {}});
        }
    }
    plan.steps.insert(plan.steps.end(), per_output.begin(), per_output.end());

    // The kind says how the actuating outputs are driven. A setpoint sent to
    // anything else (another category, a device not present) does not change it.
    if (by_setpoint && zeroed) {
        plan.summary.kind = SafeStateKind::Mixed;
    } else if (by_setpoint) {
        plan.summary.kind = SafeStateKind::Setpoints;
    } else if (zeroed) {
        plan.summary.kind = SafeStateKind::Zero;
    } else {
        // No output is driven: name what was declared.
        plan.summary.kind = setpoints.empty() ? SafeStateKind::Zero : SafeStateKind::Setpoints;
    }
    return plan;
}

}  // namespace

SafeStatePlan plan_safe_state(const registry::DeviceRegistry &registry, const CallRouter &call_router,
                              const runtime::SafetyConfig &safety) {
    return work_out_plan(registry, call_router, safety).summary;
}

SafeStateKind planned_safe_state_kind(const registry::DeviceRegistry &registry, const CallRouter &call_router,
                                      const runtime::SafetyConfig &safety) {
    return plan_safe_state(registry, call_router, safety).kind;
}

const char *failure_cause_to_string(FailureCause cause) {
    switch (cause) {
        case FailureCause::Run:
            return "run";
        case FailureCause::Config:
            return "config";
    }
    return "run";
}

namespace {

// Refused before it reached the provider, for a reason the config fixes: the
// device or function does not exist, or an argument does not validate. Gating
// (FAILED_PRECONDITION) and an unavailable provider are the runtime's state,
// not the config's.
FailureCause cause_of(const CallResult &res) {
    if (!res.dispatched && (res.status_code == anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND ||
                            res.status_code == anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT)) {
        return FailureCause::Config;
    }
    return FailureCause::Run;
}

}  // namespace

CallOutcome SafeStateController::run_call(const runtime::ModeTransitionCallConfig &call) {
    const auto res = call_router_.execute_call(to_call_request(call), provider_registry_);

    CallOutcome outcome;
    outcome.device_handle = call.device_handle;
    outcome.function = call.function_name.empty() ? std::to_string(call.function_id) : call.function_name;
    outcome.success = res.success;
    if (!res.success) {
        outcome.error = res.error_message;
        outcome.cause = cause_of(res);
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
    if (auto why = build_zero_args(spec, request.args); !why.empty()) {
        outcome.success = false;
        outcome.error = std::move(why);
        outcome.cause = FailureCause::Config;
        outcome.sent = false;
        return outcome;
    }

    const auto res = call_router_.execute_call(request, provider_registry_);
    outcome.success = res.success;
    if (!res.success) {
        outcome.error = res.error_message;
        outcome.cause = cause_of(res);
    }
    return outcome;
}

EstopResult SafeStateController::run_ladder(const std::string *only_device, const AbsentDevices *absent) {
    const auto wanted = [only_device](const std::string &handle) {
        return only_device == nullptr || handle == *only_device;
    };

    EstopResult result;
    const WorkedPlan plan = work_out_plan(registry_, call_router_, safety_);
    result.kind = plan.summary.kind;
    result.drives_no_output = plan.summary.drives_no_output();
    for (const auto &step : plan.steps) {
        const std::string &handle = step.kind == PlanStep::Kind::Call ? step.call->device_handle : step.handle;
        if (!wanted(handle)) {
            continue;
        }
        const std::string function =
            step.kind == PlanStep::Kind::Call
                ? (step.call->function_name.empty() ? std::to_string(step.call->function_id) : step.call->function_name)
                : step.spec.function_name;
        // An entry for a device in `absent` is recorded, not run: one its
        // provider reports missing (anolishq/anolis#461), or one behind a stop
        // chain in force at startup (#483). Zero and not-driven entries check
        // too: a device behind the stop is registered, only unpowered.
        if (absent != nullptr) {
            if (const auto it = absent->find(handle); it != absent->end()) {
                result.skipped.push_back({handle, function, it->second});
                continue;
            }
        }
        switch (step.kind) {
            case PlanStep::Kind::Call:
                result.actions.push_back(run_call(*step.call));
                break;
            case PlanStep::Kind::Zero:
                result.actions.push_back(run_zero_call(handle, step.spec));
                break;
            case PlanStep::Kind::NotDriven:
                result.actions.push_back({handle, function, false, step.why, FailureCause::Config, false});
                break;
        }
    }
    // No fall-through at run time: a call that failed then mostly failed on
    // transport, and another call would go to the same device. The verdict
    // says so instead.
    result.safe_state_achieved =
        result.kind != SafeStateKind::None && !result.actions.empty() &&
        std::all_of(result.actions.begin(), result.actions.end(), [](const CallOutcome &a) { return a.success; });
    return result;
}

EstopResult SafeStateController::reassert_for(const std::string &device_handle) { return run_ladder(&device_handle); }

EstopResult SafeStateController::drive_safe_state(const AbsentDevices &absent) {
    EstopResult result = run_ladder(nullptr, &absent);

    StartupSafeStateReport report;
    report.ran = result.kind != SafeStateKind::None;
    report.kind = result.kind;
    report.safe_state_achieved = result.safe_state_achieved;
    for (const auto &action : result.actions) {
        if (!action.success) {
            report.failed_actions.push_back(action);
        }
    }
    report.skipped_actions = result.skipped;
    std::lock_guard<std::mutex> lock(mutex_);
    startup_report_ = std::move(report);
    return result;
}

StartupSafeStateReport SafeStateController::startup_report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StartupSafeStateReport report = startup_report_;
    report.on_startup = safety_.on_startup;
    return report;
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
    restored_reason_.reset();
    LOG_WARN("[SafeState] E-stop engaged" << (reason.empty() ? "" : (": " + reason)));
    // On disk before the ladder runs, so a crash or restart mid-ladder still
    // comes back latched (#482).
    if (latch_store_ != nullptr) {
        std::string error;
        if (!latch_store_->save(*latched_at_ms_, reason, error)) {
            LOG_ERROR("[SafeState] E-stop latch not persisted (" << error << "); a restart would come back unlatched");
        }
    }

    EstopResult result = run_ladder();

    // A skipped zero-rung call never reaches the CallRouter, so this is the
    // only place it is reported. Same shape as the re-assert path's warning.
    for (const auto &action : result.actions) {
        if (!action.success) {
            LOG_WARN("[SafeState] E-stop did not drive " << action.device_handle << "/" << action.function << ": "
                                                         << action.error);
        }
    }

    // On an automation machine, also drive FAULT: any->FAULT is always valid and
    // cannot be vetoed, so it stops the behavior tree and routes the operator to
    // the FAULT->MANUAL recovery path. While the plan drives an output, actuating
    // FAULT-entry hooks are refused by the latch: the plan above is the single
    // safe-state authority. When it drives none (nothing declared, or nothing
    // declared dispatches), nothing else drives those actuators, so this FAULT
    // entry's hooks are let through -- on this thread, for this set_mode only
    // (#251). safe_state_achieved stays false: the plan drove nothing.
    if (mode_manager_ != nullptr) {
        result.fault_requested = true;
        std::string err;
        const bool admit_hooks = result.drives_no_output;
        if (admit_hooks) {
            LOG_WARN(
                "[SafeState] E-stop plan drives no actuating output; running the -> FAULT mode-transition hooks "
                "this FAULT entry fires");
        }
        // Withdrawn however set_mode returns, so the admission never outlives it.
        struct HookAdmission {
            std::atomic<std::thread::id> &thread;
            HookAdmission(std::atomic<std::thread::id> &slot, bool admit) : thread(slot) {
                if (admit) {
                    thread.store(std::this_thread::get_id(), std::memory_order_release);
                }
            }
            HookAdmission(const HookAdmission &) = delete;
            HookAdmission &operator=(const HookAdmission &) = delete;
            ~HookAdmission() { thread.store(std::thread::id{}, std::memory_order_release); }
        };
        const HookAdmission admission(fault_entry_hook_thread_, admit_hooks);
        result.fault_ok = mode_manager_->set_mode(automation::RuntimeMode::FAULT, err);
        if (!result.fault_ok) {
            LOG_WARN("[SafeState] Could not drive FAULT on e-stop: " << err);
        }
    }

    if (result.safe_state_achieved) {
        LOG_INFO("[SafeState] E-stop ladder=" << safe_state_kind_to_string(result.kind)
                                              << " actions=" << result.actions.size() << " safe_state_achieved=true");
    } else {
        LOG_WARN("[SafeState] E-stop ladder=" << safe_state_kind_to_string(result.kind)
                                              << " actions=" << result.actions.size() << " safe_state_achieved=false");
    }
    return result;
}

std::optional<std::string> SafeStateController::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clear_guard_) {
        if (auto refusal = clear_guard_()) {
            LOG_WARN("[SafeState] E-stop latch clear refused: " << *refusal);
            return refusal;
        }
    }
    latched_.store(false, std::memory_order_release);
    latched_at_ms_.reset();
    restored_reason_.reset();
    LOG_INFO("[SafeState] E-stop latch cleared");
    if (latch_store_ != nullptr) {
        std::string error;
        if (!latch_store_->remove(error)) {
            // Fails toward held: a file left behind latches the next start.
            LOG_WARN("[SafeState] E-stop latch file not removed (" << error
                                                                   << "); the next start will come back latched");
        }
    }
    return std::nullopt;
}

void SafeStateController::set_clear_guard(std::function<std::optional<std::string>()> guard) {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_guard_ = std::move(guard);
}

void SafeStateController::hold(const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_.load(std::memory_order_acquire)) {
            // Already held: the latch keeps the time and file of when it was
            // engaged, and the stop still enters FAULT below (#547).
            LOG_WARN("[SafeState] E-stop latch already held: " << reason << ". No safe-state calls are sent.");
        } else {
            latched_.store(true, std::memory_order_release);
            latched_at_ms_ = now_epoch_ms();
            restored_reason_.reset();
            LOG_WARN("[SafeState] E-stop latch held: " << reason << ". No safe-state calls are sent.");
            if (latch_store_ != nullptr) {
                std::string error;
                if (!latch_store_->save(*latched_at_ms_, reason, error)) {
                    LOG_ERROR("[SafeState] E-stop latch not persisted (" << error
                                                                         << "); a restart would come back unlatched");
                }
            }
        }
    }
    // FAULT outside mutex_: set_mode runs the -> FAULT hooks, whose actuating
    // calls the latch now refuses; nothing here admits them.
    if (mode_manager_ != nullptr) {
        std::string err;
        if (!mode_manager_->set_mode(automation::RuntimeMode::FAULT, err)) {
            LOG_ERROR("[SafeState] E-stop hold could not enter FAULT: " << err);
        }
    }
}

void SafeStateController::attach_latch_store(std::unique_ptr<LatchStore> store) {
    std::lock_guard<std::mutex> lock(mutex_);
    latch_store_ = std::move(store);
    if (latch_store_ == nullptr) {
        return;
    }
    const PersistedLatch persisted = latch_store_->load();
    if (persisted.state == PersistedLatch::State::Absent) {
        return;
    }
    latched_.store(true, std::memory_order_release);
    // latched_at is never null while latched; a file that cannot say when
    // gets the time it was found.
    latched_at_ms_ = persisted.latched_at_epoch_ms.value_or(now_epoch_ms());
    restored_reason_ = std::format("started latched: {}", persisted.detail);
    LOG_WARN("[SafeState] E-stop " << *restored_reason_
                                   << "; clear it (POST /v0/estop/clear) once the machine is "
                                      "safe");
}

EstopCapability SafeStateController::capability() const {
    EstopCapability cap;
    // Kind and uncovered list from one evaluation of the plan, so the pair
    // cannot disagree (#640).
    auto plan = plan_safe_state(registry_, call_router_, safety_);
    cap.software_safe_state = plan.kind;
    cap.uncovered_functions = std::move(plan.uncovered);
    cap.uncovered_actuating_functions = cap.uncovered_functions.size();
    {
        // Read latch flag and timestamp together so the pair is consistent
        // against a concurrent clear() (no {latched:true, latched_at:null}).
        std::lock_guard<std::mutex> lock(mutex_);
        cap.latched = latched_.load(std::memory_order_acquire);
        cap.latched_at_epoch_ms = latched_at_ms_;
        cap.restored_reason = restored_reason_;
    }
    return cap;
}

}  // namespace control
}  // namespace anolis
