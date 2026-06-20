#include "http/auth.hpp"

#include <cstddef>

namespace anolis::http {

bool is_loopback(const std::string &remote_addr) {
    return remote_addr == "127.0.0.1" || remote_addr == "::1" || remote_addr == "::ffff:127.0.0.1" ||
           remote_addr.rfind("127.", 0) == 0;
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

bool authorize(bool auth_enabled, bool exempt_loopback, const std::string &expected_token,
               const std::string &remote_addr, const std::string &authorization_header) {
    if (!auth_enabled) {
        return true;
    }
    if (exempt_loopback && is_loopback(remote_addr)) {
        return true;
    }
    // A request can never be authorized against an empty secret.
    if (expected_token.empty()) {
        return false;
    }
    constexpr std::string_view kPrefix = "Bearer ";
    if (authorization_header.size() <= kPrefix.size() ||
        authorization_header.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    std::string_view presented(authorization_header);
    presented.remove_prefix(kPrefix.size());
    return constant_time_equals(presented, expected_token);
}

}  // namespace anolis::http
