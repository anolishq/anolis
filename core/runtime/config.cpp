/**
 * @file config.cpp
 * @brief YAML loading and validation for the runtime configuration model.
 *
 * The loader is intentionally tolerant of unknown keys and some deprecated
 * aliases, but it always rebuilds `RuntimeConfig` from defaults on each load so
 * omitted sections never inherit stale state from a previous file.
 */

#include "config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <regex>
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

namespace {

bool ensure_mapping(const YAML::Node &node, const std::string &path, std::string &error) {
    if (!node || node.IsNull()) {
        return true;
    }
    if (!node.IsMap()) {
        error = "'" + path + "' must be a map";
        return false;
    }
    return true;
}

bool ensure_sequence(const YAML::Node &node, const std::string &path, std::string &error) {
    if (!node || node.IsNull()) {
        return true;
    }
    if (!node.IsSequence()) {
        error = "'" + path + "' must be a sequence";
        return false;
    }
    return true;
}

void warn_unknown_keys(const YAML::Node &node, const std::string &path,
                       std::initializer_list<const char *> valid_keys) {
    if (!node || !node.IsMap()) {
        return;
    }

    for (const auto &entry : node) {
        const std::string key = entry.first.as<std::string>();
        const bool known = std::find_if(valid_keys.begin(), valid_keys.end(),
                                        [&key](const char *candidate) { return key == candidate; }) != valid_keys.end();
        if (!known) {
            LOG_WARN("[Config] Unknown key '" << path << "." << key << "' (will be ignored)");
        }
    }
}

bool is_valid_mode_name(const std::string &mode_name) {
    return mode_name == "MANUAL" || mode_name == "AUTO" || mode_name == "IDLE" || mode_name == "FAULT" ||
           mode_name == "*";
}

bool parse_scalar_parameter_value(const YAML::Node &node, automation::ParameterValue &out, std::string &error,
                                  const std::string &path) {
    if (!node || node.IsNull()) {
        error = "'" + path + "' must be a scalar value";
        return false;
    }
    if (!node.IsScalar()) {
        error = "'" + path + "' must be a scalar value";
        return false;
    }

    const std::string raw = node.as<std::string>();

    // bool literal
    if (raw == "true" || raw == "false") {
        out = (raw == "true");
        return true;
    }

    // integer literal
    static const std::regex kIntPattern(R"(^[+-]?[0-9]+$)");
    if (std::regex_match(raw, kIntPattern)) {
        try {
            out = std::stoll(raw);
            return true;
        } catch (const std::exception &) {
            error = "'" + path + "' integer value out of range";
            return false;
        }
    }

    // floating literal
    static const std::regex kFloatPattern(R"(^[+-]?([0-9]+\.[0-9]*|[0-9]*\.[0-9]+)([eE][+-]?[0-9]+)?$)");
    if (std::regex_match(raw, kFloatPattern)) {
        try {
            out = std::stod(raw);
            return true;
        } catch (const std::exception &) {
            error = "'" + path + "' floating value out of range";
            return false;
        }
    }

    // Fallback: treat as string
    out = raw;
    return true;
}

// True if a bind address only accepts connections from the local host (so it is
// not exposed to the network). "0.0.0.0" / "::" (all interfaces) are NOT loopback.
bool is_loopback_bind(const std::string &bind) {
    return bind == "127.0.0.1" || bind == "::1" || bind == "localhost" || bind.rfind("127.", 0) == 0;
}

// Parse a YAML sequence of device-call payloads, shared by mode-transition hooks
// and the safety safe-state ladder. `base_path` names the sequence for
// diagnostics; each element is reported as "<base_path>[i]".
bool parse_call_list(const YAML::Node &calls_node, const std::string &base_path,
                     std::vector<ModeTransitionCallConfig> &out, std::string &error) {
    out.clear();
    if (!ensure_sequence(calls_node, base_path, error)) {
        return false;
    }

    size_t call_index = 0;
    for (const auto &call_node : calls_node) {
        const std::string call_path = base_path + "[" + std::to_string(call_index) + "]";
        if (!call_node.IsMap()) {
            error = std::format("'{}' must be a map", call_path);
            return false;
        }
        warn_unknown_keys(call_node, call_path, {"device_handle", "function_id", "function_name", "args"});

        ModeTransitionCallConfig call;
        if (call_node["device_handle"]) {
            call.device_handle = call_node["device_handle"].as<std::string>();
        }
        if (call_node["function_id"]) {
            call.function_id = call_node["function_id"].as<uint32_t>();
        }
        if (call_node["function_name"]) {
            call.function_name = call_node["function_name"].as<std::string>();
        }

        if (call_node["args"]) {
            if (!ensure_mapping(call_node["args"], call_path + ".args", error)) {
                return false;
            }
            for (const auto &arg_entry : call_node["args"]) {
                const std::string arg_key = arg_entry.first.as<std::string>();
                const std::string arg_path = std::format("{}.args.{}", call_path, arg_key);
                automation::ParameterValue arg_value;
                if (!parse_scalar_parameter_value(arg_entry.second, arg_value, error, arg_path)) {
                    return false;
                }
                call.args[arg_key] = arg_value;
            }
        }

        out.push_back(std::move(call));
        ++call_index;
    }

    return true;
}

}  // namespace

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

        // Authentication must have a token to compare against.
        if (config.http.auth_enabled && config.http.auth_token.empty()) {
            error = "http.auth_enabled=true requires http.auth_token (or the ANOLIS_API_TOKEN env var)";
            return false;
        }

        // Safety gate: never expose the REST surface to the network unauthenticated.
        if (!is_loopback_bind(config.http.bind) && !config.http.auth_enabled && !config.http.allow_insecure_bind) {
            error = "http.bind '" + config.http.bind +
                    "' is non-loopback but authentication is disabled; set http.auth_enabled=true "
                    "(recommended) or http.allow_insecure_bind=true to override";
            return false;
        }

        // TLS requires both a certificate and a key (or neither).
        if (config.http.tls_cert_path.empty() != config.http.tls_key_path.empty()) {
            error = "http TLS requires both tls_cert_path and tls_key_path (or neither)";
            return false;
        }

        // Bearer token over plaintext on the network is sniffable — warn (don't
        // block: a TLS-terminating reverse proxy is a valid deployment).
        const bool tls_on = !config.http.tls_cert_path.empty();
        if (config.http.auth_enabled && !is_loopback_bind(config.http.bind) && !tls_on) {
            LOG_WARN(
                "[Config] HTTP auth is enabled on a non-loopback bind without TLS; the Bearer token "
                "will be sent in plaintext. Configure http.tls_cert_path/tls_key_path or terminate TLS "
                "at a reverse proxy.");
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

    // Validate Health staleness overrides (#220). 0 means "derive from cadence";
    // a negative value is nonsense. When both are explicit, WARNING must precede
    // STALE.
    if (config.health.staleness.warn_after_ms < 0 || config.health.staleness.stale_after_ms < 0) {
        error = "health.staleness.warn_after_ms/stale_after_ms must be >= 0";
        return false;
    }
    if (config.health.staleness.warn_after_ms > 0 && config.health.staleness.stale_after_ms > 0 &&
        config.health.staleness.stale_after_ms <= config.health.staleness.warn_after_ms) {
        error = "health.staleness.stale_after_ms must be > warn_after_ms when both are set";
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

        const auto validate_hook_group = [&](const std::vector<ModeTransitionHookConfig> &hooks,
                                             const std::string &group_name) -> bool {
            for (size_t hook_index = 0; hook_index < hooks.size(); ++hook_index) {
                const auto &hook = hooks[hook_index];
                const std::string hook_path =
                    "automation.mode_transition_hooks." + group_name + "[" + std::to_string(hook_index) + "]";

                if (!hook.from.empty() && !is_valid_mode_name(hook.from)) {
                    error = hook_path + ".from must be one of MANUAL/AUTO/IDLE/FAULT/*";
                    return false;
                }
                if (!hook.to.empty() && !is_valid_mode_name(hook.to)) {
                    error = hook_path + ".to must be one of MANUAL/AUTO/IDLE/FAULT/*";
                    return false;
                }
                if (hook.calls.empty()) {
                    error = hook_path + ".calls must contain at least one call";
                    return false;
                }

                for (size_t call_index = 0; call_index < hook.calls.size(); ++call_index) {
                    const auto &call = hook.calls[call_index];
                    const std::string call_path = hook_path + ".calls[" + std::to_string(call_index) + "]";
                    if (call.device_handle.empty()) {
                        error = call_path + ".device_handle is required";
                        return false;
                    }
                    if (call.function_id == 0 && call.function_name.empty()) {
                        error = call_path + " must provide function_id or function_name";
                        return false;
                    }
                }
            }
            return true;
        };

        if (!validate_hook_group(config.automation.mode_transition_hooks.before_transition, "before_transition")) {
            return false;
        }
        if (!validate_hook_group(config.automation.mode_transition_hooks.after_transition, "after_transition")) {
            return false;
        }
    }

    // Safety safe-state calls are validated unconditionally: a pure-manual
    // machine has no automation section but may still declare a software
    // safe-state, and the estop ladder must be able to trust these calls.
    const auto validate_safety_calls = [&](const std::vector<ModeTransitionCallConfig> &calls,
                                           const std::string &list_name) -> bool {
        for (size_t i = 0; i < calls.size(); ++i) {
            const auto &call = calls[i];
            const std::string call_path = "safety.safe_state." + list_name + "[" + std::to_string(i) + "]";
            if (call.device_handle.empty()) {
                error = call_path + ".device_handle is required";
                return false;
            }
            if (call.function_id == 0 && call.function_name.empty()) {
                error = call_path + " must provide function_id or function_name";
                return false;
            }
        }
        return true;
    };
    if (!validate_safety_calls(config.safety.safe_state.hooks, "hooks")) {
        return false;
    }
    if (!validate_safety_calls(config.safety.safe_state.setpoints, "setpoints")) {
        return false;
    }

    return true;
}

// A machine can declare its safe state twice over, for two different triggers:
// `safety.safe_state` is what `POST /v0/estop` runs, while a `-> FAULT` entry in
// `automation.mode_transition_hooks` is what an autonomous fault runs. They are
// unconnected, and the refuse-hookless gate only ever asks for the second.
//
// So an operator who follows that gate's refusal message to the letter ends up
// with FAULT hooks and no `safety.safe_state`. Pressing the e-stop then ran
// nothing AND — because the latch engages before FAULT is entered — suppressed
// the FAULT hooks that any other route into FAULT would have executed. The
// safety-labelled route was the worse one (#251).
//
// When the operator has said nothing at all about `safety`, adopt their explicit
// `-> FAULT` hooks as the e-stop's ladder. Everything downstream then behaves
// normally: the ladder runs them as ordinary safe-state calls, their outcomes
// land in the e-stop response, and the FAULT-transition dispatch is untouched
// (still refused by the latch, so the two cannot double-drive).
//
// Two deliberate limits:
//   - `to` must be exactly FAULT. A wildcard hook fires on EVERY transition and
//     is not a safe-state declaration; its author never claimed it was one.
//   - `from` must be a wildcard. The ladder has no notion of which mode it is
//     leaving, so a hook that only claims to be correct when leaving AUTO cannot
//     be run unconditionally.
// A machine whose hooks do not qualify is left exactly as it was, and the
// startup preflight already warns that it has no software safe state.
void adopt_fault_hooks_as_safe_state(RuntimeConfig &config, bool safety_section_declared) {
    // An operator who wrote a `safety:` block considered the question. Even if it
    // declares nothing, that is an answer -- respect it rather than overriding a
    // deliberate latch-only choice.
    if (safety_section_declared) {
        return;
    }
    if (!config.automation.enabled) {
        return;
    }
    const auto &safe_state = config.safety.safe_state;
    if (!safe_state.hooks.empty() || !safe_state.setpoints.empty() || safe_state.zero_is_safe) {
        return;
    }

    const auto qualifies = [](const ModeTransitionHookConfig &hook) {
        const bool targets_fault = hook.to == "FAULT";
        const bool from_any = hook.from.empty() || hook.from == "*";
        return targets_fault && from_any;
    };

    std::vector<ModeTransitionCallConfig> adopted;
    for (const auto *group : {&config.automation.mode_transition_hooks.before_transition,
                              &config.automation.mode_transition_hooks.after_transition}) {
        for (const auto &hook : *group) {
            if (!qualifies(hook)) {
                continue;
            }
            adopted.insert(adopted.end(), hook.calls.begin(), hook.calls.end());
        }
    }

    if (adopted.empty()) {
        return;
    }

    config.safety.safe_state.hooks = std::move(adopted);
    LOG_INFO("[Config] No safety.safe_state declared; adopting "
             << config.safety.safe_state.hooks.size()
             << " call(s) from the '* -> FAULT' mode-transition hooks as the software safe state. "
                "POST /v0/estop will run them. Declare a safety.safe_state block to override, "
                "or an empty one to keep the e-stop latch-only.");
}

bool load_config(const std::string &config_path, RuntimeConfig &config, std::string &error) {
    try {
        YAML::Node yaml = YAML::LoadFile(config_path);
        if (!yaml.IsMap()) {
            error = "Config root must be a map";
            return false;
        }

        // Start from defaults on every load so omitted sections do not retain
        // stale values from previous files.
        config = RuntimeConfig{};

        // Unknown keys are warned about and ignored so config evolution can be
        // rolled out without breaking older runtimes immediately.
        const std::vector<std::string> valid_keys = {"runtime", "http",       "providers", "polling", "telemetry",
                                                     "logging", "automation", "safety",    "health"};
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
            if (!ensure_mapping(yaml["runtime"], "runtime", error)) {
                return false;
            }
            warn_unknown_keys(yaml["runtime"], "runtime",
                              {"name", "shutdown_timeout_ms", "startup_timeout_ms", "data_dir", "mode"});

            if (yaml["runtime"]["name"]) {
                config.runtime.name = yaml["runtime"]["name"].as<std::string>();
            }
            if (yaml["runtime"]["data_dir"]) {
                config.runtime.data_dir = yaml["runtime"]["data_dir"].as<std::string>();
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
            if (!ensure_mapping(yaml["http"], "http", error)) {
                return false;
            }
            warn_unknown_keys(yaml["http"], "http",
                              {"enabled", "bind", "port", "cors_allowed_origins", "cors_allow_credentials",
                               "thread_pool_size", "auth_enabled", "auth_token", "auth_exempt_loopback",
                               "allow_insecure_bind", "tls_cert_path", "tls_key_path"});

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
                } else {
                    error = "'http.cors_allowed_origins' must be a string or sequence";
                    return false;
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

            // Authentication
            if (yaml["http"]["auth_enabled"]) {
                config.http.auth_enabled = yaml["http"]["auth_enabled"].as<bool>();
            }
            if (yaml["http"]["auth_token"]) {
                config.http.auth_token = yaml["http"]["auth_token"].as<std::string>();
            }
            if (yaml["http"]["auth_exempt_loopback"]) {
                config.http.auth_exempt_loopback = yaml["http"]["auth_exempt_loopback"].as<bool>();
            }
            if (yaml["http"]["allow_insecure_bind"]) {
                config.http.allow_insecure_bind = yaml["http"]["allow_insecure_bind"].as<bool>();
            }

            // TLS
            if (yaml["http"]["tls_cert_path"]) {
                config.http.tls_cert_path = yaml["http"]["tls_cert_path"].as<std::string>();
            }
            if (yaml["http"]["tls_key_path"]) {
                config.http.tls_key_path = yaml["http"]["tls_key_path"].as<std::string>();
            }

            // Environment fallback: only when auth is enabled and the config did
            // not provide a token. Mirrors the INFLUXDB_TOKEN handling below.
            if (config.http.auth_enabled && config.http.auth_token.empty()) {
#ifdef _WIN32
                char *token_env = nullptr;
                size_t token_len = 0;
                if (_dupenv_s(&token_env, &token_len, "ANOLIS_API_TOKEN") == 0 && token_env != nullptr) {
                    config.http.auth_token = token_env;
                    std::free(token_env);
                }
#else
                const char *token_env = std::getenv("ANOLIS_API_TOKEN");
                if (token_env != nullptr) {
                    config.http.auth_token = token_env;
                }
#endif
            }
        }

        // Provider blocks are parsed independently so structural validation can
        // point at the exact provider index that failed.
        if (yaml["providers"]) {
            if (!ensure_sequence(yaml["providers"], "providers", error)) {
                return false;
            }

            config.providers.clear();
            size_t provider_index = 0;
            for (const auto &provider_node : yaml["providers"]) {
                const std::string provider_path = "providers[" + std::to_string(provider_index) + "]";
                if (!provider_node.IsMap()) {
                    error = "'" + provider_path + "' must be a map";
                    return false;
                }
                warn_unknown_keys(
                    provider_node, provider_path,
                    {"id", "command", "args", "timeout_ms", "hello_timeout_ms", "ready_timeout_ms", "restart_policy"});

                provider::ProviderConfig provider;

                if (provider_node["id"]) {
                    provider.id = provider_node["id"].as<std::string>();
                }

                if (provider_node["command"]) {
                    provider.command = provider_node["command"].as<std::string>();
                }

                if (provider_node["args"]) {
                    if (!ensure_sequence(provider_node["args"], provider_path + ".args", error)) {
                        return false;
                    }
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

                // Restart policy is nested because its defaults and validation
                // rules belong to the provider entry, not the global runtime.
                if (provider_node["restart_policy"]) {
                    const auto &rp = provider_node["restart_policy"];
                    if (!ensure_mapping(rp, provider_path + ".restart_policy", error)) {
                        return false;
                    }
                    warn_unknown_keys(rp, provider_path + ".restart_policy",
                                      {"enabled", "max_attempts", "backoff_ms", "timeout_ms", "success_reset_ms"});

                    if (rp["enabled"]) {
                        provider.restart_policy.enabled = rp["enabled"].as<bool>();
                    }

                    if (rp["max_attempts"]) {
                        provider.restart_policy.max_attempts = rp["max_attempts"].as<int>();
                    }

                    if (rp["backoff_ms"]) {
                        if (!ensure_sequence(rp["backoff_ms"], provider_path + ".restart_policy.backoff_ms", error)) {
                            return false;
                        }
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
                ++provider_index;
            }
        }

        // Load polling config
        if (yaml["polling"]) {
            if (!ensure_mapping(yaml["polling"], "polling", error)) {
                return false;
            }
            if (yaml["polling"]["interval_ms"]) {
                config.polling.interval_ms = yaml["polling"]["interval_ms"].as<int>();
            }
        }

        // Load health config (#220): device-liveness staleness thresholds.
        if (yaml["health"]) {
            if (!ensure_mapping(yaml["health"], "health", error)) {
                return false;
            }
            warn_unknown_keys(yaml["health"], "health", {"staleness"});
            if (yaml["health"]["staleness"]) {
                if (!ensure_mapping(yaml["health"]["staleness"], "health.staleness", error)) {
                    return false;
                }
                warn_unknown_keys(yaml["health"]["staleness"], "health.staleness", {"warn_after_ms", "stale_after_ms"});
                if (yaml["health"]["staleness"]["warn_after_ms"]) {
                    config.health.staleness.warn_after_ms = yaml["health"]["staleness"]["warn_after_ms"].as<int>();
                }
                if (yaml["health"]["staleness"]["stale_after_ms"]) {
                    config.health.staleness.stale_after_ms = yaml["health"]["staleness"]["stale_after_ms"].as<int>();
                }
            }
        }

        // Load telemetry config
        if (yaml["telemetry"]) {
            if (!ensure_mapping(yaml["telemetry"], "telemetry", error)) {
                return false;
            }
            warn_unknown_keys(yaml["telemetry"], "telemetry",
                              {"enabled", "influxdb", "influx_url", "influx_org", "influx_bucket", "influx_token",
                               "batch_size", "flush_interval_ms", "health_interval_ms"});

            if (yaml["telemetry"]["enabled"]) {
                config.telemetry.enabled = yaml["telemetry"]["enabled"].as<bool>();
            }

            if (yaml["telemetry"]["health_interval_ms"]) {
                config.telemetry.health_interval_ms = yaml["telemetry"]["health_interval_ms"].as<int>();
                if (config.telemetry.health_interval_ms < 0) {
                    error = "telemetry.health_interval_ms must be >= 0";
                    return false;
                }
            }

            // Accept the older flat telemetry shape long enough to warn and map
            // it, but prefer the canonical nested `telemetry.influxdb.*` form.
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

            // Canonical nested telemetry settings are loaded after warnings so
            // newer keys cleanly override any deprecated flat values.
            if (yaml["telemetry"]["influxdb"]) {
                auto influx = yaml["telemetry"]["influxdb"];
                if (!ensure_mapping(influx, "telemetry.influxdb", error)) {
                    return false;
                }
                warn_unknown_keys(influx, "telemetry.influxdb",
                                  {"url", "org", "bucket", "token", "batch_size", "flush_interval_ms",
                                   "max_retry_buffer_size", "queue_size"});

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

            // Environment fallback is used only when telemetry is enabled and
            // the config file did not provide a token explicitly.
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
            if (!ensure_mapping(yaml["logging"], "logging", error)) {
                return false;
            }
            if (yaml["logging"]["level"]) {
                config.logging.level = yaml["logging"]["level"].as<std::string>();
            }
        }

        // Whether the operator said ANYTHING about safety. Distinguishing
        // "declared nothing" from "declared none" is what makes the adoption
        // below safe to do automatically — see adopt_fault_hooks_as_safe_state.
        const bool safety_section_declared = static_cast<bool>(yaml["safety"]);

        // Load safety config. Parsed unconditionally (independent of automation)
        // so a pure-manual machine can still declare a software safe-state.
        if (yaml["safety"]) {
            if (!ensure_mapping(yaml["safety"], "safety", error)) {
                return false;
            }
            warn_unknown_keys(yaml["safety"], "safety", {"safe_state"});

            if (yaml["safety"]["safe_state"]) {
                const auto &safe_state_node = yaml["safety"]["safe_state"];
                if (!ensure_mapping(safe_state_node, "safety.safe_state", error)) {
                    return false;
                }
                warn_unknown_keys(safe_state_node, "safety.safe_state", {"hooks", "setpoints", "zero_is_safe"});

                if (safe_state_node["hooks"] && !parse_call_list(safe_state_node["hooks"], "safety.safe_state.hooks",
                                                                 config.safety.safe_state.hooks, error)) {
                    return false;
                }
                if (safe_state_node["setpoints"] &&
                    !parse_call_list(safe_state_node["setpoints"], "safety.safe_state.setpoints",
                                     config.safety.safe_state.setpoints, error)) {
                    return false;
                }
                if (safe_state_node["zero_is_safe"]) {
                    config.safety.safe_state.zero_is_safe = safe_state_node["zero_is_safe"].as<bool>();
                }
            }
        }

        // Load automation config
        if (yaml["automation"]) {
            if (!ensure_mapping(yaml["automation"], "automation", error)) {
                return false;
            }
            warn_unknown_keys(yaml["automation"], "automation",
                              {"enabled", "behavior_tree", "behavior_tree_path", "tick_rate_hz", "manual_gating_policy",
                               "parameters", "mode_transition_hooks"});

            if (yaml["automation"]["enabled"]) {
                config.automation.enabled = yaml["automation"]["enabled"].as<bool>();
            }
            if (yaml["automation"]["behavior_tree"]) {
                config.automation.behavior_tree = yaml["automation"]["behavior_tree"].as<std::string>();
            } else if (yaml["automation"]["behavior_tree_path"]) {
                LOG_WARN(
                    "[Config] Deprecated automation key 'automation.behavior_tree_path' - use "
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

            // Parameter parsing is type-directed: the declared parameter type
            // controls how the default value and constraints are interpreted.
            if (yaml["automation"]["parameters"]) {
                if (!ensure_sequence(yaml["automation"]["parameters"], "automation.parameters", error)) {
                    return false;
                }

                config.automation.parameters.clear();
                size_t param_index = 0;
                for (const auto &param_node : yaml["automation"]["parameters"]) {
                    const std::string param_path = "automation.parameters[" + std::to_string(param_index) + "]";
                    if (!param_node.IsMap()) {
                        error = "'" + param_path + "' must be a map";
                        return false;
                    }
                    warn_unknown_keys(param_node, param_path,
                                      {"name", "type", "default", "min", "max", "allowed_values"});

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

                    // Start from a typed zero/default so omitted defaults still
                    // produce a value consistent with the declared type.
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
                        if (!ensure_sequence(param_node["allowed_values"], param_path + ".allowed_values", error)) {
                            return false;
                        }
                        for (const auto &val : param_node["allowed_values"]) {
                            param.allowed_values.push_back(val.as<std::string>());
                        }
                    }

                    config.automation.parameters.push_back(param);
                    ++param_index;
                }
            }

            if (yaml["automation"]["mode_transition_hooks"]) {
                const auto &hooks_node = yaml["automation"]["mode_transition_hooks"];
                if (!ensure_mapping(hooks_node, "automation.mode_transition_hooks", error)) {
                    return false;
                }
                warn_unknown_keys(hooks_node, "automation.mode_transition_hooks",
                                  {"before_transition", "after_transition"});

                const auto parse_hook_group = [&](const YAML::Node &group_node, const std::string &group_path,
                                                  std::vector<ModeTransitionHookConfig> &out) -> bool {
                    out.clear();
                    if (!group_node || group_node.IsNull()) {
                        return true;
                    }
                    if (!ensure_sequence(group_node, group_path, error)) {
                        return false;
                    }

                    size_t hook_index = 0;
                    for (const auto &hook_node : group_node) {
                        const std::string hook_path = group_path + "[" + std::to_string(hook_index) + "]";
                        if (!hook_node.IsMap()) {
                            error = std::format("'{}' must be a map", hook_path);
                            return false;
                        }
                        warn_unknown_keys(hook_node, hook_path, {"from", "to", "fail_on_error", "calls"});

                        ModeTransitionHookConfig hook;
                        if (hook_node["from"]) {
                            hook.from = hook_node["from"].as<std::string>();
                        }
                        if (hook_node["to"]) {
                            hook.to = hook_node["to"].as<std::string>();
                        }
                        if (hook_node["fail_on_error"]) {
                            hook.fail_on_error = hook_node["fail_on_error"].as<bool>();
                        }

                        if (!hook_node["calls"]) {
                            error = std::format("'{}.calls' is required", hook_path);
                            return false;
                        }
                        if (!parse_call_list(hook_node["calls"], hook_path + ".calls", hook.calls, error)) {
                            return false;
                        }

                        out.push_back(std::move(hook));
                        ++hook_index;
                    }

                    return true;
                };

                if (!parse_hook_group(hooks_node["before_transition"],
                                      "automation.mode_transition_hooks.before_transition",
                                      config.automation.mode_transition_hooks.before_transition)) {
                    return false;
                }
                if (!parse_hook_group(hooks_node["after_transition"],
                                      "automation.mode_transition_hooks.after_transition",
                                      config.automation.mode_transition_hooks.after_transition)) {
                    return false;
                }
            }
        }

        // Structural YAML checks above guarantee node shape; centralized
        // validation here enforces cross-field and semantic constraints.
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
            http_msg << " (" << (config.http.tls_cert_path.empty() ? "http" : "https") << "://" << config.http.bind
                     << ":" << config.http.port << ", auth " << (config.http.auth_enabled ? "on" : "off") << ")";
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

        adopt_fault_hooks_as_safe_state(config, safety_section_declared);

        std::stringstream automation_msg;
        automation_msg << "[Config] Automation: " << (config.automation.enabled ? "enabled" : "disabled");
        if (config.automation.enabled) {
            automation_msg << " (BT: " << config.automation.behavior_tree
                           << ", tick rate: " << config.automation.tick_rate_hz << " Hz, "
                           << config.automation.parameters.size() << " parameters, hooks: before="
                           << config.automation.mode_transition_hooks.before_transition.size()
                           << ", after=" << config.automation.mode_transition_hooks.after_transition.size() << ")";
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
