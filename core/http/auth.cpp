#include "http/auth.hpp"

#include <cstddef>

namespace anolis::http {

bool is_loopback(const std::string &remote_addr) {
    return remote_addr == "127.0.0.1" || remote_addr == "::1" || remote_addr == "::ffff:127.0.0.1" ||
           remote_addr.rfind("127.", 0) == 0;
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    // Walk b's length whatever a's is, and fold a length mismatch into diff,
    // so the time depends on b (the expected token) only (#522).
    std::size_t diff = a.size() ^ b.size();
    for (std::size_t i = 0; i < b.size(); ++i) {
        const unsigned char ai = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        diff |= ai ^ static_cast<unsigned char>(b[i]);
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
