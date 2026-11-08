#pragma once

/**
 * @file provider_config.hpp
 * @brief Provider process and restart-policy configuration types.
 */

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anolis {
namespace provider {

/**
 * @brief Automatic restart policy for a supervised provider.
 *
 * `backoff_ms` is indexed by restart attempt number and is expected to have the
 * same length as `max_attempts`.
 *
 * `success_reset_ms` defines how long the replacement process must remain
 * healthy before the supervisor clears the crash streak and closes the current
 * recovery window.
 *
 * `timeout_ms` bounds one whole restart attempt, from spawning the replacement
 * to its rediscovered inventory; `Hello` and `WaitReady` share it (#556).
 * Unset, the attempt gets `restart_timeout_ms()` of its provider.
 */
struct RestartPolicyConfig {
    bool enabled = false;                          // Enable automatic restart on crash
    int max_attempts = 3;                          // Max restart attempts before giving up
    std::vector<int> backoff_ms{100, 1000, 5000};  // Exponential backoff schedule (ms)
    std::optional<int> timeout_ms;                 // Budget for one restart attempt; unset: hello + ready
    int success_reset_ms = 1000;                   // Healthy duration required before resetting crash attempts
};

/**
 * @brief Runtime configuration for launching and supervising one provider process.
 *
 * The runtime uses this config both for initial startup and for later restart
 * attempts, so the command, args, timeouts, and restart policy together define
 * the provider's full operational contract.
 */
struct ProviderConfig {
    std::string id;                      // e.g., "sim0"
    std::string command;                 // Path to provider executable
    std::vector<std::string> args;       // Command-line arguments
    int timeout_ms = 5000;               // ADPP operation timeout (default 5s)
    int hello_timeout_ms = 5000;         // Process liveness check timeout (default 5s)
    int ready_timeout_ms = 60000;        // Hardware initialization timeout (default 60s)
    RestartPolicyConfig restart_policy;  // Automatic restart configuration
};

/**
 * @brief The budget for one restart attempt of this provider (#556).
 *
 * `restart_policy.timeout_ms` when set. Otherwise `hello_timeout_ms +
 * ready_timeout_ms`, the longest `Hello` and `WaitReady` can take at boot.
 * Spawning and discovery spend the same budget, so a provider that needs nearly
 * all of both timeouts needs an explicit, larger value. Saturates at INT_MAX.
 */
inline int restart_timeout_ms(const ProviderConfig &config) {
    if (config.restart_policy.timeout_ms) {
        return *config.restart_policy.timeout_ms;
    }
    const std::int64_t startup_ms = std::int64_t{config.hello_timeout_ms} + config.ready_timeout_ms;
    return static_cast<int>(std::min<std::int64_t>(startup_ms, INT_MAX));
}

}  // namespace provider
}  // namespace anolis
