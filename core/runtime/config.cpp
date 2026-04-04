#include "config.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_set>

#include "../logging/logger.hpp"

namespace anolis {
namespace runtime {

// Helper to parse GatingPolicy
std::optional<GatingPolicy> parse_gating_policy(const std::string &policy_str) {
    if (policy_str == "BLOCK") {
        return GatingPolicy::BLOCK;
    }
    if (policy_str == "OVERRIDE") {
        return GatingPolicy::OVERRIDE;
    }
    return std::nullopt;
}

std::string gating_policy_to_string(GatingPolicy policy) {
    switch (policy) {
        case GatingPolicy::BLOCK:
            return "BLOCK";
        case GatingPolicy::OVERRIDE:
            return "OVERRIDE";
        default:
            return "UNKNOWN";
    }
}

bool validate_config(const RuntimeConfig &config, std::string &error) {
    // Validate runtime parameters
    if (config.runtime.shutdown_timeout_ms < 500 || config.runtime.shutdown_timeout_ms > 30000) {
        error = "runtime.shutdown_timeout_ms must be between 500 and 30000 (0.5s - 30s)";
        return false;
    }
    if (config.runtime.startup_timeout_ms < 5000 || config.runtime.startup_timeout_ms > 300000) {
        error = "runtime.startup_timeout_ms must be between 5000 and 300000 (5s - 5min)";
        return false;
    }

    // Validate HTTP settings
    if (config.http.enabled) {
        if (config.http.port < 1 || config.http.port > 65535) {
            error = "HTTP port must be between 1 and 65535";
            return false;
        }
        if (config.http.thread_pool_size < 1) {
            error = "HTTP thread_pool_size must be at least 1";
            return false;
        }
        if (config.http.cors_allowed_origins.empty()) {
            error = "http.cors_allowed_origins must not be empty";
            return false;
        }
        // CORS spec violation: wildcard origin cannot be used with credentials
        if (config.http.cors_allow_credentials) {
            for (const auto &origin : config.http.cors_allowed_origins) {
                if (origin == "*") {
                    error = "CORS wildcard origin '*' cannot be used with cors_allow_credentials=true";
                    return false;
                }
            }
        }
    }

    // Validate Provider settings
    if (config.providers.empty()) {
        error = "Config must specify at least one provider";
        return false;
    }

    // Check for duplicate provider IDs
    std::unordered_set<std::string> seen_provider_ids;
    for (const auto &provider : config.providers) {
        if (provider.id.empty()) {
            error = "Provider missing 'id' field";
            return false;
        }
        if (seen_provider_ids.count(provider.id)) {
            error = "Duplicate provider ID: '" + provider.id + "'";
            return false;
        }
        seen_provider_ids.insert(provider.id);

        if (provider.command.empty()) {
            error = "Provider '" + provider.id + "' missing 'command' field";
            return false;
        }
        if (provider.timeout_ms < 100) {
            error = "Provider timeout must be >= 100ms";
            return false;
        }
        if (provider.hello_timeout_ms < 100) {
            error = "Provider hello_timeout_ms must be >= 100ms";
            return false;
        }
        if (provider.ready_timeout_ms < 1000) {
            error = "Provider ready_timeout_ms must be >= 1000ms";
            return false;
        }

        // Validate restart policy
        if (provider.restart_policy.enabled) {
            if (provider.restart_policy.max_attempts < 1) {
                error = "Provider '" + provider.id + "' restart policy max_attempts must be >= 1";
                return false;
            }

            if (provider.restart_policy.backoff_ms.empty()) {
                error = "Provider '" + provider.id + "' restart policy backoff_ms cannot be empty";
                return false;
            }

            // Validate backoff array length matches max_attempts
            if (provider.restart_policy.backoff_ms.size() !=
                static_cast<size_t>(provider.restart_policy.max_attempts)) {
                error = "Provider '" + provider.id + "' restart policy backoff_ms array length (" +
                        std::to_string(provider.restart_policy.backoff_ms.size()) + ") must match max_attempts (" +
                        std::to_string(provider.restart_policy.max_attempts) + ")";
                return false;
            }

            // Validate backoff values are positive
            for (size_t i = 0; i < provider.restart_policy.backoff_ms.size(); ++i) {
                if (provider.restart_policy.backoff_ms[i] < 0) {
                    error = "Provider '" + provider.id + "' restart policy backoff_ms[" + std::to_string(i) +
                            "] must be >= 0";
                    return false;
                }
            }

            if (provider.restart_policy.timeout_ms < 1000) {
                error = "Provider '" + provider.id + "' restart policy timeout_ms must be >= 1000ms";
                return false;
            }
            if (provider.restart_policy.success_reset_ms < 0) {
                error = "Provider '" + provider.id + "' restart policy success_reset_ms must be >= 0";
                return false;
            }
        }
    }

    // Validate Polling settings
    if (config.polling.interval_ms < 100) {
        error = "polling interval must be >= 100ms";
        return false;
    }

    // Validate Logging settings
    if (config.logging.level != "debug" && config.logging.level != "info" && config.logging.level != "warn" &&
        config.logging.level != "error") {
        error = "Invalid log level: " + config.logging.level;
        return false;
    }

    // Validate Automation settings
    if (config.automation.enabled) {
        if (config.automation.behavior_tree.empty()) {
            error = "Automation enabled but behavior_tree path not specified";
            return false;
        }
        if (config.automation.tick_rate_hz < 1 || config.automation.tick_rate_hz > 1000) {
            error = "Automation tick_rate_hz must be between 1 and 1000 Hz";
            return false;
        }

        for (const auto &param : config.automation.parameters) {
            if (param.name.empty()) {
                error = "Parameter missing name";
                return false;
            }
            if (!automation::parameter_value_matches_type(param.type, param.default_value)) {
                error = "Parameter '" + param.name + "' default value type mismatch: expected " +
                        std::string(automation::parameter_type_to_string(param.type)) + ", got " +
                        std::string(automation::parameter_value_type_name(param.default_value));
                return false;
            }

            const bool numeric_type =
                (param.type == automation::ParameterType::DOUBLE || param.type == automation::ParameterType::INT64);
            if (!numeric_type && (param.min.has_value() || param.max.has_value())) {
                error = "Parameter '" + param.name + "' min/max constraints require numeric type";
                return false;
            }
            if (param.min.has_value() && param.max.has_value() && param.min.value() > param.max.value()) {
                error = "Parameter '" + param.name + "' has invalid constraints: min > max";
                return false;
            }
            if (param.type != automation::ParameterType::STRING && !param.allowed_values.empty()) {
                error = "Parameter '" + param.name + "' allowed_values is only valid for string parameters";
                return false;
            }
        }
    }

    return true;
}

bool load_config(const std::string &config_path, RuntimeConfig &config, std::string &error) {
    try {
        YAML::Node yaml = YAML::LoadFile(config_path);

        // Check for unknown top-level keys
        const std::vector<std::string> valid_keys = {"runtime",   "http",    "providers", "polling",
                                                     "telemetry", "logging", "automation"};
        for (const auto &key_node : yaml) {
            std::string key = key_node.first.as<std::string>();
            bool known = false;
            for (const auto &valid_key : valid_keys) {
                if (key == valid_key) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                LOG_WARN("[Config] Unknown top-level key: '" << key << "' (will be ignored)");
            }
        }

        // Load runtime params
        if (yaml["runtime"]) {
            if (yaml["runtime"]["name"]) {
                config.runtime.name = yaml["runtime"]["name"].as<std::string>();
            }
            if (yaml["runtime"]["shutdown_timeout_ms"]) {
                config.runtime.shutdown_timeout_ms = yaml["runtime"]["shutdown_timeout_ms"].as<int>();
            }
            if (yaml["runtime"]["startup_timeout_ms"]) {
                config.runtime.startup_timeout_ms = yaml["runtime"]["startup_timeout_ms"].as<int>();
            }

            // Reject configs with deprecated 'mode' field
            if (yaml["runtime"]["mode"]) {
                error =
                    "runtime.mode is no longer configurable. IDLE mode is enforced at startup. "
                    "Use HTTP POST /v0/mode to transition: IDLE -> MANUAL -> AUTO";
                return false;
            }
        }

        // Load HTTP config
        if (yaml["http"]) {
            if (yaml["http"]["enabled"]) {
                config.http.enabled = yaml["http"]["enabled"].as<bool>();
            }
            if (yaml["http"]["bind"]) {
                config.http.bind = yaml["http"]["bind"].as<std::string>();
            }
            if (yaml["http"]["port"]) {
                config.http.port = yaml["http"]["port"].as<int>();
            }

            // CORS allowlist (supports scalar or sequence)
            if (yaml["http"]["cors_allowed_origins"]) {
                const auto &origins_node = yaml["http"]["cors_allowed_origins"];
                config.http.cors_allowed_origins.clear();
                if (origins_node.IsSequence()) {
                    for (const auto &origin : origins_node) {
                        config.http.cors_allowed_origins.push_back(origin.as<std::string>());
                    }
                } else if (origins_node.IsScalar()) {
                    config.http.cors_allowed_origins.push_back(origins_node.as<std::string>());
                }

                if (config.http.cors_allowed_origins.empty()) {
                    config.http.cors_allowed_origins.push_back("*");
                }
            }
            if (yaml["http"]["cors_allow_credentials"]) {
                config.http.cors_allow_credentials = yaml["http"]["cors_allow_credentials"].as<bool>();
            }

            if (yaml["http"]["thread_pool_size"]) {
                config.http.thread_pool_size = yaml["http"]["thread_pool_size"].as<int>();
            }
        }

        // Load providers
        if (yaml["providers"]) {
            config.providers.clear();  // Ensure idempotent parsing
            for (const auto &provider_node : yaml["providers"]) {
                provider::ProviderConfig provider;

                if (provider_node["id"]) {
                    provider.id = provider_node["id"].as<std::string>();
                }

                if (provider_node["command"]) {
                    provider.command = provider_node["command"].as<std::string>();
                }

                if (provider_node["args"]) {
                    for (const auto &arg : provider_node["args"]) {
                        provider.args.push_back(arg.as<std::string>());
                    }
                }

                if (provider_node["timeout_ms"]) {
                    provider.timeout_ms = provider_node["timeout_ms"].as<int>();
                }

                if (provider_node["hello_timeout_ms"]) {
                    provider.hello_timeout_ms = provider_node["hello_timeout_ms"].as<int>();
                }

                if (provider_node["ready_timeout_ms"]) {
                    provider.ready_timeout_ms = provider_node["ready_timeout_ms"].as<int>();
                }

                // Parse restart policy
                if (provider_node["restart_policy"]) {
                    const auto &rp = provider_node["restart_policy"];

                    if (rp["enabled"]) {
                        provider.restart_policy.enabled = rp["enabled"].as<bool>();
                    }

                    if (rp["max_attempts"]) {
                        provider.restart_policy.max_attempts = rp["max_attempts"].as<int>();
                    }

                    if (rp["backoff_ms"]) {
                        provider.restart_policy.backoff_ms.clear();
                        for (const auto &backoff : rp["backoff_ms"]) {
                            provider.restart_policy.backoff_ms.push_back(backoff.as<int>());
                        }
                    }

                    if (rp["timeout_ms"]) {
                        provider.restart_policy.timeout_ms = rp["timeout_ms"].as<int>();
                    }
                    if (rp["success_reset_ms"]) {
                        provider.restart_policy.success_reset_ms = rp["success_reset_ms"].as<int>();
                    }
                }

                config.providers.push_back(provider);
            }
        }

        // Load polling config
        if (yaml["polling"]) {
            if (yaml["polling"]["interval_ms"]) {
                config.polling.interval_ms = yaml["polling"]["interval_ms"].as<int>();
            }
        }

        // Load telemetry config
        if (yaml["telemetry"]) {
            if (yaml["telemetry"]["enabled"]) {
                config.telemetry.enabled = yaml["telemetry"]["enabled"].as<bool>();
            }

            // Check for deprecated flat keys
            const std::vector<std::string> deprecated_flat_keys = {"influx_url",   "influx_org", "influx_bucket",
                                                                   "influx_token", "batch_size", "flush_interval_ms"};
            for (const auto &deprecated_key : deprecated_flat_keys) {
                if (yaml["telemetry"][deprecated_key]) {
                    // Strip "influx_" prefix if present for canonical key name
                    std::string canonical_key = deprecated_key;
                    if (canonical_key.find("influx_") == 0) {
                        canonical_key = canonical_key.substr(7);  // Remove "influx_" prefix
                    }
                    LOG_WARN("[Config] Deprecated telemetry key 'telemetry."
                             << deprecated_key << "' - use 'telemetry.influxdb." << canonical_key << "' instead");
                }
            }

            // InfluxDB settings (canonical nested structure)
            if (yaml["telemetry"]["influxdb"]) {
                auto influx = yaml["telemetry"]["influxdb"];

                if (influx["url"]) {
                    config.telemetry.influx_url = influx["url"].as<std::string>();
                }
                if (influx["org"]) {
                    config.telemetry.influx_org = influx["org"].as<std::string>();
                }
                if (influx["bucket"]) {
                    config.telemetry.influx_bucket = influx["bucket"].as<std::string>();
                }
                if (influx["token"]) {
                    config.telemetry.influx_token = influx["token"].as<std::string>();
                }
                if (influx["batch_size"]) {
                    config.telemetry.batch_size = influx["batch_size"].as<size_t>();
                }
                if (influx["flush_interval_ms"]) {
                    config.telemetry.flush_interval_ms = influx["flush_interval_ms"].as<int>();
                }
                if (influx["max_retry_buffer_size"]) {
                    config.telemetry.max_retry_buffer_size = influx["max_retry_buffer_size"].as<size_t>();
                }
                if (influx["queue_size"]) {
                    config.telemetry.queue_size = influx["queue_size"].as<size_t>();
                }
            }

            // Check for token from environment variable if not in config
            if (config.telemetry.enabled && config.telemetry.influx_token.empty()) {
#ifdef _WIN32
                char *token_env = nullptr;
                size_t token_len = 0;
                if (_dupenv_s(&token_env, &token_len, "INFLUXDB_TOKEN") == 0 && token_env != nullptr) {
                    config.telemetry.influx_token = token_env;
                    std::free(token_env);
                }
#else
                const char *token_env = std::getenv("INFLUXDB_TOKEN");
                if (token_env != nullptr) {
                    config.telemetry.influx_token = token_env;
                }
#endif
            }
        }

        // Load logging config
        if (yaml["logging"]) {
            if (yaml["logging"]["level"]) {
                config.logging.level = yaml["logging"]["level"].as<std::string>();
            }
        }

        // Load automation config
        if (yaml["automation"]) {
            if (yaml["automation"]["enabled"]) {
                config.automation.enabled = yaml["automation"]["enabled"].as<bool>();
            }
            if (yaml["automation"]["behavior_tree"]) {
                config.automation.behavior_tree = yaml["automation"]["behavior_tree"].as<std::string>();
            } else if (yaml["automation"]["behavior_tree_path"]) {
                LOG_WARN("[Config] Deprecated automation key 'automation.behavior_tree_path' - use "
                         "'automation.behavior_tree' instead");
                config.automation.behavior_tree = yaml["automation"]["behavior_tree_path"].as<std::string>();
            }
            if (yaml["automation"]["tick_rate_hz"]) {
                config.automation.tick_rate_hz = yaml["automation"]["tick_rate_hz"].as<int>();
            }
            if (yaml["automation"]["manual_gating_policy"]) {
                auto policy_str = yaml["automation"]["manual_gating_policy"].as<std::string>();
                auto policy = parse_gating_policy(policy_str);
                if (!policy) {
                    error = "Invalid manual_gating_policy: must be BLOCK or OVERRIDE";
                    return false;
                }
                config.automation.manual_gating_policy = *policy;
            }

            // Load parameters
            if (yaml["automation"]["parameters"]) {
                config.automation.parameters.clear();  // Ensure idempotent parsing
                for (const auto &param_node : yaml["automation"]["parameters"]) {
                    ParameterConfig param;

                    if (param_node["name"]) {
                        param.name = param_node["name"].as<std::string>();
                    }

                    if (!param_node["type"]) {
                        error = "Parameter '" + param.name + "' missing type";
                        return false;
                    }
                    {
                        const std::string type_str = param_node["type"].as<std::string>();
                        const auto parsed_type = automation::parameter_type_from_string(type_str);
                        if (!parsed_type.has_value()) {
                            error = "Parameter '" + param.name + "' has invalid type: " + type_str;
                            return false;
                        }
                        param.type = parsed_type.value();
                    }

                    // Initialize typed default before overriding from YAML.
                    switch (param.type) {
                        case automation::ParameterType::DOUBLE:
                            param.default_value = 0.0;
                            break;
                        case automation::ParameterType::INT64:
                            param.default_value = int64_t{0};
                            break;
                        case automation::ParameterType::BOOL:
                            param.default_value = false;
                            break;
                        case automation::ParameterType::STRING:
                            param.default_value = std::string{};
                            break;
                    }

                    // Parse default value based on typed parameter definition.
                    if (param_node["default"]) {
                        try {
                            switch (param.type) {
                                case automation::ParameterType::DOUBLE:
                                    param.default_value = param_node["default"].as<double>();
                                    break;
                                case automation::ParameterType::INT64:
                                    param.default_value = param_node["default"].as<int64_t>();
                                    break;
                                case automation::ParameterType::BOOL:
                                    param.default_value = param_node["default"].as<bool>();
                                    break;
                                case automation::ParameterType::STRING:
                                    param.default_value = param_node["default"].as<std::string>();
                                    break;
                            }
                        } catch (const YAML::Exception &e) {
                            error =
                                "Parameter '" + param.name + "' default value parse failed: " + std::string(e.what());
                            return false;
                        }
                    }

                    // Parse constraints (numeric types)
                    if (param_node["min"]) {
                        param.min = param_node["min"].as<double>();
                    }
                    if (param_node["max"]) {
                        param.max = param_node["max"].as<double>();
                    }

                    // Parse allowed_values (string enums)
                    if (param_node["allowed_values"]) {
                        for (const auto &val : param_node["allowed_values"]) {
                            param.allowed_values.push_back(val.as<std::string>());
                        }
                    }

                    config.automation.parameters.push_back(param);
                }
            }
        }

        // Perform validation (this replaces centralized logic in the loop)
        if (!validate_config(config, error)) {
            return false;
        }

        LOG_INFO("[Config] Loaded " << config.providers.size() << " provider(s)");

        if (!config.runtime.name.empty()) {
            LOG_INFO("[Config] Runtime name: " << config.runtime.name);
        }

        std::stringstream http_msg;
        http_msg << "[Config] HTTP: " << (config.http.enabled ? "enabled" : "disabled");
        if (config.http.enabled) {
            http_msg << " (" << config.http.bind << ":" << config.http.port << ")";
        }
        LOG_INFO(http_msg.str());

        LOG_INFO("[Config] Polling interval: " << config.polling.interval_ms << "ms");

        std::stringstream telemetry_msg;
        telemetry_msg << "[Config] Telemetry: " << (config.telemetry.enabled ? "enabled" : "disabled");
        if (config.telemetry.enabled) {
            telemetry_msg << " (" << config.telemetry.influx_url << "/" << config.telemetry.influx_bucket << ")";
        }
        LOG_INFO(telemetry_msg.str());

        LOG_INFO("[Config] Log level: " << config.logging.level);

        std::stringstream automation_msg;
        automation_msg << "[Config] Automation: " << (config.automation.enabled ? "enabled" : "disabled");
        if (config.automation.enabled) {
            automation_msg << " (BT: " << config.automation.behavior_tree
                           << ", tick rate: " << config.automation.tick_rate_hz << " Hz, "
                           << config.automation.parameters.size() << " parameters)";
        }
        LOG_INFO(automation_msg.str());

        return true;
    } catch (const YAML::BadFile &) {
        error = "Cannot open config file: " + config_path;
        return false;
    } catch (const YAML::ParserException &e) {
        error = "YAML parse error: " + std::string(e.what());
        return false;
    } catch (const std::exception &e) {
        error = "Config load error: " + std::string(e.what());
        return false;
    }
}

}  // namespace runtime
}  // namespace anolis
