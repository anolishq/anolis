#pragma once

/**
 * @file health_line_protocol.hpp
 * @brief InfluxDB line-protocol formatting for health snapshots (#203).
 *
 * Formats `health::ProviderHealthSnapshot` into two measurements:
 *
 * - anolis_provider_health
 *   Tags: runtime_name, provider_id
 *   Fields: state, lifecycle_state, degraded, failed_device_count,
 *           device_count, uptime_seconds, supervision_enabled, attempt_count,
 *           crash_detected, circuit_open, plus allowlisted provider-reported
 *           startup counters.
 *
 * - anolis_device_health
 *   Tags: runtime_name, provider_id, device_id
 *   Fields: health, registered, staleness_ms + stale_signal_count +
 *           fault_signal_count (only once the device has been polled), plus
 *           allowlisted provider-reported metrics.
 *
 * Provider-reported metrics arrive as an untyped string->string map. Only
 * keys in the allowlists below are written, parsed strictly to their declared
 * type; unknown keys and unparseable values are skipped so schema and series
 * cardinality stay honest. The allowlists follow the reserved-key vocabulary
 * in anolis-provider-sdk docs/metrics.md plus the bread watchdog keys.
 */

#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../health/health_snapshot.hpp"
#include "influx_sink.hpp"  // escape_tag / escape_field_string

namespace anolis {
namespace telemetry {

/**
 * @brief Strict non-negative base-10 integer parse: digits only, no leftovers.
 *
 * Every allowlisted integer key is a counter or duration with contract
 * `minimum: 0`; a negative value is a provider bug and is skipped rather than
 * written as a schema-illegal row.
 */
inline bool parse_metric_int(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    try {
        out = std::stoll(s);
    } catch (...) {
        return false;
    }
    return true;
}

/** @brief Strict bool parse: exactly "true" or "false". */
inline bool parse_metric_bool(const std::string &s, bool &out) {
    if (s == "true") {
        out = true;
        return true;
    }
    if (s == "false") {
        out = false;
        return true;
    }
    return false;
}

namespace health_keys {

// Device-level integer counters (SDK metrics.md vocabulary + bread watchdog).
inline constexpr std::array<const char *, 9> device_int_keys = {
    "io_ok",
    "io_failed",
    "io_retried_attempts",
    "watchdog_timeout_ms",
    "watchdog_trip_count",
    "sample_success_count",
    "sample_failure_count",
    "call_success_count",
    "call_failure_count",
};

// Device-level booleans.
inline constexpr std::array<const char *, 4> device_bool_keys = {
    "watchdog_armed",
    "watchdog_tripped",
    "missing",
    "excluded",
};

// Provider-level integer counters (SDK startup diagnostics).
inline constexpr std::array<const char *, 3> provider_int_keys = {
    "startup_configured_devices",
    "startup_initialized_devices",
    "startup_failed_devices",
};

}  // namespace health_keys

namespace detail {

inline void append_allowlisted_metrics(std::ostringstream &line, const std::map<std::string, std::string> &metrics,
                                       const char *const *int_keys, size_t int_key_count, const char *const *bool_keys,
                                       size_t bool_key_count) {
    for (size_t i = 0; i < int_key_count; ++i) {
        auto it = metrics.find(int_keys[i]);
        if (it == metrics.end()) continue;
        int64_t value = 0;
        if (parse_metric_int(it->second, value)) {
            line << "," << int_keys[i] << "=" << value << "i";
        }
    }
    for (size_t i = 0; i < bool_key_count; ++i) {
        auto it = metrics.find(bool_keys[i]);
        if (it == metrics.end()) continue;
        bool value = false;
        if (parse_metric_bool(it->second, value)) {
            line << "," << bool_keys[i] << "=" << (value ? "true" : "false");
        }
    }
}

}  // namespace detail

/**
 * @brief Format one provider snapshot (and its devices) as line protocol.
 *
 * @param timestamp_ms single snapshot timestamp applied to every line.
 */
inline std::vector<std::string> format_health_lines(const health::ProviderHealthSnapshot &ps,
                                                    const std::string &runtime_name, int64_t timestamp_ms) {
    std::vector<std::string> lines;
    lines.reserve(1 + ps.devices.size());

    {
        std::ostringstream line;
        line << "anolis_provider_health";
        line << ",runtime_name=" << escape_tag(runtime_name);
        line << ",provider_id=" << escape_tag(ps.provider_id);

        line << " state=\"" << escape_field_string(ps.state) << "\"";
        line << ",lifecycle_state=\"" << escape_field_string(ps.lifecycle_state) << "\"";
        line << ",degraded=" << (ps.degraded ? "true" : "false");
        line << ",failed_device_count=" << ps.failed_device_count << "i";
        line << ",device_count=" << ps.device_count << "i";
        line << ",uptime_seconds=" << ps.uptime_seconds << "i";
        line << ",supervision_enabled=" << (ps.supervision.enabled ? "true" : "false");
        line << ",attempt_count=" << ps.supervision.attempt_count << "i";
        line << ",crash_detected=" << (ps.supervision.crash_detected ? "true" : "false");
        line << ",circuit_open=" << (ps.supervision.circuit_open ? "true" : "false");

        if (ps.reported) {
            detail::append_allowlisted_metrics(line, ps.reported->metrics, health_keys::provider_int_keys.data(),
                                               health_keys::provider_int_keys.size(), nullptr, 0);
        }

        line << " " << timestamp_ms;
        lines.push_back(line.str());
    }

    for (const auto &ds : ps.devices) {
        std::ostringstream line;
        line << "anolis_device_health";
        line << ",runtime_name=" << escape_tag(runtime_name);
        line << ",provider_id=" << escape_tag(ps.provider_id);
        line << ",device_id=" << escape_tag(ds.device_id);

        line << " health=\"" << escape_field_string(ds.health) << "\"";
        line << ",registered=" << (ds.registered ? "true" : "false");
        // staleness_ms + per-signal counts only once the device has actually
        // been polled: a missing/never-polled device must not read as 0 ms
        // fresh with zero degraded signals.
        if (ds.last_poll_ms > 0) {
            line << ",staleness_ms=" << ds.staleness_ms << "i";
            line << ",stale_signal_count=" << ds.stale_signal_count << "i";
            line << ",fault_signal_count=" << ds.fault_signal_count << "i";
        }

        if (ds.reported) {
            detail::append_allowlisted_metrics(
                line, ds.reported->metrics, health_keys::device_int_keys.data(), health_keys::device_int_keys.size(),
                health_keys::device_bool_keys.data(), health_keys::device_bool_keys.size());
        }

        line << " " << timestamp_ms;
        lines.push_back(line.str());
    }

    return lines;
}

}  // namespace telemetry
}  // namespace anolis
