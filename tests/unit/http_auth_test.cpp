#include <gtest/gtest.h>

#include <chrono>
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

// ---- loopback exemption: off by default, opt-in with a WARN -------------

namespace {
const char *const kFakeToken = "fake-test-token-not-a-secret";

// Load a config whose `http:` block also carries @p http_lines; return what the
// loader logged (and any error) in @p log.
bool load_http_config(const std::string &http_lines, RuntimeConfig &config, std::string &log) {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("anolis_http_exempt_test-" + std::to_string(nonce));
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
            << http_lines;
    }
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    std::string error;
    const bool ok = load_config(cfg.string(), config, error);
    std::cerr.rdbuf(old);
    fs::remove_all(dir);
    log = captured.str() + error;
    return ok;
}

bool authorize_with(const RuntimeConfig &c, const std::string &remote, const std::string &header) {
    return authorize(c.http.auth_enabled, c.http.auth_exempt_loopback, c.http.auth_token, remote, header);
}

std::string auth_lines(bool exempt) {
    std::string lines = "  auth_enabled: true\n";
    if (exempt) {
        lines += "  auth_exempt_loopback: true\n";
    }
    return lines + "  auth_token: " + kFakeToken + "\n";
}
}  // namespace

TEST(HttpLoopbackExemption, DefaultIsOff) { EXPECT_FALSE(HttpConfig{}.auth_exempt_loopback); }

TEST(HttpLoopbackExemption, AuthEnabledLoopbackNeedsTokenByDefault) {
    RuntimeConfig c;
    std::string log;
    ASSERT_TRUE(load_http_config(auth_lines(false), c, log)) << log;
    EXPECT_FALSE(c.http.auth_exempt_loopback);
    EXPECT_FALSE(authorize_with(c, "127.0.0.1", ""));
    EXPECT_FALSE(authorize_with(c, "::1", ""));
    EXPECT_FALSE(authorize_with(c, "127.0.0.1", "Bearer wrong"));
    EXPECT_TRUE(authorize_with(c, "127.0.0.1", std::string("Bearer ") + kFakeToken));
}

TEST(HttpLoopbackExemption, AuthDisabledStillAllowsEveryRequest) {
    RuntimeConfig c;
    std::string log;
    ASSERT_TRUE(load_http_config("", c, log)) << log;
    EXPECT_FALSE(c.http.auth_enabled);
    EXPECT_TRUE(authorize_with(c, "127.0.0.1", ""));
    EXPECT_TRUE(authorize_with(c, "10.0.0.5", ""));
}

TEST(HttpLoopbackExemption, ExplicitTrueStillExemptsLoopback) {
    RuntimeConfig c;
    std::string log;
    ASSERT_TRUE(load_http_config(auth_lines(true), c, log)) << log;
    EXPECT_TRUE(c.http.auth_exempt_loopback);
    EXPECT_TRUE(authorize_with(c, "127.0.0.1", ""));
    EXPECT_FALSE(authorize_with(c, "10.0.0.5", ""));
}

TEST(HttpLoopbackExemption, ExplicitTrueWithAuthLogsWarn) {
    RuntimeConfig c;
    std::string log;
    ASSERT_TRUE(load_http_config(auth_lines(true), c, log)) << log;
    EXPECT_NE(log.find("WARN"), std::string::npos) << log;
    EXPECT_NE(log.find("auth_exempt_loopback"), std::string::npos) << log;
    EXPECT_EQ(log.find(kFakeToken), std::string::npos) << log;
}

TEST(HttpLoopbackExemption, NoWarnWhenExemptionOffOrAuthDisabled) {
    RuntimeConfig on;
    std::string log;
    ASSERT_TRUE(load_http_config(auth_lines(false), on, log)) << log;
    EXPECT_EQ(log.find("auth_exempt_loopback"), std::string::npos) << log;

    RuntimeConfig off;
    ASSERT_TRUE(load_http_config("  auth_exempt_loopback: true\n", off, log)) << log;
    EXPECT_EQ(log.find("auth_exempt_loopback"), std::string::npos) << log;
}

// ---- redaction: the auth token must never reach the logs ----------------

TEST(HttpAuthRedaction, AuthTokenNotLogged) {
    namespace fs = std::filesystem;
    const std::string kToken = "SUPERSECRET_TOKEN_DO_NOT_LOG_4d9f";
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("anolis_http_auth_redaction_test-" + std::to_string(nonce));
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
