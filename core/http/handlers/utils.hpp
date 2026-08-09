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

// Helper: Send JSON response
inline void send_json(httplib::Response &res, StatusCode code, const nlohmann::json &body) {
    res.status = status_code_to_http(code);
    // Replace, don't throw, on invalid UTF-8. Responses carry provider-supplied
    // strings (device ids, labels, descriptor identity, health messages), and by
    // default dump() throws type_error.316 on a malformed byte sequence — which
    // the server turns into a 500 for the *entire* response. One provider
    // emitting a bad byte must not blank an operator's whole health view.
    res.set_content(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
}

}  // namespace http
}  // namespace anolis
