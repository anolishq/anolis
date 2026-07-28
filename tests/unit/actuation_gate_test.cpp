#include "runtime/actuation_gate.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "mocks/mock_provider_handle.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

class ActuationGateTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));
    }

    // Register device "dev0" with a single function of the given category.
    void RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category category) {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("dev0");
            devices.push_back(dev);
            return true;
        }));
        EXPECT_CALL(*mock_provider, describe_device("dev0", _))
            .WillOnce(Invoke([category](const std::string&, DescribeDeviceResponse& response) {
                response.mutable_device()->set_device_id("dev0");
                auto* fn = response.mutable_capabilities()->add_functions();
                fn->set_name("set_output");
                fn->set_function_id(1);
                fn->mutable_policy()->set_category(category);
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);
    }

    runtime::AutomationConfig automation;  // enabled=false, no hooks by default
    std::unique_ptr<registry::DeviceRegistry> registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
};

TEST_F(ActuationGateTest, RefusesEnabledActuatingWithoutHooks) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation);
    EXPECT_TRUE(gate.refused);
    ASSERT_EQ(gate.actuating_functions.size(), 1u);
    EXPECT_EQ(gate.actuating_functions[0], "sim0/dev0/set_output");
    EXPECT_THAT(gate.message, HasSubstr("mode_transition_hooks"));
    EXPECT_THAT(gate.message, HasSubstr("sim0/dev0/set_output"));
}

TEST_F(ActuationGateTest, RefusesUnspecifiedCategoryFailClosed) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_UNSPECIFIED);
    automation.enabled = true;
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation).refused);
}

TEST_F(ActuationGateTest, RefusesConfigCategory) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG);
    automation.enabled = true;
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation).refused);
}

TEST_F(ActuationGateTest, AllowsReadOnlyFunctions) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_READ);
    automation.enabled = true;
    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation);
    EXPECT_FALSE(gate.refused);
    EXPECT_TRUE(gate.actuating_functions.empty());
}

TEST_F(ActuationGateTest, AllowsWhenBeforeTransitionHookDeclared) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(runtime::ModeTransitionHookConfig{});
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation).refused);
}

TEST_F(ActuationGateTest, AllowsWhenAfterTransitionHookDeclared) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.after_transition.push_back(runtime::ModeTransitionHookConfig{});
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation).refused);
}

TEST_F(ActuationGateTest, AllowsWhenAutomationDisabled) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = false;
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation).refused);
}
