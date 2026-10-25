#pragma once

/**
 * @file actuation_gate.hpp
 * @brief Refuse-hookless safety gate (decision D6).
 *
 * Guarantees that autonomous actuation (AUTO) cannot start when the config
 * declares actuating outputs but no declared `mode_transition_hooks` entry
 * drives a safe state on the autonomous `AUTO -> FAULT` transition (a hook whose
 * `from` matches AUTO and `to` matches FAULT, each `AUTO`/`FAULT` / `*` /
 * omitted). The predicate is evaluated post-provider-discovery (it needs the
 * live capability inventory) and enforced as a veto at the single choke point
 * into AUTO.
 */

#include <string>
#include <vector>

#include "automation/mode_manager.hpp"
#include "control/call_router.hpp"

namespace anolis {

namespace registry {
class DeviceRegistry;
}

namespace automation {
class ModeManager;
}

namespace control {
class CallRouter;
}

namespace runtime {

struct AutomationConfig;
struct SafetyConfig;
struct ModeTransitionCallConfig;

/**
 * @brief The request a mode-transition hook issues for `call` on a transition
 *        into `next`.
 *
 * Marked automated, and tagged `fault_entry_hook` when `next` is FAULT, which
 * is what lets the e-stop's own FAULT entry run it under rung none (#251).
 * Both the before- and after-transition hooks build their calls here.
 */
control::CallRequest mode_hook_request(const ModeTransitionCallConfig &call, automation::RuntimeMode next);

/**
 * @brief True when the config declares a hook that fires on `AUTO -> FAULT`.
 *
 * Exposed so callers that need to reason about "does this machine have a
 * declared fault safe-state path" use the SAME predicate the gate admits AUTO
 * on. Two implementations of this question drifting apart is exactly how a
 * machine ends up gated as safe while its e-stop drives nothing (#251).
 */
bool has_fault_safe_state_hook(const AutomationConfig &automation);

/**
 * @brief ACTUATE functions that no dispatchable `AUTO -> FAULT` hook call
 *        drives, as "provider/device/function" (#246).
 *
 * The gate admits AUTO on a hook's `from`/`to` filters alone, so a hook that
 * drives some outputs satisfies it for all of them. This lists the rest, using
 * the safe-state ladder's coverage rule (`control::uncovered_actuating_functions`:
 * a call covers a function only if it targets it and passes
 * `CallRouter::validate_call`). Report-only: the gate's verdict does not use it.
 *
 * Empty when automation is disabled or no hook fires on `AUTO -> FAULT`; the
 * gate then refuses AUTO itself and names every actuating output.
 */
std::vector<std::string> fault_hook_uncovered_functions(const registry::DeviceRegistry &registry,
                                                        const control::CallRouter &call_router,
                                                        const AutomationConfig &automation);

/** @brief Outcome of the refuse-hookless evaluation. */
struct HooklessAutoGate {
    bool refused = false;
    std::vector<std::string> actuating_functions;  // "provider/device/function"
    std::string message;                           // populated when refused
};

/**
 * @brief Decide whether entering AUTO must be refused.
 *
 * Refuses iff automation is enabled AND at least one discovered function is
 * actuating (fail-closed via `registry::is_actuating` — any function not
 * explicitly read-only) AND no declared hook covers the `AUTO -> FAULT`
 * transition (in either list: `from` = AUTO/`*`/omitted AND `to` =
 * FAULT/`*`/omitted). A hook covering only another transition (e.g.
 * `AUTO->MANUAL`, or an `IDLE->FAULT` hook that never fires from AUTO) does not
 * satisfy the gate. The manual e-stop path (`safety.safe_state`) deliberately
 * does NOT satisfy this gate either: it fires only on operator `POST /v0/estop`,
 * never on autonomous transitions.
 *
 * `safety` does not change the verdict — only the refusal text. Following this
 * message names one declaration and produces a machine with no manual e-stop
 * path, which is the configuration #251 makes dangerous; when the ladder would
 * drive nothing the message says so rather than leaving the operator to
 * discover it (#258).
 *
 * "Would drive nothing" is `control::planned_safe_state_kind`, not a local
 * inspection of the config: the setpoints rung counts only when it covers every
 * actuating output, so a config with setpoints covering nothing declares a safe
 * state that plans `None`. Answering this question twice is how a machine gets
 * reported as safe while its e-stop does nothing.
 *
 * Read-only; safe to call on every AUTO transition attempt.
 */
HooklessAutoGate evaluate_hookless_auto_gate(const registry::DeviceRegistry &registry,
                                             const AutomationConfig &automation, const SafetyConfig &safety);

/**
 * @brief Install the refuse-hookless gate on every transition into AUTO.
 *
 * Registers the gate twice on `mode_manager`. As a before-callback it refuses
 * early, ahead of the before_transition hooks, so a refused AUTO entry issues
 * no hook calls. As a commit guard it re-runs against the registry under the
 * mode lock, immediately before AUTO is written (#239). The commit guard is
 * what closes the race: an inventory committed by a provider restart while
 * the transition's callbacks run is seen either here, or, once AUTO has been
 * written, by `enforce_hookless_gate_in_auto` in the restart path.
 *
 * Lock order: the guard takes the registry's shared lock while the mode lock
 * is held. The registry never reaches the ModeManager, so nothing takes them
 * in the other order.
 *
 * `registry`, `automation` and `safety` must outlive `mode_manager`.
 */
void install_hookless_auto_gate(automation::ModeManager &mode_manager, const registry::DeviceRegistry &registry,
                                const AutomationConfig &automation, const SafetyConfig &safety);

/**
 * @brief Re-enforce the refuse-hookless gate while already in AUTO (#233).
 *
 * The entry-time gate runs only on `MANUAL -> AUTO`. A provider restart can
 * publish a replacement inventory that exposes a new actuating function with no
 * `AUTO -> FAULT` safe-state hook, bypassing it. This re-check closes that
 * fail-open: it is a no-op unless `mode_manager.current_mode() == AUTO`; when the
 * gate refuses against the live registry it logs ERROR (with `context`, e.g.
 * "provider 'x' restart") and forces `FAULT` (which cannot be vetoed), halting
 * autonomous actuation. Manual control and `POST /v0/estop` remain available.
 *
 * @return true iff the gate refused and FAULT was requested.
 */
bool enforce_hookless_gate_in_auto(const registry::DeviceRegistry &registry, const AutomationConfig &automation,
                                   const SafetyConfig &safety, automation::ModeManager &mode_manager,
                                   const std::string &context);

}  // namespace runtime
}  // namespace anolis
