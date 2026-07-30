#pragma once

/**
 * @file health_snapshot.hpp
 * @brief Shared provider/device health snapshot collection (#203).
 *
 * Single source of truth for the health view served by
 * `GET /v0/providers/health` and ingested into InfluxDB as the
 * `anolis_provider_health` / `anolis_device_health` measurements. The HTTP
 * handler serializes these structs to JSON; the telemetry layer formats them
 * as line protocol. Both surfaces must derive from this collector so they
 * cannot drift.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anolis {

namespace provider {
class ProviderRegistry;
class ProviderSupervisor;
}  // namespace provider

namespace registry {
class DeviceRegistry;
}

namespace state {
class StateCache;
}

namespace health {

/**
 * @brief Cadence-derived device-liveness staleness policy (#220, D7 phase 1).
 *
 * The device `health` liveness tiers were historically a hardcoded 2s/5s wall
 * clock on time-since-last-successful-poll. A healthy but *serialized* bus
 * (e.g. EZO pH ~900ms + DO ~600-900ms => ~1.5-1.8s/cycle at N=2) structurally
 * exceeds a 2s wall clock and flapped STALE (observed live). The bound is now
 * derived from the poll cadence and the provider's device count so it scales
 * with the real cycle cost, with absolute overrides for odd buses.
 */
struct StalenessPolicy {
    int poll_interval_ms = 500;  // background poll period (runtime `polling.interval_ms`)
    int warn_after_ms = 0;       // explicit override; 0 => derive from cadence
    int stale_after_ms = 0;      // explicit override; 0 => derive from cadence
};

/** @brief Resolved WARNING/STALE liveness thresholds for one provider. */
struct StalenessBounds {
    int64_t warn_ms;
    int64_t stale_ms;
};

// Derivation constants (see resolve_staleness_bounds). Chosen so that the
// default policy at one device @ 500ms reproduces the historical 2000/5000
// ladder exactly (zero regression for fast buses); bounds only loosen as the
// serialized cycle grows.
inline constexpr int kStalenessWarnCycles = 3;
inline constexpr int kStalenessStaleCycles = 8;
inline constexpr int64_t kStalenessWarnFloorMs = 2000;
inline constexpr int64_t kStalenessStaleFloorMs = 5000;

/**
 * @brief Resolve the WARNING/STALE liveness thresholds for a provider.
 *
 * cycle_budget = poll_interval_ms * max(1, device_count)  (serialized worst case)
 * warn  = override>0 ? override : max(kStalenessWarnCycles  * cycle_budget, kStalenessWarnFloorMs)
 * stale = override>0 ? override : max(kStalenessStaleCycles * cycle_budget, kStalenessStaleFloorMs)
 * warn  = min(warn, stale)  (ordering clamp)
 */
StalenessBounds resolve_staleness_bounds(const StalenessPolicy &policy, size_t device_count);

/**
 * @brief Effective per-signal age bound in ms (#220, D7 phase 2).
 *
 * `max(declared_ms, liveness_stale_ms)`. The per-signal *age* freshness check
 * must never fire before the cadence-derived liveness STALE bound, or it would
 * re-introduce the false-STALE storm phase 1 fixed (a serialized bus whose
 * signals are legitimately ~one cycle old). So the liveness stale bound is the
 * floor; a provider's declared `stale_after_ms` only *extends* trust beyond it.
 * The age path therefore adds STALE only for a genuinely stuck/old cached
 * sample (signal age >> poll age); a proto3-unset (0) declared value simply
 * inherits the liveness bound (never instant-stale). Degraded *quality*
 * (STALE/UNKNOWN/FAULT) is provider-authoritative and handled separately.
 */
int64_t effective_signal_stale_after_ms(uint32_t declared_ms, int64_t liveness_stale_ms);

/** @brief Provider-reported ADPP DeviceHealth entry (#185). */
struct ReportedDeviceHealth {
    std::string state;  // adpp DeviceHealth::State name, e.g. "STATE_OK"
    std::string message;
    std::map<std::string, std::string> metrics;
    std::optional<int64_t> last_seen_epoch_ms;
};

struct DeviceHealthSnapshot {
    std::string device_id;
    std::string health = "UNKNOWN";  // OK | WARNING | STALE | FAULT | UNAVAILABLE | UNKNOWN
    uint64_t last_poll_ms = 0;
    uint64_t staleness_ms = 0;
    bool registered = true;
    // Per-signal freshness/quality rollup (#220 phase 2): how many cached
    // signals are FAULT-quality vs freshness-stale (degraded quality or older
    // than their effective stale_after_ms). Populated whenever a DeviceState
    // exists; they explain the FAULT/STALE health string.
    uint32_t stale_signal_count = 0;
    uint32_t fault_signal_count = 0;
    std::optional<ReportedDeviceHealth> reported;
};

/** @brief Provider-reported ADPP ProviderHealth block (#185). */
struct ReportedProviderHealth {
    std::string state;  // adpp ProviderHealth::State name
    std::string message;
    std::map<std::string, std::string> metrics;
};

struct SupervisionView {
    bool enabled = false;
    int attempt_count = 0;
    int max_attempts = 0;
    bool crash_detected = false;
    bool circuit_open = false;
    std::optional<int64_t> next_restart_in_ms;
};

struct ProviderHealthSnapshot {
    std::string provider_id;
    std::string state;  // AVAILABLE | UNAVAILABLE
    std::string lifecycle_state;
    std::optional<int64_t> last_seen_ago_ms;
    int64_t uptime_seconds = 0;
    size_t device_count = 0;
    bool degraded = false;
    int failed_device_count = 0;
    std::optional<ReportedProviderHealth> reported;
    SupervisionView supervision;
    std::vector<DeviceHealthSnapshot> devices;
};

/**
 * @brief Collect the full health view across all providers.
 *
 * Performs a live ADPP GetHealth round-trip per available provider (bread
 * answers with a live GET_WATCHDOG bus query per armed device), so callers
 * choose the cadence deliberately.
 *
 * @param supervisor may be nullptr when supervision is disabled.
 * @param staleness_policy cadence-derived liveness bounds (#220).
 * @param now current time; defaulted, overridable for deterministic tests.
 */
std::vector<ProviderHealthSnapshot> collect_providers_health(
    provider::ProviderRegistry &provider_registry, registry::DeviceRegistry &device_registry,
    state::StateCache &state_cache, provider::ProviderSupervisor *supervisor, const StalenessPolicy &staleness_policy,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

}  // namespace health
}  // namespace anolis
