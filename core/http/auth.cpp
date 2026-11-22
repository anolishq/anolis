#include "http/auth.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cstddef>

namespace anolis::http {

bool is_loopback(const std::string &remote_addr) {
    return remote_addr == "127.0.0.1" || remote_addr == "::1" || remote_addr == "::ffff:127.0.0.1" ||
           remote_addr.rfind("127.", 0) == 0;
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    // Compare SHA-256 digests, always 32 bytes, with OpenSSL's constant-time
    // compare. Hashing a takes time that depends on a alone, and hashing b
    // on b alone, so nothing about the expected token (b) shows in how long a
    // presented one (a) takes to refuse, its length included (#522). A
    // byte-wise walk branched on a's length every iteration (review of #879).
    std::array<unsigned char, 32> da{};
    std::array<unsigned char, 32> db{};
    unsigned int la = 0;
    unsigned int lb = 0;
    const bool hashed = EVP_Digest(a.data(), a.size(), da.data(), &la, EVP_sha256(), nullptr) == 1 &&
                        EVP_Digest(b.data(), b.size(), db.data(), &lb, EVP_sha256(), nullptr) == 1;
    if (!hashed || la != da.size() || lb != db.size()) {
        return false;  // cannot happen with SHA-256; refuse rather than compare garbage
    }
    return CRYPTO_memcmp(da.data(), db.data(), da.size()) == 0;
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
    // The scheme is case-insensitive and one or more spaces separate it from
    // the token (RFC 7235 2.1, RFC 6750 2.1); the token is compared exactly (#523).
    constexpr std::string_view kScheme = "bearer";
    std::string_view presented(authorization_header);
    if (presented.size() <= kScheme.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(presented[i])) != kScheme[i]) {
            return false;
        }
    }
    presented.remove_prefix(kScheme.size());
    const std::size_t token_start = presented.find_first_not_of(' ');
    if (token_start == 0 || token_start == std::string_view::npos) {
        return false;
    }
    presented.remove_prefix(token_start);
    return constant_time_equals(presented, expected_token);
}

}  // namespace anolis::http
