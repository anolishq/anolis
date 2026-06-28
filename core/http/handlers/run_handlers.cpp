#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "anolis_build_config.hpp"
#if ANOLIS_ENABLE_AUTOMATION
#include "../../automation/bt_runtime.hpp"
#endif
#include "../../runs/run_journal.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../server.hpp"
#include "utils.hpp"

namespace anolis {
namespace http {

namespace {

std::vector<std::string> parse_str_array(const nlohmann::json &j, const char *key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array()) {
        for (const auto &e : j[key]) {
            if (e.is_string()) out.push_back(e.get<std::string>());
        }
    }
    return out;
}

runs::TagScope parse_tag_scope(const nlohmann::json &j) {
    runs::TagScope scope;
    scope.provider_ids = parse_str_array(j, "provider_ids");
    scope.device_ids = parse_str_array(j, "device_ids");
    scope.signal_ids = parse_str_array(j, "signal_ids");
    return scope;
}

nlohmann::json run_envelope(const runs::Run &run) {
    return {{"status", make_status(StatusCode::OK)}, {"run", runs::to_json(run)}};
}

}  // namespace

void HttpServer::handle_post_runs(const httplib::Request &req, httplib::Response &res) {
    if (run_journal_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Run registry not enabled"));
        return;
    }

    nlohmann::json body = nlohmann::json::object();
    if (!req.body.empty()) {
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception &e) {
            send_json(res, StatusCode::INVALID_ARGUMENT,
                      make_error_response(StatusCode::INVALID_ARGUMENT, std::string("Invalid JSON: ") + e.what()));
            return;
        }
    }

    runs::RunOpenSpec spec;
    if (body.contains("experiment_label") && body["experiment_label"].is_string()) {
        spec.experiment_label = body["experiment_label"].get<std::string>();
    }
    if (body.contains("params") && body["params"].is_object()) {
        spec.params = body["params"];
    }
    if (body.contains("tag_scope") && body["tag_scope"].is_object()) {
        spec.tag_scope = parse_tag_scope(body["tag_scope"]);
    }

    // Capture the automation version at open (nullopt => a manual-only run).
    std::optional<runs::AutomationVersionRecord> version;
#if ANOLIS_ENABLE_AUTOMATION
    if (bt_runtime_ != nullptr) {
        const auto v = bt_runtime_->status().version;
        if (!v.engine_kind.empty()) {
            version = runs::AutomationVersionRecord{v.engine_kind, v.id, v.digest, v.digest_scope};
        }
    }
#endif

    auto result = run_journal_->open(spec, std::move(version));
    if (!result.ok) {
        // A run is already open (one open run per runtime) — or a write failure.
        send_json(res, StatusCode::FAILED_PRECONDITION,
                  make_error_response(StatusCode::FAILED_PRECONDITION, result.error));
        return;
    }
    send_json(res, StatusCode::OK, run_envelope(result.run));
}

void HttpServer::handle_post_run_close(const httplib::Request &req, httplib::Response &res) {
    if (run_journal_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Run registry not enabled"));
        return;
    }
    const std::string run_id = req.matches.size() > 1 ? req.matches[1].str() : "";

    runs::CloseReason reason = runs::CloseReason::OperatorStop;
    if (!req.body.empty()) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception &e) {
            send_json(res, StatusCode::INVALID_ARGUMENT,
                      make_error_response(StatusCode::INVALID_ARGUMENT, std::string("Invalid JSON: ") + e.what()));
            return;
        }
        if (body.contains("close_reason") && body["close_reason"].is_string()) {
            auto parsed = runs::close_reason_from_string(body["close_reason"].get<std::string>());
            if (!parsed) {
                send_json(
                    res, StatusCode::INVALID_ARGUMENT,
                    make_error_response(StatusCode::INVALID_ARGUMENT,
                                        "Invalid close_reason (expected operator_stop|completed|failed|superseded)"));
                return;
            }
            reason = *parsed;
        }
    }

    auto result = run_journal_->close(run_id, reason);
    if (!result.ok || !result.run.has_value()) {
        send_json(res, StatusCode::NOT_FOUND, make_error_response(StatusCode::NOT_FOUND, result.error));
        return;
    }
    send_json(res, StatusCode::OK, run_envelope(*result.run));
}

void HttpServer::handle_get_runs(const httplib::Request &req, httplib::Response &res) {
    if (run_journal_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Run registry not enabled"));
        return;
    }
    // Parse query params without exceptions (invalid input keeps the default).
    auto parse_size = [](const std::string &s, size_t fallback) -> size_t {
        size_t value = 0;
        const char *begin = s.data();
        const char *end = begin + s.size();
        auto [ptr, errc] = std::from_chars(begin, end, value);
        return (errc == std::errc() && ptr == end) ? value : fallback;
    };
    size_t limit = 50;
    size_t offset = 0;
    if (req.has_param("limit")) {
        limit = std::min<size_t>(parse_size(req.get_param_value("limit"), 50), 200);
    }
    if (req.has_param("offset")) {
        offset = parse_size(req.get_param_value("offset"), 0);
    }

    auto runs_page = run_journal_->list(limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &run : runs_page) {
        arr.push_back(runs::to_json(run));
    }
    nlohmann::json response = {{"status", make_status(StatusCode::OK)}, {"runs", arr}, {"count", arr.size()}};
    send_json(res, StatusCode::OK, response);
}

void HttpServer::handle_get_run(const httplib::Request &req, httplib::Response &res) {
    if (run_journal_ == nullptr) {
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Run registry not enabled"));
        return;
    }
    const std::string run_id = req.matches.size() > 1 ? req.matches[1].str() : "";
    auto run = run_journal_->get(run_id);
    if (!run) {
        send_json(res, StatusCode::NOT_FOUND, make_error_response(StatusCode::NOT_FOUND, "Unknown run: " + run_id));
        return;
    }
    send_json(res, StatusCode::OK, run_envelope(*run));
}

}  // namespace http
}  // namespace anolis
