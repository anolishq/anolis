#include <chrono>
#include <fstream>
#include <map>

#include "anolis_build_config.hpp"
#if ANOLIS_ENABLE_AUTOMATION
#include "../../automation/bt_runtime.hpp"
#endif
#include "../../automation/mode_manager.hpp"
#include "../../automation/parameter_manager.hpp"
#include "../../control/safe_state.hpp"
#include "../../health/health_snapshot.hpp"
#include "../../logging/logger.hpp"
#include "../../provider/i_provider_handle.hpp"    // Need provider definition for handle_get_runtime_status
#include "../../provider/provider_supervisor.hpp"  // For ProviderSupervisionSnapshot
#include "../../registry/device_registry.hpp"
#include "../../runs/run_journal.hpp"
#include "../../telemetry/influx_sink.hpp"
#include "../json.hpp"
#include "../server.hpp"
#include "utils.hpp"

namespace anolis {
namespace http {

//=============================================================================
// GET /v0/runtime/status
//=============================================================================
void HttpServer::handle_get_runtime_status(const httplib::Request &, httplib::Response &res) {
    // Calculate uptime from server start (set in HttpServer::start())
    auto uptime =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_).count();

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
                               {"version", ANOLIS_VERSION},
                               {"mode", current_mode},
                               {"uptime_seconds", uptime},
                               {"polling_interval_ms", polling_interval_ms_},
                               {"providers", providers_json},
                               {"device_count", total_device_count}};

    // Software safe-state / e-stop surface: what a POST /v0/estop would do now,
    // and whether actuation is currently latched.
    if (safe_state_ != nullptr) {
        const auto cap = safe_state_->capability();
        nlohmann::json estop = {{"latched", cap.latched},
                                {"software_safe_state", control::safe_state_kind_to_string(cap.software_safe_state)},
                                {"uncovered_actuating_functions", cap.uncovered_actuating_functions}};
        if (cap.latched_at_epoch_ms.has_value()) {
            estop["latched_at_epoch_ms"] = *cap.latched_at_epoch_ms;
        } else {
            estop["latched_at_epoch_ms"] = nullptr;
        }
        response["estop"] = estop;
    }

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
// POST /v0/estop - Engage the software safe-state (latching)
//=============================================================================
void HttpServer::handle_post_estop(const httplib::Request &req, httplib::Response &res) {
    if (safe_state_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Safe-state controller not available"));
        return;
    }

    // An optional {"reason": "..."} body is recorded for logging only.
    std::string reason = "operator";
    if (!req.body.empty()) {
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (body.contains("reason") && body["reason"].is_string()) {
                reason = body["reason"].get<std::string>();
            }
        } catch (const std::exception &) {
            // Ignore an unparseable body: e-stop must not be blocked by bad input.
        }
    }

    const auto result = safe_state_->trigger(reason);

    nlohmann::json actions = nlohmann::json::array();
    for (const auto &action : result.actions) {
        nlohmann::json entry = {
            {"device_handle", action.device_handle}, {"function", action.function}, {"success", action.success}};
        if (!action.error.empty()) {
            entry["error"] = action.error;
        }
        actions.push_back(entry);
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)},
                               {"latched", true},
                               {"software_safe_state", control::safe_state_kind_to_string(result.kind)},
                               {"actions", actions}};
    if (result.kind == control::SafeStateKind::None) {
        response["warning"] = "No software safe-state declared; the hardware e-stop is the only safe-state path";
    }
    if (result.fault_requested) {
        response["fault_engaged"] = result.fault_ok;
    }

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// POST /v0/estop/clear - Release the e-stop latch
//=============================================================================
void HttpServer::handle_post_estop_clear(const httplib::Request &, httplib::Response &res) {
    if (safe_state_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Safe-state controller not available"));
        return;
    }

    safe_state_->clear();
    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"latched", false}};
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

    // Success - return updated parameter.
    // Safe by invariant: the set() above only returns true when param_name is
    // present, and ParameterManager has no remove/erase API, so get() here is
    // guaranteed to hold a value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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

    // Health is still consulted for the neutral last_error field.
    auto health = bt_runtime_->get_health();

    // Whether the engine is currently running (qualifies execution_reason).
    bool active = bt_runtime_->is_running();

    // Neutral, engine-agnostic view (Phase 1 #114). Sourced from the
    // IAutomationEngine seam.
    const automation::AutomationStatusView view = bt_runtime_->status();

    nlohmann::json automation_version = nullptr;
    if (!view.version.engine_kind.empty()) {
        automation_version = {{"engine_kind", view.version.engine_kind},
                              {"id", view.version.id},
                              {"digest", view.version.digest},
                              {"digest_scope", view.version.digest_scope}};
    }

    // Optional coarse reason qualifying execution_status (enumerated, not free text).
    std::string execution_reason;
    if (view.version.engine_kind.empty()) {
        execution_reason = "no_definition";
    } else if (!active) {
        execution_reason = "stopped";
    } else if (mode_manager_->current_mode() != automation::RuntimeMode::AUTO) {
        execution_reason = "mode_gate";
    } else if (view.status == automation::AutomationStatus::Failed) {
        execution_reason = "terminal_failure";
    } else if (view.status == automation::AutomationStatus::Blocked) {
        execution_reason = "waiting";
    }

    // The currently-open run, if any (single call — clang-tidy-safe optional use).
    nlohmann::json run_id_json = nullptr;
    if (run_journal_ != nullptr) {
        if (const auto open_id = run_journal_->open_run_id()) {
            run_id_json = *open_id;
        }
    }

    nlohmann::json response = {
        {"status", make_status(StatusCode::OK)},
        // Neutral, engine-agnostic fields (the forward contract). The deprecated
        // BT mirrors were removed in v0.1.26 (Phase 4 #117).
        {"execution_status", automation::to_string(view.status)},
        {"automation_version", automation_version},
        {"last_evaluation_at_epoch_ms", view.last_evaluation_at_epoch_ms},
        {"engine_diagnostics", view.engine_diagnostics},
        {"run_id", run_id_json},
        {"last_error", health.last_error.empty() ? nlohmann::json() : nlohmann::json(health.last_error)}};

    if (!execution_reason.empty()) {
        response["execution_reason"] = execution_reason;
    }

    send_json(res, StatusCode::OK, response);
#else
    nlohmann::json response = make_error_response(StatusCode::UNAVAILABLE, "Automation layer disabled at build time");
    send_json(res, StatusCode::UNAVAILABLE, response);
#endif
}

//=============================================================================
// GET /v0/providers/health
//=============================================================================
namespace {

// JSON for one provider-reported ADPP DeviceHealth entry (#185).
nlohmann::json reported_device_health_json(const health::ReportedDeviceHealth &rd) {
    nlohmann::json metrics = nlohmann::json::object();
    for (const auto &[key, value] : rd.metrics) {
        metrics[key] = value;
    }
    return {{"state", rd.state},
            {"message", rd.message},
            {"metrics", metrics},
            {"last_seen_epoch_ms", rd.last_seen_epoch_ms.has_value() ? nlohmann::json(rd.last_seen_epoch_ms.value())
                                                                     : nlohmann::json(nullptr)}};
}

}  // namespace

void HttpServer::handle_get_providers_health(const httplib::Request &, httplib::Response &res) {
    // Shared collector (#203): the same snapshot feeds the InfluxDB health
    // ingestion, so the HTTP and timeseries views cannot drift.
    auto snapshot = health::collect_providers_health(provider_registry_, registry_, state_cache_, supervisor_);

    nlohmann::json providers_json = nlohmann::json::array();

    for (const auto &ps : snapshot) {
        nlohmann::json devices_json = nlohmann::json::array();
        for (const auto &ds : ps.devices) {
            nlohmann::json device_json = {{"device_id", ds.device_id},       {"health", ds.health},
                                          {"last_poll_ms", ds.last_poll_ms}, {"staleness_ms", ds.staleness_ms},
                                          {"registered", ds.registered},     {"reported", nullptr}};
            if (ds.reported) {
                device_json["reported"] = reported_device_health_json(*ds.reported);
            }
            devices_json.push_back(std::move(device_json));
        }

        // supervision is always emitted as an object, never null.
        nlohmann::json supervision_json = {
            {"enabled", ps.supervision.enabled},
            {"attempt_count", ps.supervision.attempt_count},
            {"max_attempts", ps.supervision.max_attempts},
            {"crash_detected", ps.supervision.crash_detected},
            {"circuit_open", ps.supervision.circuit_open},
            {"next_restart_in_ms", ps.supervision.next_restart_in_ms.has_value()
                                       ? nlohmann::json(ps.supervision.next_restart_in_ms.value())
                                       : nlohmann::json(nullptr)}};

        nlohmann::json reported_provider_json = nullptr;
        if (ps.reported) {
            nlohmann::json provider_metrics = nlohmann::json::object();
            for (const auto &[key, value] : ps.reported->metrics) {
                provider_metrics[key] = value;
            }
            reported_provider_json = {
                {"state", ps.reported->state}, {"message", ps.reported->message}, {"metrics", provider_metrics}};
        }

        providers_json.push_back(
            {{"provider_id", ps.provider_id},
             {"state", ps.state},
             {"lifecycle_state", ps.lifecycle_state},
             {"last_seen_ago_ms",
              ps.last_seen_ago_ms.has_value() ? nlohmann::json(ps.last_seen_ago_ms.value()) : nlohmann::json(nullptr)},
             {"uptime_seconds", ps.uptime_seconds},
             {"device_count", ps.device_count},
             {"degraded", ps.degraded},
             {"failed_device_count", ps.failed_device_count},
             {"reported", reported_provider_json},
             {"supervision", supervision_json},
             {"devices", devices_json}});
    }

    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"providers", providers_json}};

    send_json(res, StatusCode::OK, response);
}

void HttpServer::handle_get_telemetry_status(const httplib::Request &, httplib::Response &res) {
    // Read-only view of the InfluxDB telemetry sink. The sink pointer is null when
    // telemetry is disabled in config; report that plainly rather than 404-ing.
    nlohmann::json response = {{"status", make_status(StatusCode::OK)}};

    if (telemetry_sink_ == nullptr) {
        response["enabled"] = false;
        response["running"] = false;
        response["connected"] = false;
        response["total_written"] = 0;
        response["total_failed"] = 0;
        response["pending_batch"] = 0;
        response["queue_depth"] = 0;
        response["dropped_events"] = 0;
        send_json(res, StatusCode::OK, response);
        return;
    }

    response["enabled"] = telemetry_sink_->enabled();
    response["running"] = telemetry_sink_->is_running();
    response["connected"] = telemetry_sink_->is_connected();
    response["total_written"] = telemetry_sink_->total_written();
    response["total_failed"] = telemetry_sink_->total_failed();
    response["pending_batch"] = telemetry_sink_->current_batch_size();
    response["queue_depth"] = telemetry_sink_->queue_depth();
    response["dropped_events"] = telemetry_sink_->dropped_events();
    // Backend coordinates for operator context. Never includes the API token.
    response["backend"] = {
        {"url", telemetry_sink_->url()}, {"org", telemetry_sink_->org()}, {"bucket", telemetry_sink_->bucket()}};

    send_json(res, StatusCode::OK, response);
}

}  // namespace http
}  // namespace anolis
