#pragma once

/**
 * @file provider_supervisor.hpp
 * @brief Provider crash tracking, restart timing, and circuit-breaker state.
 */

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "provider_config.hpp"

namespace anolis {
namespace provider {

/**
 * @brief Thread-safe supervisor for provider restart policy state.
 *
 * ProviderSupervisor does not spawn or restart processes itself. It tracks
 * crash streaks, restart backoff timing, health heartbeats, and circuit-breaker
 * state so the runtime loop can decide when a restart attempt is allowed.
 *
 * Threading:
 * All methods lock internally and may be called from any runtime thread.
 *
 * Invariants:
 * Crash attempts are only reset after a restarted provider remains healthy for
 * the configured `success_reset_ms` window.
 */
class ProviderSupervisor {
public:
    /**
     * @brief Immutable snapshot of supervision state for one provider.
     *
     * Returned by value so HTTP handlers and other readers can inspect
     * supervision state without holding the supervisor lock.
     */
    struct ProviderSupervisionSnapshot {
        bool supervision_enabled = false;
        int attempt_count = 0;
        int max_attempts = 0;
        bool crash_detected = false;
        bool circuit_open = false;
        std::optional<int64_t> next_restart_in_ms;  // nullopt: healthy or circuit-open (see circuit_open)
        std::optional<int64_t> last_seen_ago_ms;    // nullopt if no healthy heartbeat yet
        int64_t uptime_seconds = 0;                 // 0 before first heartbeat or when unavailable
    };

    ProviderSupervisor() = default;

    /**
     * @brief Register a provider and initialize its supervision state.
     *
     * Re-registering replaces the prior policy and resets tracked state.
     */
    void register_provider(const std::string &provider_id, const RestartPolicyConfig &policy);

    /**
     * @brief Report whether a restart attempt is currently allowed.
     *
     * Returns false when supervision is disabled, the provider is unknown, the
     * circuit is open, or the current backoff window has not yet elapsed.
     */
    bool should_restart(const std::string &provider_id) const;

    /**
     * @brief Get the backoff delay associated with the current restart attempt.
     *
     * @return Delay in milliseconds, or 0 if no restart window is active
     */
    int get_backoff_ms(const std::string &provider_id) const;

    /**
     * @brief Record a newly detected provider crash.
     *
     * Increments the attempt count and schedules the next restart time. Returns
     * false when supervision is disabled or the crash exceeds the configured
     * max-attempt threshold, which opens the circuit.
     */
    bool record_crash(const std::string &provider_id);

    /**
     * @brief Mark a previously unhealthy provider as recovered.
     *
     * Clears the crash streak and closes the circuit while preserving the
     * current process start time so uptime still reflects the active instance.
     */
    void record_success(const std::string &provider_id);

    /**
     * @brief Report whether the provider has been healthy long enough to recover.
     */
    bool should_mark_recovered(const std::string &provider_id) const;

    /** @brief Report whether the circuit breaker is currently open. */
    bool is_circuit_open(const std::string &provider_id) const;

    /** @brief Get the current crash-attempt count for one provider. */
    int get_attempt_count(const std::string &provider_id) const;

    /**
     * @brief Mark that the current outage has already been noticed.
     *
     * @return true only for the first observation of a new crash window
     */
    bool mark_crash_detected(const std::string &provider_id);

    /**
     * @brief Clear the "current crash observed" flag before a restart attempt.
     */
    void clear_crash_detected(const std::string &provider_id);

    /**
     * @brief Record a healthy heartbeat from the runtime loop.
     *
     * The first heartbeat after startup or crash establishes the current
     * process start time used for uptime and recovery-window calculations.
     */
    void record_heartbeat(const std::string &provider_id);

    /**
     * @brief Get an immutable supervision snapshot for one provider.
     *
     * @return Snapshot, or `std::nullopt` if the provider is unknown
     */
    std::optional<ProviderSupervisionSnapshot> get_snapshot(const std::string &provider_id) const;

    /** @brief Get immutable supervision snapshots for all registered providers. */
    std::unordered_map<std::string, ProviderSupervisionSnapshot> get_all_snapshots() const;

private:
    struct RestartState {
        int attempt_count = 0;                                    // Current restart attempt number
        bool circuit_open = false;                                // True when max attempts exceeded
        bool crash_detected = false;                              // True if we're currently handling a crash
        std::chrono::steady_clock::time_point next_restart_time;  // Earliest time for next restart
        // Set on first record_heartbeat after start/restart; reset to {} by record_crash.
        std::chrono::steady_clock::time_point process_start_time;
        // Updated by record_heartbeat every healthy loop iteration.
        std::chrono::steady_clock::time_point last_healthy_time;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RestartPolicyConfig> policies_;
    std::unordered_map<std::string, RestartState> states_;
};

}  // namespace provider
}  // namespace anolis
