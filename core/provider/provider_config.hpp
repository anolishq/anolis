#pragma once

/**
 * @file provider_config.hpp
 * @brief Provider process and restart-policy configuration types.
 */

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
 */
struct RestartPolicyConfig {
    bool enabled = false;                          // Enable automatic restart on crash
    int max_attempts = 3;                          // Max restart attempts before giving up
    std::vector<int> backoff_ms{100, 1000, 5000};  // Exponential backoff schedule (ms)
    int timeout_ms = 30000;                        // Timeout for restart attempt (30s default)
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

}  // namespace provider
}  // namespace anolis
