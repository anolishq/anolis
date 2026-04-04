#pragma once

/**
 * @file config.hpp
 * @brief Runtime configuration types and YAML load/validation entry points.
 */

#include <optional>
#include <string>
#include <vector>

#include "../automation/parameter_types.hpp"
#include "../provider/provider_config.hpp"

namespace anolis {
namespace runtime {

/**
 * @brief Policy for manual calls while automation owns the runtime.
 */
enum class GatingPolicy { BLOCK, OVERRIDE };

/** @brief Polling scheduler settings for background state refresh. */
struct PollingConfig {
    int interval_ms = 500;  // Default 500ms
};

/** @brief Runtime logging settings. */
struct LoggingConfig {
    std::string level = "info";  // debug, info, warn, error
};

/**
 * @brief Core runtime lifecycle settings from the `runtime` YAML section.
 *
 * The current runtime mode is intentionally not configurable here. Startup
 * always begins in IDLE and mode transitions happen through the control plane.
 */
struct RuntimeModeConfig {
    std::string name;                // Instance identifier (optional, for multi-runtime deployments)
    int shutdown_timeout_ms = 2000;  // Provider graceful shutdown timeout (500-30000ms)
    int startup_timeout_ms = 30000;  // Overall startup timeout for fail-fast (5000-300000ms)
};

/**
 * @brief HTTP adapter settings for the runtime's REST surface.
 */
struct HttpConfig {
    bool enabled = true;                                 // HTTP server enabled
    std::string bind = "127.0.0.1";                      // Bind address
    int port = 8080;                                     // HTTP port
    std::vector<std::string> cors_allowed_origins{"*"};  // CORS allowlist ("*" = allow all)
    bool cors_allow_credentials = false;                 // Whether to emit Access-Control-Allow-Credentials
    int thread_pool_size = 40;                           // Worker thread pool size
};

/**
 * @brief Telemetry sink settings after YAML parsing.
 *
 * The canonical YAML shape uses nested `telemetry.influxdb.*` keys. The runtime
 * stores the resolved values in this flat struct for simpler downstream use.
 */
struct TelemetryConfig {
    bool enabled = false;  // Enable telemetry sink

    // InfluxDB settings
    std::string influx_url = "http://localhost:8086";  // InfluxDB URL
    std::string influx_org = "anolis";                 // InfluxDB organization
    std::string influx_bucket = "anolis";              // InfluxDB bucket
    std::string influx_token;                          // InfluxDB API token (from env)

    // Batching configuration
    size_t batch_size = 100;       // Flush when batch reaches this size
    int flush_interval_ms = 1000;  // Flush every N milliseconds

    // Queue settings
    size_t queue_size = 10000;            // Event queue size
    size_t max_retry_buffer_size = 1000;  // Max events to buffer on write failure
};

/**
 * @brief Runtime-defined automation parameter with optional validation rules.
 */
struct ParameterConfig {
    std::string name;
    automation::ParameterType type = automation::ParameterType::DOUBLE;
    automation::ParameterValue default_value = 0.0;

    // Constraints
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> allowed_values;  // For string enums
};

/**
 * @brief Automation subsystem settings from the `automation` YAML section.
 *
 * `behavior_tree` is the canonical config key. The loader may accept a small
 * set of deprecated aliases for backward compatibility, but the in-memory
 * representation uses this normalized form.
 */
struct AutomationConfig {
    bool enabled = false;
    std::string behavior_tree;                                // Path to BT XML file
    int tick_rate_hz = 10;                                    // BT tick rate (1-1000 Hz)
    GatingPolicy manual_gating_policy = GatingPolicy::BLOCK;  // BLOCK or OVERRIDE
    std::vector<ParameterConfig> parameters;                  // Runtime parameters
};

/**
 * @brief Fully resolved runtime configuration with defaults applied.
 */
struct RuntimeConfig {
    RuntimeModeConfig runtime;  // Runtime section (IDLE mode hardcoded, not configurable)
    HttpConfig http;
    std::vector<provider::ProviderConfig> providers;
    PollingConfig polling;
    TelemetryConfig telemetry;
    LoggingConfig logging;
    AutomationConfig automation;
};

/**
 * @brief Load, normalize, and validate runtime configuration from YAML.
 *
 * Starts from `RuntimeConfig` defaults on every call so omitted sections do not
 * retain stale values from a previous load. The loader also accepts a limited
 * set of deprecated keys and malformed-shape diagnostics are reported through
 * `error`.
 *
 * @param config_path Path to the YAML file to load
 * @param config Output configuration populated on success
 * @param error Output error string on parse or validation failure
 * @return true if the file was parsed and the resulting configuration is valid
 */
bool load_config(const std::string &config_path, RuntimeConfig &config, std::string &error);

/**
 * @brief Validate an already-populated runtime configuration.
 *
 * This checks value ranges, required sections, restart-policy consistency, and
 * cross-field constraints such as automation requiring a behavior tree path.
 *
 * @param config Configuration instance to validate
 * @param error Output error string on failure
 * @return true if the configuration is valid
 */
bool validate_config(const RuntimeConfig &config, std::string &error);

}  // namespace runtime
}  // namespace anolis
