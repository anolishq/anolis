/**
 * @file http_handlers_test.cpp
 * @brief Unit tests for HTTP server handlers
 *
 * Tests all HTTP endpoints with mocked dependencies to ensure:
 * - Correct JSON encoding/decoding
 * - Proper error responses (404, 400, 503)
 * - Path parameter parsing
 * - Provider availability checks
 * - CORS header inclusion
 *
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

#include "control/call_router.hpp"
#include "http/server.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "provider/provider_supervisor.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"
#include "state/state_cache.hpp"

// HttpHandlersTest disabled under ThreadSanitizer due to cpp-httplib incompatibility.
// The library's internal threading triggers TSAN segfaults during server initialization.
// All concurrency tests (Registry, StateCache, EventEmitter) pass - the issue is specific
// to httplib's listen/bind threading model, not anolis code.
//
// This is acceptable because:
// - These tests validate HTTP endpoint functionality, not concurrency
// - The anolis HTTP code itself is straightforward without complex threading
// - The concurrency safety of kernel components is thoroughly tested elsewhere
#if defined(__SANITIZE_THREAD__)
// GCC automatically defines __SANITIZE_THREAD__ when -fsanitize=thread is used
#define ANOLIS_SKIP_HTTP_TESTS 1
#elif defined(__has_feature)
// Clang requires checking __has_feature(thread_sanitizer)
#if __has_feature(thread_sanitizer)
#define ANOLIS_SKIP_HTTP_TESTS 1
#else
#define ANOLIS_SKIP_HTTP_TESTS 0
#endif
#else
#define ANOLIS_SKIP_HTTP_TESTS 0
#endif

#if !ANOLIS_SKIP_HTTP_TESTS

using namespace anolis;
using namespace anolis::http;
using namespace testing;
using namespace anolis::tests;

// Protobuf type aliases
using Device = anolis::deviceprovider::v1::Device;
using DescribeDeviceResponse = anolis::deviceprovider::v1::DescribeDeviceResponse;
using ReadSignalsResponse = anolis::deviceprovider::v1::ReadSignalsResponse;

/**
 * @brief Test fixture for HTTP handler tests
 *
 * Creates a real HttpServer with mocked provider dependencies.
 * Uses a dedicated test port (9999) to avoid conflicts.
 */
class HttpHandlersTest : public Test {
protected:
    void SetUp() override {
        // Create kernel components
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);

        // Create mock provider
        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "test_provider";

        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));

        provider_registry = std::make_unique<provider::ProviderRegistry>();
        provider_registry->add_provider("test_provider", mock_provider);

        // Configure HTTP server for testing
        runtime::HttpConfig http_config;
        http_config.enabled = true;
        http_config.bind = "127.0.0.1";
        http_config.port = 9999;                   // Fixed test port
        http_config.cors_allowed_origins = {"*"};  // Allow CORS for testing

        // Create HTTP server
        HttpServerDependencies dependencies(*registry, *state_cache, *call_router, *provider_registry);
        server = std::make_unique<HttpServer>(http_config, 100, std::move(dependencies));

        // Start server
        std::string error;
        ASSERT_TRUE(server->start(error)) << "Failed to start HTTP server: " << error;

        // Give server time to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Create HTTP client
        client = std::make_unique<httplib::Client>("http://127.0.0.1:9999");
        client->set_connection_timeout(1, 0);  // 1 second timeout
    }

    void TearDown() override {
        client.reset();
        server->stop();
        server.reset();
    }

    /**
     * @brief Register a mock device with the registry
     */
    void RegisterMockDevice(const std::string& device_id = "test_device") {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([device_id](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id(device_id);
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device(device_id, _))
            .WillOnce(Invoke([device_id](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id(device_id);

                auto* caps = response.mutable_capabilities();

                // Add a signal (using string signal_id)
                auto* signal = caps->add_signals();
                signal->set_signal_id("1");  // Signal ID as string
                signal->set_name("temperature");
                signal->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);

                // Add a function
                auto* fn = caps->add_functions();
                fn->set_name("reset");
                fn->set_function_id(100);

                return true;
            }));

        registry->discover_provider("test_provider", *mock_provider);
    }

    void RegisterMockDeviceWithUint64ArgRange(const std::string& device_id = "test_device") {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([device_id](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id(device_id);
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device(device_id, _))
            .WillOnce(Invoke([device_id](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id(device_id);

                auto* caps = response.mutable_capabilities();

                auto* fn = caps->add_functions();
                fn->set_name("set_counter");
                fn->set_function_id(200);

                auto* arg = fn->add_args();
                arg->set_name("counter");
                arg->set_type(anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
                arg->set_required(true);
                arg->set_min_uint64((std::numeric_limits<uint64_t>::max() / 2) + 1);
                arg->set_max_uint64(std::numeric_limits<uint64_t>::max());

                return true;
            }));

        registry->discover_provider("test_provider", *mock_provider);
    }

    /**
     * @brief Populate state cache with test data
     *
     * Simplified version - directly calls read_signals on provider
     */
    void PopulateStateCache(const std::string& device_id = "test_device") {
        EXPECT_CALL(*mock_provider, read_signals(device_id, _, _))
            .WillOnce(Invoke([](const std::string&, const std::vector<std::string>&, ReadSignalsResponse& response) {
                auto* value = response.add_values();
                value->set_signal_id("1");
                value->mutable_value()->set_double_value(23.5);
                value->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                return true;
            }));

        state_cache->poll_once(*provider_registry);
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
};

//=============================================================================
// Device Handler Tests
//=============================================================================

TEST_F(HttpHandlersTest, GetDevicesEmpty) {
    auto res = client->Get("/v0/devices");

    ASSERT_TRUE(res) << "Request failed";
    EXPECT_EQ(200, res->status);
    EXPECT_EQ("application/json", res->get_header_value("Content-Type"));

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_TRUE(json["devices"].is_array());
    EXPECT_EQ(0, json["devices"].size());
}

TEST_F(HttpHandlersTest, GetDevicesWithDevice) {
    RegisterMockDevice();

    auto res = client->Get("/v0/devices");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    ASSERT_EQ(1, json["devices"].size());

    auto& device = json["devices"][0];
    EXPECT_EQ("test_provider", device["provider_id"]);
    EXPECT_EQ("test_device", device["device_id"]);
    EXPECT_TRUE(device.contains("display_name"));
    EXPECT_TRUE(device.contains("type"));
}

TEST_F(HttpHandlersTest, GetDeviceCapabilitiesSuccess) {
    RegisterMockDevice();

    auto res = client->Get("/v0/devices/test_provider/test_device/capabilities");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_EQ("test_provider", json["provider_id"]);
    EXPECT_EQ("test_device", json["device_id"]);

    ASSERT_TRUE(json.contains("capabilities"));
    auto& caps = json["capabilities"];

    ASSERT_TRUE(caps.contains("signals"));
    ASSERT_EQ(1, caps["signals"].size());
    EXPECT_EQ("1", caps["signals"][0]["signal_id"]);

    ASSERT_TRUE(caps.contains("functions"));
    ASSERT_EQ(1, caps["functions"].size());
    EXPECT_EQ("reset", caps["functions"][0]["name"]);
    EXPECT_EQ(100, caps["functions"][0]["function_id"]);
}

TEST_F(HttpHandlersTest, GetDeviceCapabilitiesNotFound) {
    auto res = client->Get("/v0/devices/nonexistent_provider/nonexistent_device/capabilities");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
    EXPECT_TRUE(json["status"]["message"].get<std::string>().find("not found") != std::string::npos);
}

TEST_F(HttpHandlersTest, GetDeviceCapabilitiesPreservesUint64ArgRange) {
    RegisterMockDeviceWithUint64ArgRange();

    auto res = client->Get("/v0/devices/test_provider/test_device/capabilities");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    const auto json = nlohmann::json::parse(res->body);
    ASSERT_TRUE(json.contains("capabilities"));
    ASSERT_TRUE(json["capabilities"].contains("functions"));
    ASSERT_EQ(1, json["capabilities"]["functions"].size());

    const auto& args = json["capabilities"]["functions"][0]["args"];
    ASSERT_TRUE(args.contains("counter"));
    const auto& counter = args["counter"];

    ASSERT_TRUE(counter.contains("min"));
    ASSERT_TRUE(counter.contains("max"));
    EXPECT_TRUE(counter["min"].is_number_unsigned());
    EXPECT_TRUE(counter["max"].is_number_unsigned());
    EXPECT_EQ((std::numeric_limits<uint64_t>::max() / 2) + 1, counter["min"].get<uint64_t>());
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), counter["max"].get<uint64_t>());
}

//=============================================================================
// State Handler Tests
//=============================================================================

TEST_F(HttpHandlersTest, GetStateEmpty) {
    auto res = client->Get("/v0/state");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_TRUE(json.contains("generated_at_epoch_ms"));
    EXPECT_TRUE(json["devices"].is_array());
    EXPECT_EQ(0, json["devices"].size());
}

TEST_F(HttpHandlersTest, GetStateWithData) {
    RegisterMockDevice();
    ASSERT_TRUE(state_cache->initialize());
    // Note: Without poll configuration, state will be empty but request succeeds

    auto res = client->Get("/v0/state");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_TRUE(json.contains("generated_at_epoch_ms"));
    EXPECT_TRUE(json["devices"].is_array());
    // State cache without poll data returns empty devices array
}

TEST_F(HttpHandlersTest, GetDeviceStateSuccess) {
    RegisterMockDevice();
    ASSERT_TRUE(state_cache->initialize());
    // Note: Without poll configuration, device returns UNAVAILABLE quality

    auto res = client->Get("/v0/state/test_provider/test_device");

    ASSERT_TRUE(res);
    // Device exists but state is unavailable without polling
    EXPECT_EQ(503, res->status);  // Service unavailable (no state data yet)

    auto json = nlohmann::json::parse(res->body);
    // UNAVAILABLE status when no polling has occurred
    EXPECT_EQ("UNAVAILABLE", json["status"]["code"]);
}

TEST_F(HttpHandlersTest, GetDeviceStateWithSignalFilter) {
    RegisterMockDevice();
    ASSERT_TRUE(state_cache->initialize());

    // Query with signal_id filter - tests parameter parsing even without poll data
    auto res = client->Get("/v0/state/test_provider/test_device?signal_id=1");

    ASSERT_TRUE(res);
    // Service unavailable when no state data exists
    EXPECT_EQ(503, res->status);

    auto json = nlohmann::json::parse(res->body);
    // Either UNAVAILABLE or OK with empty/unavailable state is acceptable
    EXPECT_TRUE(json.contains("status"));
}

TEST_F(HttpHandlersTest, GetDeviceStateNotFound) {
    auto res = client->Get("/v0/state/nonexistent_provider/nonexistent_device");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
}

//=============================================================================
// Control Handler Tests (POST /v0/call)
//=============================================================================

TEST_F(HttpHandlersTest, PostCallSuccess) {
    RegisterMockDevice();

    // Expect call to be invoked
    EXPECT_CALL(*mock_provider, call("test_device", _, "reset", _, _)).WillOnce(Return(true));

    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 100},
                                   {"args", nlohmann::json::object()}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_EQ("test_provider", json["provider_id"]);
    EXPECT_EQ("test_device", json["device_id"]);
    EXPECT_EQ(100, json["function_id"]);
    EXPECT_TRUE(json["post_call_poll_triggered"].get<bool>());
}

TEST_F(HttpHandlersTest, PostCallInvalidJSON) {
    auto res = client->Post("/v0/call", "{invalid json", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(400, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("INVALID_ARGUMENT", json["status"]["code"]);
    EXPECT_TRUE(json["status"]["message"].get<std::string>().find("Invalid JSON") != std::string::npos);
}

TEST_F(HttpHandlersTest, PostCallMissingFields) {
    nlohmann::json request_body = {
        {"provider_id", "test_provider"}  // Missing device_id, function_id, args
    };

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(400, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("INVALID_ARGUMENT", json["status"]["code"]);
}

TEST_F(HttpHandlersTest, PostCallProviderNotFound) {
    nlohmann::json request_body = {{"provider_id", "nonexistent"},
                                   {"device_id", "test_device"},
                                   {"function_id", 100},
                                   {"args", nlohmann::json::object()}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
    EXPECT_TRUE(json["status"]["message"].get<std::string>().find("Provider not found") != std::string::npos);
}

TEST_F(HttpHandlersTest, PostCallProviderUnavailable) {
    RegisterMockDevice();

    // Override is_available to return false
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(false));

    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 100},
                                   {"args", nlohmann::json::object()}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(503, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("UNAVAILABLE", json["status"]["code"]);
}

TEST_F(HttpHandlersTest, PostCallDeviceNotFound) {
    RegisterMockDevice("test_device");

    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "wrong_device"},
                                   {"function_id", 100},
                                   {"args", nlohmann::json::object()}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
    EXPECT_TRUE(json["status"]["message"].get<std::string>().find("Device not found") != std::string::npos);
}

TEST_F(HttpHandlersTest, PostCallFunctionNotFound) {
    RegisterMockDevice();

    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 999},  // Not registered (only 100 exists)
                                   {"args", nlohmann::json::object()}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
    EXPECT_TRUE(json["status"]["message"].get<std::string>().find("Function ID not found") != std::string::npos);
}

TEST_F(HttpHandlersTest, GetProvidersHealthNullSupervisorShape) {
    // With nullptr supervisor, endpoint must still return a well-formed response.
    // supervision block is always present as an object (never null), containing
    // zeroed/disabled values.
    auto res = client->Get("/v0/providers/health");

    ASSERT_TRUE(res) << "Request failed";
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    ASSERT_TRUE(json["providers"].is_array());
    ASSERT_EQ(1u, json["providers"].size());

    const auto& provider = json["providers"][0];
    EXPECT_EQ("test_provider", provider["provider_id"]);
    EXPECT_EQ("RUNNING", provider["lifecycle_state"]);

    // supervision is always an object, never null — even when supervisor is nullptr
    ASSERT_TRUE(provider.contains("supervision"));
    ASSERT_TRUE(provider["supervision"].is_object());

    // Zeroed supervision when no supervisor
    EXPECT_FALSE(provider["supervision"]["enabled"].get<bool>());
    EXPECT_EQ(0, provider["supervision"]["attempt_count"].get<int>());
    EXPECT_TRUE(provider["supervision"]["next_restart_in_ms"].is_null());

    // last_seen_ago_ms present (null before first heartbeat — not the old epoch-zero placeholder)
    ASSERT_TRUE(provider.contains("last_seen_ago_ms"));
    EXPECT_TRUE(provider["last_seen_ago_ms"].is_null() || provider["last_seen_ago_ms"].is_number_integer());

    // devices array always present
    ASSERT_TRUE(provider.contains("devices"));
    EXPECT_TRUE(provider["devices"].is_array());
}

TEST_F(HttpHandlersTest, GetRuntimeStatus) {
    RegisterMockDevice();

    auto res = client->Get("/v0/runtime/status");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    EXPECT_EQ("MANUAL", json["mode"]);  // Default mode when mode_manager is null
    EXPECT_TRUE(json.contains("uptime_seconds"));
    EXPECT_EQ(100, json["polling_interval_ms"]);
    EXPECT_TRUE(json["providers"].is_array());
    EXPECT_EQ(1, json["providers"].size());

    const auto& provider = json["providers"][0];
    EXPECT_EQ("test_provider", provider["provider_id"]);
    ASSERT_TRUE(provider.contains("state"));
    ASSERT_TRUE(provider["state"].is_string());

    // Compatibility contract: runtime status remains coarse availability only.
    const std::string state = provider["state"].get<std::string>();
    EXPECT_TRUE(state == "AVAILABLE" || state == "UNAVAILABLE");
    EXPECT_EQ("AVAILABLE", state);
    EXPECT_EQ(1, provider["device_count"]);
    EXPECT_FALSE(provider.contains("supervision"));

    EXPECT_EQ(1, json["device_count"]);
}

TEST_F(HttpHandlersTest, GetRuntimeStatusUsesUnavailableWhenProviderUnavailable) {
    RegisterMockDevice();

    // Override fixture default to validate the unavailable branch.
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(false));

    auto res = client->Get("/v0/runtime/status");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    const auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    ASSERT_TRUE(json["providers"].is_array());
    ASSERT_EQ(1, json["providers"].size());

    const auto& provider = json["providers"][0];
    EXPECT_EQ("test_provider", provider["provider_id"]);
    EXPECT_EQ("UNAVAILABLE", provider["state"]);
    EXPECT_FALSE(provider.contains("supervision"));
}

//=============================================================================
// CORS Tests
//=============================================================================

TEST_F(HttpHandlersTest, CORSHeadersPresent) {
    // Send Origin header to trigger CORS middleware
    httplib::Headers headers = {{"Origin", "http://localhost:3000"}};
    auto res = client->Get("/v0/devices", headers);

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    // Verify CORS headers are present (server configured with CORS enabled and Origin header sent)
    EXPECT_TRUE(res->has_header("Access-Control-Allow-Origin"));
    EXPECT_EQ("*", res->get_header_value("Access-Control-Allow-Origin"));  // Wildcard configured in test
    EXPECT_TRUE(res->has_header("Content-Type"));
}

//=============================================================================
// Error Response Format Tests
//=============================================================================

TEST_F(HttpHandlersTest, ErrorResponseFormat) {
    // Trigger a 404 error
    auto res = client->Get("/v0/devices/nonexistent/nonexistent/capabilities");

    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    auto json = nlohmann::json::parse(res->body);

    // Verify error response structure
    ASSERT_TRUE(json.contains("status"));
    ASSERT_TRUE(json["status"].contains("code"));
    ASSERT_TRUE(json["status"].contains("message"));

    EXPECT_EQ("NOT_FOUND", json["status"]["code"]);
    EXPECT_FALSE(json["status"]["message"].get<std::string>().empty());
}

//=============================================================================
// Providers Health Handler Tests — with real ProviderSupervisor
//
// This fixture constructs HttpServer with a live ProviderSupervisor so that the
// supervision block in /v0/providers/health reflects actual registered policy data,
// not the zeroed fallback produced when supervisor_ is nullptr.
//=============================================================================

class HttpHandlersProvidersHealthTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);

        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "test_provider";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));

        provider_registry = std::make_unique<provider::ProviderRegistry>();
        provider_registry->add_provider("test_provider", mock_provider);

        // Real supervisor so the health endpoint returns actual policy data
        supervisor = std::make_unique<provider::ProviderSupervisor>();
        provider::RestartPolicyConfig policy;
        policy.enabled = true;
        policy.max_attempts = 3;
        policy.backoff_ms = {100, 500, 2000};
        policy.timeout_ms = 5000;
        supervisor->register_provider("test_provider", policy);

        runtime::HttpConfig http_config;
        http_config.enabled = true;
        http_config.bind = "127.0.0.1";
        http_config.port = 9998;  // Distinct port from HttpHandlersTest (9999)
        http_config.cors_allowed_origins = {"*"};

        HttpServerDependencies dependencies(*registry, *state_cache, *call_router, *provider_registry);
        dependencies.supervisor = supervisor.get();
        server = std::make_unique<HttpServer>(http_config, 100, std::move(dependencies));

        std::string error;
        ASSERT_TRUE(server->start(error)) << "Failed to start HTTP server: " << error;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        client = std::make_unique<httplib::Client>("http://127.0.0.1:9998");
        client->set_connection_timeout(1, 0);
    }

    void TearDown() override {
        client.reset();
        server->stop();
        server.reset();
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::unique_ptr<provider::ProviderSupervisor> supervisor;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
};

TEST_F(HttpHandlersProvidersHealthTest, ResponseSucceeds) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res) << "Request failed";
    EXPECT_EQ(200, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
    ASSERT_TRUE(json["providers"].is_array());
    ASSERT_EQ(1u, json["providers"].size());
    EXPECT_EQ("test_provider", json["providers"][0]["provider_id"]);
}

TEST_F(HttpHandlersProvidersHealthTest, SupervisionIsAlwaysAnObject) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];

    ASSERT_TRUE(provider.contains("supervision"));
    ASSERT_TRUE(provider["supervision"].is_object());
    EXPECT_EQ("AVAILABLE", provider["state"]);
    EXPECT_EQ("RUNNING", provider["lifecycle_state"]);
}

TEST_F(HttpHandlersProvidersHealthTest, SupervisionContainsAllRequiredKeys) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];
    const auto& sup = json["providers"][0]["supervision"];

    EXPECT_TRUE(provider.contains("lifecycle_state"));
    EXPECT_TRUE(provider["lifecycle_state"].is_string());

    EXPECT_TRUE(sup.contains("enabled"));
    EXPECT_TRUE(sup.contains("attempt_count"));
    EXPECT_TRUE(sup.contains("max_attempts"));
    EXPECT_TRUE(sup.contains("crash_detected"));
    EXPECT_TRUE(sup.contains("circuit_open"));
    EXPECT_TRUE(sup.contains("next_restart_in_ms"));

    EXPECT_TRUE(sup["enabled"].is_boolean());
    EXPECT_TRUE(sup["attempt_count"].is_number_integer());
    EXPECT_TRUE(sup["max_attempts"].is_number_integer());
    EXPECT_TRUE(sup["crash_detected"].is_boolean());
    EXPECT_TRUE(sup["circuit_open"].is_boolean());
    EXPECT_TRUE(sup["next_restart_in_ms"].is_null() || sup["next_restart_in_ms"].is_number_integer());
}

TEST_F(HttpHandlersProvidersHealthTest, SupervisionReflectsRegisteredPolicy) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& sup = json["providers"][0]["supervision"];

    EXPECT_TRUE(sup["enabled"].get<bool>());
    EXPECT_EQ(3, sup["max_attempts"].get<int>());
    EXPECT_EQ(0, sup["attempt_count"].get<int>());
    EXPECT_FALSE(sup["crash_detected"].get<bool>());
    EXPECT_FALSE(sup["circuit_open"].get<bool>());
    // Healthy, no crash history: next_restart_in_ms is null
    EXPECT_TRUE(sup["next_restart_in_ms"].is_null());
    EXPECT_EQ("RUNNING", json["providers"][0]["lifecycle_state"]);
}

TEST_F(HttpHandlersProvidersHealthTest, LifecycleStateRestartingWhenInBackoff) {
    ASSERT_TRUE(supervisor->mark_crash_detected("test_provider"));
    ASSERT_TRUE(supervisor->record_crash("test_provider"));
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(false));

    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];
    const auto& sup = provider["supervision"];

    EXPECT_EQ("UNAVAILABLE", provider["state"]);
    EXPECT_EQ("RESTARTING", provider["lifecycle_state"]);
    EXPECT_GE(sup["attempt_count"].get<int>(), 1);
    EXPECT_FALSE(sup["circuit_open"].get<bool>());
}

TEST_F(HttpHandlersProvidersHealthTest, LifecycleStateCircuitOpenWhenAttemptsExceeded) {
    ASSERT_TRUE(supervisor->mark_crash_detected("test_provider"));
    ASSERT_TRUE(supervisor->record_crash("test_provider"));
    ASSERT_TRUE(supervisor->record_crash("test_provider"));
    ASSERT_TRUE(supervisor->record_crash("test_provider"));
    EXPECT_FALSE(supervisor->record_crash("test_provider"));
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(false));

    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];
    const auto& sup = provider["supervision"];

    EXPECT_EQ("UNAVAILABLE", provider["state"]);
    EXPECT_EQ("CIRCUIT_OPEN", provider["lifecycle_state"]);
    EXPECT_TRUE(sup["circuit_open"].get<bool>());
    EXPECT_TRUE(sup["next_restart_in_ms"].is_null());
}

TEST_F(HttpHandlersProvidersHealthTest, LastSeenAgoMsIsPresentAndCorrectType) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];

    ASSERT_TRUE(provider.contains("last_seen_ago_ms"));
    // null before first heartbeat; integer once heartbeats have been recorded
    EXPECT_TRUE(provider["last_seen_ago_ms"].is_null() || provider["last_seen_ago_ms"].is_number_integer());
}

TEST_F(HttpHandlersProvidersHealthTest, UptimeSecondsAndDeviceCountPresent) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];

    ASSERT_TRUE(provider.contains("uptime_seconds"));
    EXPECT_TRUE(provider["uptime_seconds"].is_number_integer());
    ASSERT_TRUE(provider.contains("device_count"));
    EXPECT_TRUE(provider["device_count"].is_number_integer());
}

TEST_F(HttpHandlersProvidersHealthTest, DevicesArrayAlwaysPresent) {
    auto res = client->Get("/v0/providers/health");
    ASSERT_TRUE(res);
    auto json = nlohmann::json::parse(res->body);
    const auto& provider = json["providers"][0];

    ASSERT_TRUE(provider.contains("devices"));
    EXPECT_TRUE(provider["devices"].is_array());
}

//=============================================================================
// Capability JSON encoding bug tests (F1)
//=============================================================================
// encode_function_spec() uses a single has_bounds flag that emits both min and max
// keys together, or neither. A function with only min set gets a phantom max=0
// emitted. A function with only max set gets a phantom min=0 emitted.
//
// The test below directly exposes this by registering a function with only
// set_min_uint64(5) and asserting that "max" is absent from the JSON output.
// CURRENTLY FAILS because has_bounds = (5 != 0 || 0 != 0) = true causes both
// min=5 and max=0 to be emitted.

class HttpHandlersBoundsEncodingBugTest : public HttpHandlersTest {
protected:
    void RegisterDeviceWithMinOnlyUint64(const std::string& device_id = "test_device") {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([device_id](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id(device_id);
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device(device_id, _))
            .WillOnce(Invoke([device_id](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id(device_id);

                auto* caps = response.mutable_capabilities();
                auto* fn = caps->add_functions();
                fn->set_name("set_counter");
                fn->set_function_id(200);

                auto* arg = fn->add_args();
                arg->set_name("counter");
                arg->set_type(anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
                arg->set_required(true);
                arg->set_min_uint64(5);
                // intentionally no set_max_uint64() — only a lower bound

                return true;
            }));

        registry->discover_provider("test_provider", *mock_provider);
    }
};

// CURRENTLY FAILS: has_bounds single-flag emits phantom max=0 alongside min=5.
// After fix: only "min" key is present; "max" is absent.
TEST_F(HttpHandlersBoundsEncodingBugTest, MinOnlyUint64_JsonHasMinButNotMax) {
    RegisterDeviceWithMinOnlyUint64();

    auto res = client->Get("/v0/devices/test_provider/test_device/capabilities");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);

    const auto json = nlohmann::json::parse(res->body);
    ASSERT_TRUE(json.contains("capabilities"));
    ASSERT_TRUE(json["capabilities"].contains("functions"));
    ASSERT_EQ(1, json["capabilities"]["functions"].size());

    const auto& counter = json["capabilities"]["functions"][0]["args"]["counter"];

    ASSERT_TRUE(counter.contains("min"));
    EXPECT_FALSE(counter.contains("max"));  // no max was set — must not appear in JSON
    EXPECT_EQ(5ULL, counter["min"].get<uint64_t>());
}

//=============================================================================
// F2: Base64 validation tests (POST /v0/call with bytes argument)
//=============================================================================

// Registers a device with a single bytes-type function argument.
// Used by all F2 tests below.
void RegisterMockDeviceWithBytesArg(anolis::tests::MockProviderHandle& mock_provider,
                                    anolis::registry::DeviceRegistry& registry,
                                    const std::string& device_id = "test_device") {
    using Device = anolis::deviceprovider::v1::Device;
    using DescribeDeviceResponse = anolis::deviceprovider::v1::DescribeDeviceResponse;

    EXPECT_CALL(mock_provider, list_devices(_)).WillOnce(Invoke([device_id](std::vector<Device>& devices) {
        Device dev;
        dev.set_device_id(device_id);
        devices.push_back(dev);
        return true;
    }));

    EXPECT_CALL(mock_provider, describe_device(device_id, _))
        .WillOnce(Invoke([device_id](const std::string&, DescribeDeviceResponse& response) {
            auto* device = response.mutable_device();
            device->set_device_id(device_id);

            auto* caps = response.mutable_capabilities();
            auto* fn = caps->add_functions();
            fn->set_name("send_raw");
            fn->set_function_id(300);

            auto* arg = fn->add_args();
            arg->set_name("payload");
            arg->set_type(anolis::deviceprovider::v1::VALUE_TYPE_BYTES);
            arg->set_required(true);

            return true;
        }));

    registry.discover_provider("test_provider", mock_provider);
}

// Valid Base64 with standard padding must succeed (regression guard).
TEST_F(HttpHandlersTest, PostCallBytesArg_ValidBase64WithPadding_Succeeds) {
    RegisterMockDeviceWithBytesArg(*mock_provider, *registry);

    EXPECT_CALL(*mock_provider, call("test_device", _, "send_raw", _, _)).WillOnce(Return(true));

    // "hello" in Base64 = "aGVsbG8="
    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 300},
                                   {"args", {{"payload", {{"type", "bytes"}, {"base64", "aGVsbG8="}}}}}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
}

// Valid Base64 without padding must succeed (regression guard).
TEST_F(HttpHandlersTest, PostCallBytesArg_ValidBase64NoPadding_Succeeds) {
    RegisterMockDeviceWithBytesArg(*mock_provider, *registry);

    EXPECT_CALL(*mock_provider, call("test_device", _, "send_raw", _, _)).WillOnce(Return(true));

    // "hel" in Base64 = "aGVs" (no padding needed for 3-byte input)
    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 300},
                                   {"args", {{"payload", {{"type", "bytes"}, {"base64", "aGVs"}}}}}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("OK", json["status"]["code"]);
}

// Illegal characters mid-string must be rejected with 400. Currently FAILS (returns 200).
TEST_F(HttpHandlersTest, PostCallBytesArg_MalformedBase64MidString_Returns400) {
    RegisterMockDeviceWithBytesArg(*mock_provider, *registry);

    // "aGVs##bG8=" — the ## are not valid Base64 characters
    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 300},
                                   {"args", {{"payload", {{"type", "bytes"}, {"base64", "aGVs##bG8="}}}}}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(400, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("INVALID_ARGUMENT", json["status"]["code"]);
}

// Fully invalid Base64 must be rejected with 400. Currently FAILS (returns 200).
TEST_F(HttpHandlersTest, PostCallBytesArg_FullyInvalidBase64_Returns400) {
    RegisterMockDeviceWithBytesArg(*mock_provider, *registry);

    // "!!!" — no valid Base64 characters at all
    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 300},
                                   {"args", {{"payload", {{"type", "bytes"}, {"base64", "!!!"}}}}}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(400, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("INVALID_ARGUMENT", json["status"]["code"]);
}

// Space character mid-string must be rejected with 400. Currently FAILS (returns 200).
TEST_F(HttpHandlersTest, PostCallBytesArg_SpaceMidString_Returns400) {
    RegisterMockDeviceWithBytesArg(*mock_provider, *registry);

    // "aGVs bG8=" — space is not a valid Base64 character
    nlohmann::json request_body = {{"provider_id", "test_provider"},
                                   {"device_id", "test_device"},
                                   {"function_id", 300},
                                   {"args", {{"payload", {{"type", "bytes"}, {"base64", "aGVs bG8="}}}}}};

    auto res = client->Post("/v0/call", request_body.dump(), "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(400, res->status);
    auto json = nlohmann::json::parse(res->body);
    EXPECT_EQ("INVALID_ARGUMENT", json["status"]["code"]);
}

#else  // ANOLIS_SKIP_HTTP_TESTS
TEST(HttpHandlersTest, DISABLED_SkippedUnderThreadSanitizer) {
    // This test exists to document that HttpHandlersTest suite is disabled
    // under TSAN due to cpp-httplib incompatibility. All 19 HTTP handler
    // tests are skipped when building with -fsanitize=thread.
    GTEST_SKIP() << "HTTP handler tests disabled under ThreadSanitizer";
}

#endif  // !ANOLIS_SKIP_HTTP_TESTS
