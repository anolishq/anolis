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

}  // namespace runtime
}  // namespace anolis
