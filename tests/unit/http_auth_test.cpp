#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "http/auth.hpp"
#include "runtime/config.hpp"

using anolis::http::authorize;
using anolis::http::constant_time_equals;
using anolis::http::is_loopback;
using namespace anolis::runtime;

// ---- authorize() — the request-level auth decision ----------------------

TEST(HttpAuth, DisabledAllowsEverything) {
    // auth_enabled=false -> all requests pass, even from the network with no token.
    EXPECT_TRUE(authorize(false, true, "secret", "10.0.0.5", ""));
    EXPECT_TRUE(authorize(false, false, "secret", "10.0.0.5", "Bearer wrong"));
}

TEST(HttpAuth, LoopbackExemptWhenConfigured) {
    EXPECT_TRUE(authorize(true, true, "secret", "127.0.0.1", ""));
    EXPECT_TRUE(authorize(true, true, "secret", "::1", ""));
    EXPECT_TRUE(authorize(true, true, "secret", "::ffff:127.0.0.1", ""));
}

TEST(HttpAuth, LoopbackNotExemptWhenDisabled) {
    // exempt_loopback=false -> even localhost must present the token.
    EXPECT_FALSE(authorize(true, false, "secret", "127.0.0.1", ""));
    EXPECT_TRUE(authorize(true, false, "secret", "127.0.0.1", "Bearer secret"));
}

TEST(HttpAuth, NetworkRequiresValidBearerToken) {
    EXPECT_TRUE(authorize(true, true, "secret", "10.0.0.5", "Bearer secret"));
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", "Bearer wrong"));
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", ""));
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", "secret"));          // missing scheme
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", "Basic secret"));    // wrong scheme
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", "Bearer "));         // empty token
    EXPECT_FALSE(authorize(true, true, "secret", "10.0.0.5", "Bearer secretx"));  // prefix match only
}

TEST(HttpAuth, EmptyExpectedTokenNeverAuthorizesNetwork) {
    // Misconfiguration guard: an empty secret must not accept any network token.
    EXPECT_FALSE(authorize(true, true, "", "10.0.0.5", "Bearer "));
    EXPECT_FALSE(authorize(true, true, "", "10.0.0.5", "Bearer anything"));
    // ...but loopback is still exempt if configured.
    EXPECT_TRUE(authorize(true, true, "", "127.0.0.1", ""));
}

TEST(HttpAuth, IsLoopback) {
    EXPECT_TRUE(is_loopback("127.0.0.1"));
    EXPECT_TRUE(is_loopback("127.1.2.3"));
    EXPECT_TRUE(is_loopback("::1"));
    EXPECT_FALSE(is_loopback("0.0.0.0"));
    EXPECT_FALSE(is_loopback("10.0.0.1"));
    EXPECT_FALSE(is_loopback("192.168.1.10"));
    EXPECT_FALSE(is_loopback(""));
}

TEST(HttpAuth, ConstantTimeEquals) {
    EXPECT_TRUE(constant_time_equals("abc123", "abc123"));
    EXPECT_FALSE(constant_time_equals("abc123", "abc124"));
    EXPECT_FALSE(constant_time_equals("abc", "abcd"));  // differing length
    EXPECT_TRUE(constant_time_equals("", ""));
}

// ---- bind-safety gate in validate_config --------------------------------

namespace {
// A config that is valid except for whatever the test varies on `http`.
RuntimeConfig base_valid_config() {
    RuntimeConfig c;
    anolis::provider::ProviderConfig p;
    p.id = "test0";
    p.command = "./provider-test";
    c.providers.push_back(p);
    return c;
}
}  // namespace

TEST(HttpAuthBindGate, LoopbackWithoutAuthIsAllowed) {
    RuntimeConfig c = base_valid_config();
    c.http.bind = "127.0.0.1";
    c.http.auth_enabled = false;
    std::string error;
    EXPECT_TRUE(validate_config(c, error)) << error;
}

TEST(HttpAuthBindGate, NonLoopbackWithoutAuthIsRejected) {
    RuntimeConfig c = base_valid_config();
    c.http.bind = "0.0.0.0";
    c.http.auth_enabled = false;
    c.http.allow_insecure_bind = false;
    std::string error;
    EXPECT_FALSE(validate_config(c, error));
    EXPECT_NE(error.find("non-loopback"), std::string::npos) << error;
}

TEST(HttpAuthBindGate, NonLoopbackWithAuthIsAllowed) {
    RuntimeConfig c = base_valid_config();
    c.http.bind = "0.0.0.0";
    c.http.auth_enabled = true;
    c.http.auth_token = "secret";
    std::string error;
    EXPECT_TRUE(validate_config(c, error)) << error;
}

TEST(HttpAuthBindGate, NonLoopbackInsecureOverrideIsAllowed) {
    RuntimeConfig c = base_valid_config();
    c.http.bind = "0.0.0.0";
    c.http.auth_enabled = false;
    c.http.allow_insecure_bind = true;
    std::string error;
    EXPECT_TRUE(validate_config(c, error)) << error;
}

TEST(HttpAuthBindGate, AuthEnabledWithoutTokenIsRejected) {
    RuntimeConfig c = base_valid_config();
    c.http.bind = "127.0.0.1";
    c.http.auth_enabled = true;
    c.http.auth_token = "";
    std::string error;
    EXPECT_FALSE(validate_config(c, error));
    EXPECT_NE(error.find("auth_token"), std::string::npos) << error;
}

// ---- TLS config validation (both-or-neither) ----------------------------

TEST(HttpTls, BothCertAndKeyIsValid) {
    RuntimeConfig c = base_valid_config();
    c.http.tls_cert_path = "/etc/anolis/cert.pem";
    c.http.tls_key_path = "/etc/anolis/key.pem";
    std::string error;
    EXPECT_TRUE(validate_config(c, error)) << error;
}

TEST(HttpTls, CertWithoutKeyIsRejected) {
    RuntimeConfig c = base_valid_config();
    c.http.tls_cert_path = "/etc/anolis/cert.pem";
    c.http.tls_key_path = "";
    std::string error;
    EXPECT_FALSE(validate_config(c, error));
    EXPECT_NE(error.find("tls_"), std::string::npos) << error;
}

TEST(HttpTls, KeyWithoutCertIsRejected) {
    RuntimeConfig c = base_valid_config();
    c.http.tls_cert_path = "";
    c.http.tls_key_path = "/etc/anolis/key.pem";
    std::string error;
    EXPECT_FALSE(validate_config(c, error));
    EXPECT_NE(error.find("tls_"), std::string::npos) << error;
}

TEST(HttpTls, NeitherIsValid) {
    RuntimeConfig c = base_valid_config();
    std::string error;
    EXPECT_TRUE(validate_config(c, error)) << error;
}

// ---- redaction: the auth token must never reach the logs ----------------

TEST(HttpAuthRedaction, AuthTokenNotLogged) {
    namespace fs = std::filesystem;
    const std::string kToken = "SUPERSECRET_TOKEN_DO_NOT_LOG_4d9f";
    fs::path dir = fs::temp_directory_path() / "anolis_http_auth_redaction_test";
    fs::create_directories(dir);
    fs::path cfg = dir / "runtime.yaml";
    {
        std::ofstream out(cfg);
        out << "providers:\n"
            << "  - id: test0\n"
            << "    command: ./provider-test\n"
            << "http:\n"
            << "  enabled: true\n"
            << "  bind: 127.0.0.1\n"
            << "  auth_enabled: true\n"
            << "  auth_token: " << kToken << "\n";
    }

    // Capture everything the config loader logs (Logger writes to std::cerr).
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    RuntimeConfig config;
    std::string error;
    const bool ok = load_config(cfg.string(), config, error);
    std::cerr.rdbuf(old);

    fs::remove_all(dir);

    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(config.http.auth_token, kToken);  // parsed correctly...
    EXPECT_EQ(captured.str().find(kToken), std::string::npos) << "auth token leaked into logs:\n"
                                                              << captured.str();  // ...but never logged.
}
