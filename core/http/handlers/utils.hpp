#pragma once

#include <httplib.h>

#include <nlohmann/json.hpp>
#include <string>

#include "../errors.hpp"

namespace anolis {
namespace http {

// Helper: Parse provider_id and device_id from regex matches
inline bool parse_path_params(const httplib::Request &req, std::string &provider_id, std::string &device_id) {
    if (req.matches.size() >= 3) {
        provider_id = req.matches[1].str();
        device_id = req.matches[2].str();
        return true;
    }
    return false;
}

/**
 * @brief Serialize JSON for the wire without throwing on invalid UTF-8.
 *
 * Bodies and event frames carry provider-, engine- and client-supplied text
 * (device ids, labels, health and fault messages, a request path). By default
 * dump() throws type_error.316 on a byte sequence that is not valid UTF-8; this
 * writes U+FFFD for each such byte instead. Every HTTP-layer serialization goes
 * through here: inside a route handler a throw would turn the whole response
 * into a 500, and in the error handler or an SSE content provider, which
 * cpp-httplib calls outside its routing try/catch, it would abort the runtime
 * (#594).
 */
inline std::string dump_json(const nlohmann::json &body) {
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Helper: Send JSON response
inline void send_json(httplib::Response &res, StatusCode code, const nlohmann::json &body) {
    res.status = status_code_to_http(code);
    // One provider emitting a bad byte must not blank an operator's whole
    // health view: see dump_json().
    res.set_content(dump_json(body), "application/json");
}

}  // namespace http
}  // namespace anolis
