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

namespace anolis {

namespace registry {
class DeviceRegistry;
}

namespace automation {
class ModeManager;
}

namespace runtime {

struct AutomationConfig;

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
 * Read-only; safe to call on every AUTO transition attempt.
 */
HooklessAutoGate evaluate_hookless_auto_gate(const registry::DeviceRegistry &registry,
                                             const AutomationConfig &automation);

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
                                   automation::ModeManager &mode_manager, const std::string &context);

}  // namespace runtime
}  // namespace anolis
