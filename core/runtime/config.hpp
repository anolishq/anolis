#pragma once

/**
 * @file config.hpp
 * @brief Runtime configuration types and YAML load/validation entry points.
 */

#include <map>
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
 * @brief Device-health liveness staleness thresholds (#220).
 *
 * Both default to 0 = "derive from the poll cadence and device count"
 * (health::resolve_staleness_bounds). A positive value is an absolute ms
 * override for buses the cadence heuristic does not fit.
 */
struct HealthStalenessConfig {
    int warn_after_ms = 0;   // 0 => derive
    int stale_after_ms = 0;  // 0 => derive
};

/** @brief Device/provider health surface policy. */
struct HealthConfig {
    HealthStalenessConfig staleness;
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
    std::string data_dir;            // Runtime data directory (run registry JSONL, etc.); empty => ./anolis-data
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

    // Authentication (Bearer token). Non-loopback exposure requires auth (see the
    // startup guard in validate_config); loopback stays friction-free by default.
    bool auth_enabled = false;         // Require a Bearer token on incoming requests
    std::string auth_token;            // Shared secret; falls back to ANOLIS_API_TOKEN env
    bool auth_exempt_loopback = true;  // Skip auth for requests from 127.0.0.1 / ::1
    bool allow_insecure_bind = false;  // Override the non-loopback-without-auth startup guard

    // TLS (HTTPS). Set both paths to serve over TLS (httplib::SSLServer); leave
    // empty for plain HTTP. Strongly recommended whenever auth is used on a
    // non-loopback bind (a Bearer token over plaintext is sniffable).
    std::string tls_cert_path;  // PEM certificate (chain) path
    std::string tls_key_path;   // PEM private key path
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

    // Health snapshot ingestion cadence (#203). Each snapshot is a live ADPP
    // GetHealth round-trip per provider (bread: a GET_WATCHDOG bus query per
    // armed device). 0 disables ingestion.
    int health_interval_ms = 15000;
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
 * @brief Generic device call payload for a mode-transition hook.
 */
struct ModeTransitionCallConfig {
    std::string device_handle;                               // provider_id/device_id
    uint32_t function_id = 0;                                // optional numeric selector
    std::string function_name;                               // optional name selector
    std::map<std::string, automation::ParameterValue> args;  // scalar args only
};

/**
 * @brief Hook definition for mode transition lifecycle.
 *
 * `from` or `to` may be empty (wildcard). Matching is runtime-string based
 * against MANUAL/AUTO/IDLE/FAULT.
 */
struct ModeTransitionHookConfig {
    std::string from;  // optional mode filter, empty or "*" means any
    std::string to;    // optional mode filter, empty or "*" means any
    bool fail_on_error = true;
    std::vector<ModeTransitionCallConfig> calls;
};

/**
 * @brief Grouped mode transition hooks by lifecycle timing.
 */
struct ModeTransitionHooksConfig {
    std::vector<ModeTransitionHookConfig> before_transition;
    std::vector<ModeTransitionHookConfig> after_transition;
};

/**
 * @brief Declared software safe-state actions for `POST /v0/estop`.
 *
 * Parsed independently of the `automation` section so it is available on a
 * pure-manual machine. The estop ladder consumes these in order: run `hooks`;
 * else drive `setpoints` (only if they cover every actuating output); else zero
 * all actuating outputs, but only if `zero_is_safe` is explicitly asserted.
 */
struct SafeStateConfig {
    std::vector<ModeTransitionCallConfig> hooks;
    std::vector<ModeTransitionCallConfig> setpoints;
    bool zero_is_safe = false;
};

/**
 * @brief Settings from the top-level `safety` YAML section.
 */
struct SafetyConfig {
    SafeStateConfig safe_state;
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
    ModeTransitionHooksConfig mode_transition_hooks;          // Generic mode transition hooks
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
    SafetyConfig safety;
    HealthConfig health;
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
