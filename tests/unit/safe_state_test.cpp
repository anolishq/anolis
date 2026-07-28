#include "control/safe_state.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "control/call_router.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"
#include "state/state_cache.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

namespace {

runtime::ModeTransitionCallConfig make_call(const std::string& handle, const std::string& function_name) {
    runtime::ModeTransitionCallConfig call;
    call.device_handle = handle;
    call.function_name = function_name;
    // The test actuator's "set_output" has a required "pwm" arg; supply it so
    // declared hooks/setpoints pass argument validation.
    call.args["pwm"] = automation::ParameterValue{0.0};
    return call;
}

}  // namespace

class SafeStateTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);
        provider_registry = std::make_unique<provider::ProviderRegistry>();

        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));
        provider_registry->add_provider("sim0", mock_provider);
    }

    // Register an actuating "set_output" (CATEGORY_ACTUATE) with a required double
    // "pwm" arg. `arg_type` lets a test force a non-numeric required arg.
    void RegisterActuator(
        anolis::deviceprovider::v1::ValueType arg_type = anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE) {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("heater");
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device("heater", _))
            .WillOnce(Invoke([arg_type](const std::string&, DescribeDeviceResponse& response) {
                response.mutable_device()->set_device_id("heater");
                auto* caps = response.mutable_capabilities();
                auto* fn = caps->add_functions();
                fn->set_name("set_output");
                fn->set_function_id(1);
                fn->mutable_policy()->set_category(
                    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
                auto* arg = fn->add_args();
                arg->set_name("pwm");
                arg->set_type(arg_type);
                arg->set_required(true);
                return true;
            }));

        registry->discover_provider("sim0", *mock_provider);
    }

    std::unique_ptr<control::SafeStateController> make_controller() {
        return std::make_unique<control::SafeStateController>(*registry, *call_router, *provider_registry, safety);
    }

    runtime::SafetyConfig safety;
    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
};

TEST_F(SafeStateTest, NoDeclaredSafeStateReportsNoneAndLatches) {
    RegisterActuator();
    auto controller = make_controller();

    // StrictMock: no call() expected — a None ladder must not actuate.
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_TRUE(result.actions.empty());
    EXPECT_TRUE(controller->is_engaged());

    const auto cap = controller->capability();
    EXPECT_EQ(cap.software_safe_state, control::SafeStateKind::None);
    EXPECT_EQ(cap.uncovered_actuating_functions, 1u);  // one actuator, no setpoint
    EXPECT_TRUE(cap.latched);
}

TEST_F(SafeStateTest, HooksRungRunsDeclaredHooks) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Hooks);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_TRUE(result.actions[0].success);
}

TEST_F(SafeStateTest, SetpointsApplyOnlyWithFullCoverage) {
    RegisterActuator();
    safety.safe_state.setpoints.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    EXPECT_EQ(controller->capability().software_safe_state, control::SafeStateKind::Setpoints);
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Setpoints);
}

TEST_F(SafeStateTest, PartialSetpointCoverageFailsClosedToNone) {
    RegisterActuator();
    // Setpoint targets a different function, so the actuator is uncovered.
    safety.safe_state.setpoints.push_back(make_call("sim0/heater", "some_other_fn"));
    auto controller = make_controller();

    // StrictMock: no call() expected — must not actuate under partial coverage.
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 1u);
}

TEST_F(SafeStateTest, ZeroRungZeroesActuatorsWhenDeclaredSafe) {
    RegisterActuator();
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", 1, "set_output", _, _)).WillOnce(Return(true));

    EXPECT_EQ(controller->capability().software_safe_state, control::SafeStateKind::Zero);
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_TRUE(result.actions[0].success);
}

TEST_F(SafeStateTest, ZeroRungRefusesNonNumericRequiredArg) {
    RegisterActuator(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    // StrictMock: no call() — we cannot invent a zero for a required string arg.
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_FALSE(result.actions[0].success);
    EXPECT_THAT(result.actions[0].error, HasSubstr("cannot zero"));
}

TEST_F(SafeStateTest, ClearReleasesLatch) {
    RegisterActuator();
    auto controller = make_controller();

    controller->trigger("test");
    EXPECT_TRUE(controller->is_engaged());

    controller->clear();
    EXPECT_FALSE(controller->is_engaged());
    EXPECT_FALSE(controller->capability().latched);
}

TEST_F(SafeStateTest, ZeroRungSkipsNonActuateFunctions) {
    EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
        Device dev;
        dev.set_device_id("mixed");
        devices.push_back(dev);
        return true;
    }));
    EXPECT_CALL(*mock_provider, describe_device("mixed", _))
        .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
            response.mutable_device()->set_device_id("mixed");
            auto* caps = response.mutable_capabilities();

            auto* actuate = caps->add_functions();
            actuate->set_name("set_output");
            actuate->set_function_id(1);
            actuate->mutable_policy()->set_category(
                anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
            auto* pwm = actuate->add_args();
            pwm->set_name("pwm");
            pwm->set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
            pwm->set_required(true);

            auto* config = caps->add_functions();  // must NOT be zeroed
            config->set_name("calibrate");
            config->set_function_id(2);
            config->mutable_policy()->set_category(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG);
            auto* offset = config->add_args();
            offset->set_name("offset");
            offset->set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
            offset->set_required(true);
            return true;
        }));
    registry->discover_provider("sim0", *mock_provider);

    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    // Only the ACTUATE function is driven; StrictMock fails if CONFIG is called.
    EXPECT_CALL(*mock_provider, call("mixed", 1, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_EQ(result.actions[0].function, "set_output");
}

TEST_F(SafeStateTest, ZeroRungRefusesActuatorWithNoRequiredArg) {
    EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
        Device dev;
        dev.set_device_id("btn");
        devices.push_back(dev);
        return true;
    }));
    EXPECT_CALL(*mock_provider, describe_device("btn", _))
        .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
            response.mutable_device()->set_device_id("btn");
            auto* fn = response.mutable_capabilities()->add_functions();
            fn->set_name("pulse");  // ACTUATE but no args -> nothing to zero
            fn->set_function_id(1);
            fn->mutable_policy()->set_category(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
            return true;
        }));
    registry->discover_provider("sim0", *mock_provider);

    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    // StrictMock: no call() — a no-arg actuator must not be bare-invoked.
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_FALSE(result.actions[0].success);
    EXPECT_THAT(result.actions[0].error, HasSubstr("no numeric required argument"));
}
