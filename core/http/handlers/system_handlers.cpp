#include <chrono>
#include <fstream>

#include "anolis_build_config.hpp"
#if ANOLIS_ENABLE_AUTOMATION
#include "../../automation/bt_runtime.hpp"
#endif
#include "../../automation/mode_manager.hpp"
#include "../../automation/parameter_manager.hpp"
#include "../../logging/logger.hpp"
#include "../../provider/i_provider_handle.hpp"    // Need provider definition for handle_get_runtime_status
#include "../../provider/provider_supervisor.hpp"  // For ProviderSupervisionSnapshot
#include "../../registry/device_registry.hpp"
#include "../json.hpp"
#include "../server.hpp"
#include "utils.hpp"

namespace anolis {
namespace http {

namespace {
std::string derive_lifecycle_state(bool is_available,
                                   const provider::ProviderSupervisor::ProviderSupervisionSnapshot &snap) {
    if (is_available) {
        return snap.attempt_count > 0 ? "RECOVERING" : "RUNNING";
    }

    if (snap.circuit_open) {
        return "CIRCUIT_OPEN";
    }

    if (snap.crash_detected || snap.attempt_count > 0 || snap.next_restart_in_ms.has_value()) {
        return "RESTARTING";
    }

    return "DOWN";
}
}  // namespace

//=============================================================================
// GET /v0/runtime/status
//=============================================================================
void HttpServer::handle_get_runtime_status(const httplib::Request &, httplib::Response &res) {
    // Calculate uptime (approximate - could be tracked more precisely)
    static auto start_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    // Build provider status list
    nlohmann::json providers_json = nlohmann::json::array();
    size_t total_device_count = 0;

    for (const auto &[provider_id, provider] : provider_registry_.get_all_providers()) {
        auto devices = registry_.get_devices_for_provider(provider_id);

        std::string state = provider->is_available() ? "AVAILABLE" : "UNAVAILABLE";

        providers_json.push_back({{"provider_id", provider_id}, {"state", state}, {"device_count", devices.size()}});

        total_device_count += devices.size();
    }

    // Get current mode from mode manager
    std::string current_mode = "MANUAL";
    if (mode_manager_ != nullptr) {
        current_mode = automation::mode_to_string(mode_manager_->current_mode());
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)},
                               {"mode", current_mode},
                               {"uptime_seconds", uptime},
                               {"polling_interval_ms", polling_interval_ms_},
                               {"providers", providers_json},
                               {"device_count", total_device_count}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// GET /v0/mode - Get current automation mode
//=============================================================================
void HttpServer::handle_get_mode(const httplib::Request &, httplib::Response &res) {
    // If automation not enabled, return error
    if (mode_manager_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    auto current = mode_manager_->current_mode();
    std::string mode_str = automation::mode_to_string(current);

    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"mode", mode_str}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// POST /v0/mode - Set automation mode
//=============================================================================
void HttpServer::handle_post_mode(const httplib::Request &req, httplib::Response &res) {
    // If automation not enabled, return error
    if (mode_manager_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Parse request body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception &e) {
        nlohmann::json response =
            make_error_response(StatusCode::INVALID_ARGUMENT, std::string("Invalid JSON: ") + e.what());
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Validate required field
    if (!body.contains("mode") || !body["mode"].is_string()) {
        nlohmann::json response =
            make_error_response(StatusCode::INVALID_ARGUMENT,
                                "Missing or invalid 'mode' field (expected string: MANUAL, AUTO, IDLE, or FAULT)");
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    std::string mode_str = body["mode"];

    // Validate mode string (must be exact match)
    if (mode_str != "MANUAL" && mode_str != "AUTO" && mode_str != "IDLE" && mode_str != "FAULT") {
        nlohmann::json response = make_error_response(
            StatusCode::INVALID_ARGUMENT, "Invalid mode: '" + mode_str + "' (must be MANUAL, AUTO, IDLE, or FAULT)");
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Parse mode string
    auto new_mode = automation::string_to_mode(mode_str);
    if (!new_mode) {
        // This should never happen since we validated above, but handle defensive
        nlohmann::json response = make_error_response(StatusCode::INVALID_ARGUMENT, "Invalid mode string");
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Attempt mode transition
    std::string error;
    if (!mode_manager_->set_mode(*new_mode, error)) {
        nlohmann::json response = make_error_response(StatusCode::FAILED_PRECONDITION, error);
        send_json(res, StatusCode::FAILED_PRECONDITION, response);
        return;
    }

    // Success - return new mode
    auto current = mode_manager_->current_mode();
    std::string current_str = automation::mode_to_string(current);

    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"mode", current_str}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// GET /v0/parameters - Get all parameters
//=============================================================================
void HttpServer::handle_get_parameters(const httplib::Request &, httplib::Response &res) {
    // If parameter manager not available, return error
    if (parameter_manager_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Parameter system not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Get all parameter definitions
    auto params = parameter_manager_->get_all_definitions();

    nlohmann::json parameters_json = nlohmann::json::array();
    for (const auto &[name, def] : params) {
        nlohmann::json param_json = {{"name", name}, {"type", automation::parameter_type_to_string(def.type)}};

        // Add current value based on type
        if (std::holds_alternative<double>(def.value)) {
            param_json["value"] = std::get<double>(def.value);
        } else if (std::holds_alternative<int64_t>(def.value)) {
            param_json["value"] = std::get<int64_t>(def.value);
        } else if (std::holds_alternative<bool>(def.value)) {
            param_json["value"] = std::get<bool>(def.value);
        } else if (std::holds_alternative<std::string>(def.value)) {
            param_json["value"] = std::get<std::string>(def.value);
        }

        // Add constraints if present
        if (def.min.has_value()) {
            param_json["min"] = def.min.value();
        }
        if (def.max.has_value()) {
            param_json["max"] = def.max.value();
        }
        if (def.allowed_values.has_value() && !def.allowed_values.value().empty()) {
            param_json["allowed_values"] = def.allowed_values.value();
        }

        parameters_json.push_back(param_json);
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"parameters", parameters_json}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// POST /v0/parameters - Update parameter value
//=============================================================================
void HttpServer::handle_post_parameters(const httplib::Request &req, httplib::Response &res) {
    // If parameter manager not available, return error
    if (parameter_manager_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Parameter system not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Parse request body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception &e) {
        nlohmann::json response =
            make_error_response(StatusCode::INVALID_ARGUMENT, std::string("Invalid JSON: ") + e.what());
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Validate required fields
    if (!body.contains("name") || !body["name"].is_string()) {
        nlohmann::json response =
            make_error_response(StatusCode::INVALID_ARGUMENT, "Missing or invalid 'name' field (expected string)");
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    if (!body.contains("value")) {
        nlohmann::json response = make_error_response(StatusCode::INVALID_ARGUMENT, "Missing 'value' field");
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    std::string param_name = body["name"];

    // Get parameter definition to determine type
    auto def_opt = parameter_manager_->get_definition(param_name);
    if (!def_opt.has_value()) {
        nlohmann::json response =
            make_error_response(StatusCode::NOT_FOUND, "Parameter '" + param_name + "' not found");
        send_json(res, StatusCode::NOT_FOUND, response);
        return;
    }

    const auto &def = def_opt.value();

    // Parse value based on parameter type
    automation::ParameterValue new_value;
    try {
        if (def.type == automation::ParameterType::DOUBLE) {
            if (!body["value"].is_number()) {
                throw std::runtime_error("Expected number for double parameter");
            }
            new_value = body["value"].get<double>();
        } else if (def.type == automation::ParameterType::INT64) {
            if (!body["value"].is_number_integer()) {
                throw std::runtime_error("Expected integer for int64 parameter");
            }
            new_value = body["value"].get<int64_t>();
        } else if (def.type == automation::ParameterType::BOOL) {
            if (!body["value"].is_boolean()) {
                throw std::runtime_error("Expected boolean for bool parameter");
            }
            new_value = body["value"].get<bool>();
        } else if (def.type == automation::ParameterType::STRING) {
            if (!body["value"].is_string()) {
                throw std::runtime_error("Expected string for string parameter");
            }
            new_value = body["value"].get<std::string>();
        }
    } catch (const std::exception &e) {
        nlohmann::json response =
            make_error_response(StatusCode::INVALID_ARGUMENT, std::string("Invalid value: ") + e.what());
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Set parameter value with validation
    std::string error;
    if (!parameter_manager_->set(param_name, new_value, error)) {
        nlohmann::json response = make_error_response(StatusCode::INVALID_ARGUMENT, error);
        send_json(res, StatusCode::INVALID_ARGUMENT, response);
        return;
    }

    // Success - return updated parameter
    auto updated_value = parameter_manager_->get(param_name).value();
    nlohmann::json value_json;
    if (std::holds_alternative<double>(updated_value)) {
        value_json = std::get<double>(updated_value);
    } else if (std::holds_alternative<int64_t>(updated_value)) {
        value_json = std::get<int64_t>(updated_value);
    } else if (std::holds_alternative<bool>(updated_value)) {
        value_json = std::get<bool>(updated_value);
    } else if (std::holds_alternative<std::string>(updated_value)) {
        value_json = std::get<std::string>(updated_value);
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)},
                               {"parameter", {{"name", param_name}, {"value", value_json}}}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// GET /v0/automation/tree - Get loaded behavior tree XML
//=============================================================================
void HttpServer::handle_get_automation_tree(const httplib::Request &, httplib::Response &res) {
#if ANOLIS_ENABLE_AUTOMATION
    // If automation not enabled or bt_runtime not available
    if (bt_runtime_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Get the tree path
    const std::string &tree_path = bt_runtime_->get_tree_path();
    if (tree_path.empty()) {
        nlohmann::json response = make_error_response(StatusCode::NOT_FOUND, "No behavior tree loaded");
        send_json(res, StatusCode::NOT_FOUND, response);
        return;
    }

    // Read the file
    std::ifstream file(tree_path);
    if (!file.is_open()) {
        std::string error = "Failed to read behavior tree file: " + tree_path;
        nlohmann::json response = make_error_response(StatusCode::INTERNAL, error);
        send_json(res, StatusCode::INTERNAL, response);
        return;
    }

    std::string xml_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Return XML content
    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"tree", xml_content}};

    send_json(res, StatusCode::OK, response);
#else
    nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer disabled at build time");
    send_json(res, StatusCode::UNAVAILABLE, response);
#endif
}

//=============================================================================
// GET /v0/automation/status
//=============================================================================
void HttpServer::handle_get_automation_status(const httplib::Request &, httplib::Response &res) {
#if ANOLIS_ENABLE_AUTOMATION
    // If automation not enabled or bt_runtime not available
    if (bt_runtime_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer not enabled");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Check if mode_manager is available (should be if bt_runtime exists)
    if (mode_manager_ == nullptr) {
        nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Mode manager not available");
        send_json(res, StatusCode::UNAVAILABLE, response);
        return;
    }

    // Get health status from BT runtime
    auto health = bt_runtime_->get_health();

    // Convert BTStatus enum to string
    std::string bt_status_str;
    switch (health.bt_status) {
        case automation::BTStatus::BT_IDLE:
            bt_status_str = "IDLE";
            break;
        case automation::BTStatus::BT_RUNNING:
            bt_status_str = "RUNNING";
            break;
        case automation::BTStatus::BT_STALLED:
            bt_status_str = "STALLED";
            break;
        case automation::BTStatus::BT_ERROR:
            bt_status_str = "ERROR";
            break;
        default:
            bt_status_str = "UNKNOWN";
            break;
    }

    // Get current mode
    bool enabled = (mode_manager_->current_mode() == automation::RuntimeMode::AUTO);
    bool active = bt_runtime_->is_running();

    // Extract tree name from path (just filename without extension)
    std::string tree_name;
    if (!health.current_tree.empty()) {
        size_t last_slash = health.current_tree.find_last_of("/\\");
        size_t last_dot = health.current_tree.find_last_of('.');
        if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash) {
            tree_name = health.current_tree.substr(last_slash + 1, last_dot - last_slash - 1);
        } else if (last_slash != std::string::npos) {
            tree_name = health.current_tree.substr(last_slash + 1);
        } else {
            tree_name = health.current_tree;
        }
    }

    nlohmann::json response = {
        {"status", make_status(StatusCode::OK)},
        {"enabled", enabled},
        {"active", active},
        {"bt_status", bt_status_str},
        {"last_tick_ms", health.last_tick_ms},
        {"ticks_since_progress", health.ticks_since_progress},
        {"total_ticks", health.total_ticks},
        {"last_error", health.last_error.empty() ? nlohmann::json() : nlohmann::json(health.last_error)},
        {"error_count", health.error_count},
        {"current_tree", tree_name}};

    send_json(res, StatusCode::OK, response);
#else
    nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer disabled at build time");
    send_json(res, StatusCode::UNAVAILABLE, response);
#endif
}

//=============================================================================
// GET /v0/providers/health
//=============================================================================
void HttpServer::handle_get_providers_health(const httplib::Request &, httplib::Response &res) {
    using namespace std::chrono;
    using SupervisionSnapshot = provider::ProviderSupervisor::ProviderSupervisionSnapshot;

    // Collect all supervision snapshots once (thread-safe copy).
    std::unordered_map<std::string, SupervisionSnapshot> snapshots;
    if (supervisor_) {
        snapshots = supervisor_->get_all_snapshots();
    }

    nlohmann::json providers_json = nlohmann::json::array();

    // Iterate through all providers
    for (const auto &[provider_id, provider] : provider_registry_.get_all_providers()) {
        // Get provider availability
        bool is_available = provider->is_available();
        std::string state = is_available ? "AVAILABLE" : "UNAVAILABLE";

        // Get devices for this provider
        auto devices = registry_.get_devices_for_provider(provider_id);

        nlohmann::json devices_json = nlohmann::json::array();

        // Check health of each device
        for (const auto &device : devices) {
            std::string device_handle = provider_id + "/" + device.device_id;
            auto device_state = state_cache_.get_device_state(device_handle);

            std::string health_status = "UNKNOWN";
            uint64_t last_poll_ms = 0;
            uint64_t staleness_ms = 0;

            if (device_state) {
                last_poll_ms = duration_cast<milliseconds>(device_state->last_poll_time.time_since_epoch()).count();

                auto now = system_clock::now();
                auto age_ms = duration_cast<milliseconds>(now - device_state->last_poll_time).count();

                staleness_ms = age_ms;

                // Determine health based on staleness
                // OK: < 2 seconds, WARNING: 2-5 seconds, STALE: > 5 seconds
                if (!is_available) {
                    health_status = "UNAVAILABLE";
                } else if (age_ms < 2000) {
                    health_status = "OK";
                } else if (age_ms < 5000) {
                    health_status = "WARNING";
                } else {
                    health_status = "STALE";
                }
            } else {
                health_status = "UNKNOWN";
            }

            devices_json.push_back({{"device_id", device.device_id},
                                    {"health", health_status},
                                    {"last_poll_ms", last_poll_ms},
                                    {"staleness_ms", staleness_ms}});
        }

        // Build provider-level timing fields and supervision block.
        // supervision is always emitted as an object, never null.
        nlohmann::json last_seen_ago_ms_json = nullptr;
        int64_t uptime_seconds = 0;
        nlohmann::json supervision_json;
        std::string lifecycle_state = is_available ? "RUNNING" : "DOWN";

        auto snap_it = snapshots.find(provider_id);
        if (snap_it != snapshots.end()) {
            const auto &snap = snap_it->second;
            lifecycle_state = derive_lifecycle_state(is_available, snap);

            // last_seen_ago_ms: null before first heartbeat, otherwise duration.
            last_seen_ago_ms_json = snap.last_seen_ago_ms.has_value() ? nlohmann::json(snap.last_seen_ago_ms.value())
                                                                      : nlohmann::json(nullptr);

            // uptime_seconds forced to 0 when UNAVAILABLE (process may have crashed).
            uptime_seconds = is_available ? snap.uptime_seconds : 0;

            supervision_json = {{"enabled", snap.supervision_enabled},
                                {"attempt_count", snap.attempt_count},
                                {"max_attempts", snap.max_attempts},
                                {"crash_detected", snap.crash_detected},
                                {"circuit_open", snap.circuit_open},
                                {"next_restart_in_ms", snap.next_restart_in_ms.has_value()
                                                           ? nlohmann::json(snap.next_restart_in_ms.value())
                                                           : nlohmann::json(nullptr)}};
        } else {
            // No supervisor or provider not yet registered: emit zeroed supervision.
            supervision_json = {{"enabled", false},        {"attempt_count", 0},    {"max_attempts", 0},
                                {"crash_detected", false}, {"circuit_open", false}, {"next_restart_in_ms", nullptr}};
        }

        providers_json.push_back({{"provider_id", provider_id},
                                  {"state", state},
                                  {"lifecycle_state", lifecycle_state},
                                  {"last_seen_ago_ms", last_seen_ago_ms_json},
                                  {"uptime_seconds", uptime_seconds},
                                  {"device_count", devices.size()},
                                  {"supervision", supervision_json},
                                  {"devices", devices_json}});
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"providers", providers_json}};

    send_json(res, StatusCode::OK, response);
}

}  // namespace http
}  // namespace anolis
