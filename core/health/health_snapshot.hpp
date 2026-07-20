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

/** @brief Provider-reported ADPP DeviceHealth entry (#185). */
struct ReportedDeviceHealth {
    std::string state;  // adpp DeviceHealth::State name, e.g. "STATE_OK"
    std::string message;
    std::map<std::string, std::string> metrics;
    std::optional<int64_t> last_seen_epoch_ms;
};

struct DeviceHealthSnapshot {
    std::string device_id;
    std::string health = "UNKNOWN";  // OK | WARNING | STALE | UNAVAILABLE | UNKNOWN
    uint64_t last_poll_ms = 0;
    uint64_t staleness_ms = 0;
    bool registered = true;
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
 */
std::vector<ProviderHealthSnapshot> collect_providers_health(provider::ProviderRegistry &provider_registry,
                                                             registry::DeviceRegistry &device_registry,
                                                             state::StateCache &state_cache,
                                                             provider::ProviderSupervisor *supervisor);

}  // namespace health
}  // namespace anolis
