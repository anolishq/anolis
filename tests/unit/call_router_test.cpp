#include "control/call_router.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "automation/mode_manager.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

class CallRouterTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        router = std::make_unique<control::CallRouter>(*registry, *state_cache);
        mode_manager = std::make_unique<automation::ModeManager>();
        provider_registry = std::make_unique<provider::ProviderRegistry>();

        // Default: Connect router to mode manager with NO blocking
        router->set_mode_manager(mode_manager.get(), "OVERRIDE");

        // Set mode to MANUAL for tests (IDLE is the safe default, but tests need to test functionality)
        std::string err;
        mode_manager->set_mode(automation::RuntimeMode::MANUAL, err);

        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));

        provider_registry->add_provider("sim0", mock_provider);
    }

    // ... existing RegisterMockDevice ...
    void RegisterMockDevice() {
        // Setup mock provider behavior for discovery
        // Note: The registry will call list_devices then describe_device
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("dev1");
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device("dev1", _))
            .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id("dev1");

                auto* caps = response.mutable_capabilities();
                auto* fn = caps->add_functions();
                fn->set_name("reset");
                fn->set_function_id(1);
                return true;
            }));

        registry->discover_provider("sim0", *mock_provider);
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> router;
    std::unique_ptr<automation::ModeManager> mode_manager;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
};

TEST_F(CallRouterTest, ExecuteCallSuccess) {
    RegisterMockDevice();

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    // Successful call
    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _)).WillOnce(Return(true));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_OK);
}

TEST_F(CallRouterTest, ExecuteCallSuccessByFunctionId) {
    RegisterMockDevice();

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_id = 1;  // Resolve by ID (HTTP-style path)

    EXPECT_CALL(*mock_provider, call("dev1", 1, "reset", _, _)).WillOnce(Return(true));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_OK);
}

TEST_F(CallRouterTest, ProviderUnavailable) {
    RegisterMockDevice();

    // Override is_available to false for the call check
    // Note: poll_once or other valid calls usually check this.
    // CallRouter checks it explicitly.
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(false));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE);
}

TEST_F(CallRouterTest, InvalidFunction) {
    RegisterMockDevice();  // Has "reset"

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "explode";  // Not registered

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND);
}

TEST_F(CallRouterTest, InvalidFunctionId) {
    RegisterMockDevice();  // Has function ID 1

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_id = 999;  // Not registered

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND);
    EXPECT_THAT(result.error_message, HasSubstr("Function ID not found"));
}

TEST_F(CallRouterTest, PreconditionFailure) {
    RegisterMockDevice();

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    // Setup CallResult response object (reference param) with status embedded inside it
    // Wait, mock_provider call signature: (const string&, uint32_t, const string&, const ValueMap&, CallResponse&)
    // The CallResponse is the 5th arg.

    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _))
        .WillOnce(Invoke([](const std::string&, uint32_t, const std::string&, const ValueMap&, CallResponse&) {
            // Note: CallResponse doesn't have status code in DPV0 if it fails?
            // Checking protocol.pb.h above, CallResponse has `results` and `operation_id`.
            // STATUS is defined in the top-level Response envelope
            // (anolis-protocol/proto/anolis/deviceprovider/v1/envelope.proto). However, the provider interface
            // (IProviderHandle) returns bool. If it returns false, the router checks last_status_code() of the provider
            // handle? Let's check IProviderHandle again.
            return false;  // Provider indicates failure
        }));

    // If call returns false, Router should check provider->last_status_code()
    EXPECT_CALL(*mock_provider, last_status_code())
        .WillRepeatedly(Return(anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION));

    // Router also retrieves error message
    EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(ReturnRef(mock_provider->_err));
    mock_provider->_err = "Precondition failed";

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION);
}

TEST_F(CallRouterTest, PolicyBlockInAuto) {
    RegisterMockDevice();

    // 1. Set policy to BLOCK
    router->set_mode_manager(mode_manager.get(), "BLOCK");

    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::AUTO, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";
    req.is_automated = false;  // Explicitly mark as manual call

    // Expect NO CALL to provider (manual calls blocked in AUTO mode)
    EXPECT_CALL(*mock_provider, call(_, _, _, _, _)).Times(0);

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION);
    EXPECT_THAT(result.error_message, HasSubstr("blocked in AUTO"));
}

TEST_F(CallRouterTest, AutomatedCallsBypassBlock) {
    RegisterMockDevice();

    // 1. Set policy to BLOCK
    router->set_mode_manager(mode_manager.get(), "BLOCK");

    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::AUTO, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";
    req.is_automated = true;  // Mark as automated (BT) call

    // Expect CALL to provider (automated calls bypass blocking)
    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _)).WillOnce(Return(true));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_OK);
}

TEST_F(CallRouterTest, PolicyOverrideInAuto) {
    RegisterMockDevice();

    // 1. Set policy to OVERRIDE (Explicitly allow)
    router->set_mode_manager(mode_manager.get(), "OVERRIDE");

    // 2. Set mode to AUTO
    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::AUTO, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";
    req.is_automated = false;  // Manual call, but with OVERRIDE policy

    // Expect CALL to provider (OVERRIDE policy allows manual calls)
    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _)).WillOnce(Return(true));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_TRUE(result.success);
}

TEST_F(CallRouterTest, IdleModeBlocksControlOperations) {
    RegisterMockDevice();

    // Transition to IDLE mode
    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::IDLE, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";
    req.is_automated = false;

    // Expect NO CALL to provider (control operations blocked in IDLE)
    EXPECT_CALL(*mock_provider, call(_, _, _, _, _)).Times(0);

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION);
    EXPECT_THAT(result.error_message, HasSubstr("blocked in IDLE"));
}

TEST_F(CallRouterTest, IdleModeBlocksAutomatedCalls) {
    RegisterMockDevice();

    // Transition to IDLE mode
    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::IDLE, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";
    req.is_automated = true;  // Even automated calls blocked in IDLE

    // Expect NO CALL to provider
    EXPECT_CALL(*mock_provider, call(_, _, _, _, _)).Times(0);

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION);
    EXPECT_THAT(result.error_message, HasSubstr("blocked in IDLE"));
}

TEST_F(CallRouterTest, ManualModeAllowsCalls) {
    RegisterMockDevice();

    // Start in MANUAL mode
    std::string err;
    ASSERT_TRUE(mode_manager->set_mode(automation::RuntimeMode::MANUAL, err));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    // Calls should succeed in MANUAL mode
    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _)).WillOnce(Return(true));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_TRUE(result.success);
}

TEST_F(CallRouterTest, InvalidArgumentPropagation) {
    RegisterMockDevice();

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    // Setup provider to return failure
    EXPECT_CALL(*mock_provider, call("dev1", _, "reset", _, _)).WillOnce(Return(false));

    // Consistently return the error info
    // (Note: we use WillRepeatedly to be safe if called multiple times or order varies slightly)
    EXPECT_CALL(*mock_provider, last_status_code())
        .WillRepeatedly(Return(anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT));

    mock_provider->_err = "Invalid voltage";
    EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(ReturnRef(mock_provider->_err));

    auto result = router->execute_call(req, *provider_registry);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.status_code, anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT);
    EXPECT_THAT(result.error_message, HasSubstr("Invalid voltage"));
}

// ============================================================================
// Validation Tests - Test argument validation
// ============================================================================

class CallRouterValidationTest : public Test {
protected:
    using ValueType = anolis::deviceprovider::v1::ValueType;
    using ArgSpec = anolis::deviceprovider::v1::ArgSpec;
    using Value = anolis::deviceprovider::v1::Value;

    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        router = std::make_unique<control::CallRouter>(*registry, *state_cache);

        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));

        providers["sim0"] = mock_provider;

        // Register device with function specs for validation testing
        RegisterValidationDevice();
    }

    void RegisterValidationDevice() {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("testdev");
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device("testdev", _))
            .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id("testdev");

                auto* caps = response.mutable_capabilities();

                // Function: set_temperature(value: double[0.0-100.0] required, unit: string optional)
                auto* fn1 = caps->add_functions();
                fn1->set_name("set_temperature");
                fn1->set_function_id(1);

                auto* arg1 = fn1->add_args();
                arg1->set_name("value");
                arg1->set_type(ValueType::VALUE_TYPE_DOUBLE);
                arg1->set_required(true);
                arg1->set_min_double(0.0);
                arg1->set_max_double(100.0);
                arg1->set_description("Temperature value");
                arg1->set_unit("C");

                auto* arg2 = fn1->add_args();
                arg2->set_name("unit");
                arg2->set_type(ValueType::VALUE_TYPE_STRING);
                arg2->set_required(false);
                arg2->set_description("Temperature unit");

                // Function: set_index(index: int64[1-10] required)
                auto* fn2 = caps->add_functions();
                fn2->set_name("set_index");
                fn2->set_function_id(2);

                auto* arg3 = fn2->add_args();
                arg3->set_name("index");
                arg3->set_type(ValueType::VALUE_TYPE_INT64);
                arg3->set_required(true);
                arg3->set_min_int64(1);
                arg3->set_max_int64(10);

                // Function: set_flag(enabled: bool required)
                auto* fn3 = caps->add_functions();
                fn3->set_name("set_flag");
                fn3->set_function_id(3);

                auto* arg4 = fn3->add_args();
                arg4->set_name("enabled");
                arg4->set_type(ValueType::VALUE_TYPE_BOOL);
                arg4->set_required(true);

                return true;
            }));

        registry->discover_provider("sim0", *mock_provider);
    }

    Value make_double(double val) {
        Value v;
        v.set_type(ValueType::VALUE_TYPE_DOUBLE);
        v.set_double_value(val);
        return v;
    }

    Value make_int64(int64_t val) {
        Value v;
        v.set_type(ValueType::VALUE_TYPE_INT64);
        v.set_int64_value(val);
        return v;
    }

    Value make_string(const std::string& val) {
        Value v;
        v.set_type(ValueType::VALUE_TYPE_STRING);
        v.set_string_value(val);
        return v;
    }

    Value make_bool(bool val) {
        Value v;
        v.set_type(ValueType::VALUE_TYPE_BOOL);
        v.set_bool_value(val);
        return v;
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> router;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unordered_map<std::string, std::shared_ptr<provider::IProviderHandle>> providers;
};

TEST_F(CallRouterValidationTest, ValidCallWithRequiredArgs) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(50.0);

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

TEST_F(CallRouterValidationTest, ValidCallWithOptionalArgs) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(50.0);
    req.args["unit"] = make_string("C");

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

TEST_F(CallRouterValidationTest, MissingRequiredArgument) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    // Missing required "value" argument

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("Missing required argument: value"));
}

TEST_F(CallRouterValidationTest, TypeMismatchDouble) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_string("not a number");  // Wrong type

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("Type mismatch"));
    EXPECT_THAT(error, HasSubstr("double"));
}

TEST_F(CallRouterValidationTest, TypeMismatchInt64) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_index";
    req.args["index"] = make_double(5.0);  // Wrong type (double instead of int64)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("Type mismatch"));
    EXPECT_THAT(error, HasSubstr("int64"));
}

TEST_F(CallRouterValidationTest, TypeMismatchBool) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_flag";
    req.args["enabled"] = make_int64(1);  // Wrong type (int instead of bool)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("Type mismatch"));
    EXPECT_THAT(error, HasSubstr("bool"));
}

TEST_F(CallRouterValidationTest, DoubleOutOfRangeTooLow) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(-10.0);  // Below min (0.0)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("below minimum"));
}

TEST_F(CallRouterValidationTest, DoubleOutOfRangeTooHigh) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(150.0);  // Above max (100.0)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("above maximum"));
}

TEST_F(CallRouterValidationTest, DoubleAtBoundaries) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";

    // Test min boundary
    req.args["value"] = make_double(0.0);
    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;

    // Test max boundary
    req.args["value"] = make_double(100.0);
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

TEST_F(CallRouterValidationTest, Int64OutOfRangeTooLow) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_index";
    req.args["index"] = make_int64(0);  // Below min (1)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("below minimum"));
}

TEST_F(CallRouterValidationTest, Int64OutOfRangeTooHigh) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_index";
    req.args["index"] = make_int64(20);  // Above max (10)

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("above maximum"));
}

TEST_F(CallRouterValidationTest, Int64AtBoundaries) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_index";

    // Test min boundary
    req.args["index"] = make_int64(1);
    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;

    // Test max boundary
    req.args["index"] = make_int64(10);
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

TEST_F(CallRouterValidationTest, UnknownArgument) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(50.0);
    req.args["unknown_param"] = make_string("oops");  // Not in function spec

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("Unknown argument: unknown_param"));
}

TEST_F(CallRouterValidationTest, OptionalArgumentCanBeOmitted) {
    control::CallRequest req;
    req.device_handle = "sim0/testdev";
    req.function_name = "set_temperature";
    req.args["value"] = make_double(50.0);
    // Omit optional "unit" argument

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// =======================
// Concurrency Tests
// =======================

TEST_F(CallRouterTest, ConcurrentCallsSameProvider) {
    // Regression test for provider_locks_ map race condition
    // Multiple threads calling the same provider concurrently should not crash

    RegisterMockDevice();

    // Mock provider will be called multiple times
    EXPECT_CALL(*mock_provider, call("dev1", 1, "reset", _, _)).Times(10).WillRepeatedly(Return(true));

    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = "reset";

    std::vector<std::thread> threads;
    threads.reserve(10);
    std::vector<control::CallResult> results(10);

    // Launch 10 threads calling the same provider concurrently
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back(
            [this, req, &results, i]() { results[i] = router->execute_call(req, *provider_registry); });
    }

    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }

    // All calls should succeed
    for (const auto& result : results) {
        EXPECT_TRUE(result.success) << "Error: " << result.error_message;
    }
}

TEST_F(CallRouterTest, ConcurrentCallsMultipleProviders) {
    // Test concurrent calls to different providers
    // This specifically tests the provider_locks_ map insertion race

    // Register devices on multiple providers
    RegisterMockDevice();  // sim0/dev1

    auto mock_provider2 = std::make_shared<StrictMock<MockProviderHandle>>();
    mock_provider2->_id = "sim1";
    EXPECT_CALL(*mock_provider2, provider_id()).WillRepeatedly(ReturnRef(mock_provider2->_id));
    EXPECT_CALL(*mock_provider2, is_available()).WillRepeatedly(Return(true));
    provider_registry->add_provider("sim1", mock_provider2);

    EXPECT_CALL(*mock_provider2, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
        Device dev;
        dev.set_device_id("dev2");
        devices.push_back(dev);
        return true;
    }));

    EXPECT_CALL(*mock_provider2, describe_device("dev2", _))
        .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
            auto* device = response.mutable_device();
            device->set_device_id("dev2");
            auto* caps = response.mutable_capabilities();
            auto* fn = caps->add_functions();
            fn->set_name("start");
            fn->set_function_id(2);
            return true;
        }));

    registry->discover_provider("sim1", *mock_provider2);

    // Mock calls
    EXPECT_CALL(*mock_provider, call("dev1", 1, "reset", _, _)).Times(5).WillRepeatedly(Return(true));

    EXPECT_CALL(*mock_provider2, call("dev2", 2, "start", _, _)).Times(5).WillRepeatedly(Return(true));

    std::vector<std::thread> threads;
    threads.reserve(10);
    std::vector<control::CallResult> results(10);

    // Launch threads alternating between providers
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, &results, i]() {
            control::CallRequest req;
            if (i % 2 == 0) {
                req.device_handle = "sim0/dev1";
                req.function_name = "reset";
            } else {
                req.device_handle = "sim1/dev2";
                req.function_name = "start";
            }
            results[i] = router->execute_call(req, *provider_registry);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All calls should succeed
    for (const auto& result : results) {
        EXPECT_TRUE(result.success) << "Error: " << result.error_message;
    }
}

// =======================
// Range Validation Bug Tests (F1)
// =======================
// These tests expose two defects in validate_argument_range():
//
//   Bug 1 — copy-paste: has_min and has_max are computed with identical || expressions,
//            so a one-sided bound (min set, max unset, or vice versa) is silently
//            expanded into a two-sided bound. A provider declaring only min=5.0 gets a
//            phantom max=0.0 enforced; a provider declaring only max=50.0 gets a phantom
//            min=0.0 enforced.
//
//   Bug 2 — zero heuristic: the presence check "field != 0" makes zero indistinguishable
//            from "not set". A provider declaring min=0.0 has that constraint silently
//            dropped; values below zero are accepted without complaint.
//
// Tests marked CURRENTLY FAILS are RED on main and go GREEN after the fix.
// Tests marked REGRESSION GUARD currently produce correct output (possibly for the
// wrong internal reason) and must remain GREEN through and after the fix.

class CallRouterRangeBugTest : public Test {
protected:
    using ValueType = anolis::deviceprovider::v1::ValueType;
    using Value = anolis::deviceprovider::v1::Value;

    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        router = std::make_unique<control::CallRouter>(*registry, *state_cache);

        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));

        providers["sim0"] = mock_provider;

        RegisterRangeTestDevice();
    }

    void RegisterRangeTestDevice() {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("rangedev");
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device("rangedev", _))
            .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
                auto* device = response.mutable_device();
                device->set_device_id("rangedev");

                auto* caps = response.mutable_capabilities();

                // fn 1: "min_only_double" — val: double, min=5.0, no max
                // Bug 1: current code gives this a phantom max=0.0.
                {
                    auto* fn = caps->add_functions();
                    fn->set_name("min_only_double");
                    fn->set_function_id(1);
                    auto* arg = fn->add_args();
                    arg->set_name("val");
                    arg->set_type(ValueType::VALUE_TYPE_DOUBLE);
                    arg->set_required(true);
                    arg->set_min_double(5.0);
                    // intentionally no set_max_double()
                }

                // fn 2: "max_only_double" — val: double, no min, max=50.0
                // Bug 1: current code gives this a phantom min=0.0.
                {
                    auto* fn = caps->add_functions();
                    fn->set_name("max_only_double");
                    fn->set_function_id(2);
                    auto* arg = fn->add_args();
                    arg->set_name("val");
                    arg->set_type(ValueType::VALUE_TYPE_DOUBLE);
                    arg->set_required(true);
                    arg->set_max_double(50.0);
                    // intentionally no set_min_double()
                }

                // fn 3: "zero_min_double" — val: double, min=0.0, no max
                // Bug 2: current code treats min=0.0 as "not set" via != 0 heuristic.
                {
                    auto* fn = caps->add_functions();
                    fn->set_name("zero_min_double");
                    fn->set_function_id(3);
                    auto* arg = fn->add_args();
                    arg->set_name("val");
                    arg->set_type(ValueType::VALUE_TYPE_DOUBLE);
                    arg->set_required(true);
                    arg->set_min_double(0.0);
                    // intentionally no set_max_double()
                }

                // fn 4: "min_only_int64" — idx: int64, min=1, no max
                // Bug 1: current code gives this a phantom max=0.
                {
                    auto* fn = caps->add_functions();
                    fn->set_name("min_only_int64");
                    fn->set_function_id(4);
                    auto* arg = fn->add_args();
                    arg->set_name("idx");
                    arg->set_type(ValueType::VALUE_TYPE_INT64);
                    arg->set_required(true);
                    arg->set_min_int64(1);
                    // intentionally no set_max_int64()
                }

                // fn 5: "max_only_int64" — idx: int64, no min, max=10
                // Bug 1: current code gives this a phantom min=0.
                {
                    auto* fn = caps->add_functions();
                    fn->set_name("max_only_int64");
                    fn->set_function_id(5);
                    auto* arg = fn->add_args();
                    arg->set_name("idx");
                    arg->set_type(ValueType::VALUE_TYPE_INT64);
                    arg->set_required(true);
                    arg->set_max_int64(10);
                    // intentionally no set_min_int64()
                }

                return true;
            }));

        registry->discover_provider("sim0", *mock_provider);
    }

    Value make_double(double v) {
        Value val;
        val.set_type(ValueType::VALUE_TYPE_DOUBLE);
        val.set_double_value(v);
        return val;
    }

    Value make_int64(int64_t v) {
        Value val;
        val.set_type(ValueType::VALUE_TYPE_INT64);
        val.set_int64_value(v);
        return val;
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> router;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unordered_map<std::string, std::shared_ptr<provider::IProviderHandle>> providers;
};

// ---- Bug 1: one-sided double bounds ----

// CURRENTLY FAILS: phantom max=0.0 causes "above maximum 0.000000" for val=10.0.
TEST_F(CallRouterRangeBugTest, MinOnlyDouble_InRangeValuePasses) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "min_only_double";
    req.args["val"] = make_double(10.0);  // above min=5.0, no upper bound

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// REGRESSION GUARD: below min should always be rejected.
TEST_F(CallRouterRangeBugTest, MinOnlyDouble_BelowMinFails) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "min_only_double";
    req.args["val"] = make_double(4.9);  // below min=5.0

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("below minimum"));
}

// CURRENTLY FAILS: phantom min=0.0 causes "below minimum 0.000000" for val=-10.0.
TEST_F(CallRouterRangeBugTest, MaxOnlyDouble_InRangeNegativeValuePasses) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "max_only_double";
    req.args["val"] = make_double(-10.0);  // below max=50.0, no lower bound

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// REGRESSION GUARD: above max should always be rejected.
TEST_F(CallRouterRangeBugTest, MaxOnlyDouble_AboveMaxFails) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "max_only_double";
    req.args["val"] = make_double(51.0);  // above max=50.0

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("above maximum"));
}

// ---- Bug 2: zero-valued minimum ----

// CURRENTLY FAILS: zero heuristic treats min=0.0 as "not set", so -1.0 passes
// when it should be rejected.
TEST_F(CallRouterRangeBugTest, ZeroMinDouble_BelowZeroFails) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "zero_min_double";
    req.args["val"] = make_double(-1.0);  // below min=0.0

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("below minimum"));
}

// REGRESSION GUARD: value exactly at the zero minimum should pass.
TEST_F(CallRouterRangeBugTest, ZeroMinDouble_AtZeroPasses) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "zero_min_double";
    req.args["val"] = make_double(0.0);  // exactly at min=0.0

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// ---- Bug 1: one-sided int64 bounds ----

// CURRENTLY FAILS: phantom max=0 causes "above maximum 0" for val=5.
TEST_F(CallRouterRangeBugTest, MinOnlyInt64_InRangeValuePasses) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "min_only_int64";
    req.args["idx"] = make_int64(5);  // above min=1, no upper bound

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// REGRESSION GUARD: below min should always be rejected.
TEST_F(CallRouterRangeBugTest, MinOnlyInt64_BelowMinFails) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "min_only_int64";
    req.args["idx"] = make_int64(0);  // below min=1

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("below minimum"));
}

// CURRENTLY FAILS: phantom min=0 causes "below minimum 0" for val=-5.
TEST_F(CallRouterRangeBugTest, MaxOnlyInt64_InRangeNegativeValuePasses) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "max_only_int64";
    req.args["idx"] = make_int64(-5);  // below max=10, no lower bound

    std::string error;
    EXPECT_TRUE(router->validate_call(req, error)) << "Error: " << error;
}

// REGRESSION GUARD: above max should always be rejected.
TEST_F(CallRouterRangeBugTest, MaxOnlyInt64_AboveMaxFails) {
    control::CallRequest req;
    req.device_handle = "sim0/rangedev";
    req.function_name = "max_only_int64";
    req.args["idx"] = make_int64(11);  // above max=10

    std::string error;
    EXPECT_FALSE(router->validate_call(req, error));
    EXPECT_THAT(error, HasSubstr("above maximum"));
}
