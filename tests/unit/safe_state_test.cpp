#include "control/safe_state.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "automation/mode_manager.hpp"
#include "control/call_router.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "runtime/actuation_gate.hpp"
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
        anolis::deviceprovider::v1::ValueType arg_type = anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE,
        std::optional<double> min_double = std::nullopt) {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("heater");
            devices.push_back(dev);
            return true;
        }));

        EXPECT_CALL(*mock_provider, describe_device("heater", _))
            .WillOnce(Invoke([arg_type, min_double](const std::string&, DescribeDeviceResponse& response) {
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
                if (min_double.has_value()) {
                    arg->set_min_double(*min_double);
                }
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

// One verdict for the whole rung (#256): a single failed call means the safe
// state was not achieved, even though the other calls landed.
TEST_F(SafeStateTest, OneFailedRungCallMeansSafeStateNotAchieved) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true)).WillOnce(Return(false));
    EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(Return("bus timeout"));
    EXPECT_CALL(*mock_provider, last_status_code())
        .WillRepeatedly(Return(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE));

    const auto result = controller->trigger("test");
    ASSERT_EQ(result.actions.size(), 2u);
    EXPECT_TRUE(result.actions[0].success);
    EXPECT_FALSE(result.actions[1].success);
    EXPECT_FALSE(result.safe_state_achieved);
}

TEST_F(SafeStateTest, AllRungCallsSucceedingMeansSafeStateAchieved) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    EXPECT_TRUE(controller->trigger("test").safe_state_achieved);
}

TEST_F(SafeStateTest, NoRungMeansSafeStateNotAchieved) {
    RegisterActuator();
    auto controller = make_controller();

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_FALSE(result.safe_state_achieved);
}

// A rung that issues nothing has achieved nothing: the zero rung on an
// inventory with no ACTUATE function sends no call.
TEST_F(SafeStateTest, RungThatIssuesNoCallMeansSafeStateNotAchieved) {
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    EXPECT_TRUE(result.actions.empty());
    EXPECT_FALSE(result.safe_state_achieved);
}

// The verdict is logged at WARN when the safe state was not achieved.
TEST_F(SafeStateTest, UnachievedVerdictIsLoggedAtWarn) {
    RegisterActuator();
    auto controller = make_controller();

    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    controller->trigger("test");
    std::cerr.rdbuf(previous);

    EXPECT_THAT(captured.str(),
                HasSubstr("[WARN]  [SafeState] E-stop ladder=none actions=0 safe_state_achieved=false"));
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

//=============================================================================
// uncovered_actuating_functions: dispatchable coverage of the active rung (#253)
//=============================================================================

// A hooks safe state that drives every actuating function has no gap. The
// count used to look at setpoints only, so this read 1.
TEST_F(SafeStateTest, HooksCoveringEveryActuatorCountZeroUncovered) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    const auto cap = controller->capability();
    EXPECT_EQ(cap.software_safe_state, control::SafeStateKind::Hooks);
    EXPECT_EQ(cap.uncovered_actuating_functions, 0u);
}

// A hook that names the function but cannot pass validate_call (here: its
// required argument is missing) does not cover it.
TEST_F(SafeStateTest, UndispatchableHookDoesNotCover) {
    RegisterActuator();
    auto call = make_call("sim0/heater", "set_output");
    call.args.clear();
    safety.safe_state.hooks.push_back(call);
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 1u);
}

TEST_F(SafeStateTest, HookOnAnotherFunctionDoesNotCover) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "some_other_fn"));
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 1u);
}

TEST_F(SafeStateTest, SetpointsCoveringEveryActuatorCountZeroUncovered) {
    RegisterActuator();
    safety.safe_state.setpoints.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 0u);
}

// The zero rung covers a function only when it can build the call.
TEST_F(SafeStateTest, ZeroRungCoversNumericActuator) {
    RegisterActuator();
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 0u);
}

TEST_F(SafeStateTest, ZeroRungDoesNotCoverActuatorItCannotBuild) {
    RegisterActuator(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 1u);
}

// Rung 3 builds pwm=0, but the provider declares min 1: the e-stop call
// fails validation, so the function is not covered.
TEST_F(SafeStateTest, ZeroRungDoesNotCoverActuatorWhoseMinExcludesZero) {
    RegisterActuator(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE, 1.0);
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    EXPECT_EQ(controller->capability().uncovered_actuating_functions, 1u);
}

TEST_F(SafeStateTest, UncoveredHelperNamesTheFunction) {
    RegisterActuator();
    const auto uncovered = control::uncovered_actuating_functions(*registry, *call_router, {});
    ASSERT_EQ(uncovered.size(), 1u);
    EXPECT_EQ(uncovered[0], "sim0/heater/set_output");

    EXPECT_TRUE(
        control::uncovered_actuating_functions(*registry, *call_router, {make_call("sim0/heater", "set_output")})
            .empty());
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

// The safe pole of a boolean is unknown: false releases a brake as readily as
// it disables a drive. Rung 3 must report the function as not driven rather
// than send a guessed bool (#255).
TEST_F(SafeStateTest, ZeroRungNeverSendsBoolArgument) {
    RegisterActuator(anolis::deviceprovider::v1::VALUE_TYPE_BOOL);
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    // StrictMock: any call() would fail this test.
    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_EQ(result.actions[0].function, "set_output");
    EXPECT_FALSE(result.actions[0].success);
    EXPECT_THAT(result.actions[0].error, HasSubstr("cannot zero"));
}

// A skipped call never reaches execute_call, so nothing else logs it. The
// e-stop must say at WARN which function it did not drive, and why.
TEST_F(SafeStateTest, TriggerWarnsForEachActionNotDriven) {
    RegisterActuator(anolis::deviceprovider::v1::VALUE_TYPE_BOOL);
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    // The logger writes to std::cerr; capture it in memory.
    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    controller->trigger("test");
    std::cerr.rdbuf(previous);
    const std::string log = captured.str();

    EXPECT_THAT(log, HasSubstr("[WARN]  [SafeState] E-stop did not drive sim0/heater/set_output: cannot zero "
                               "required non-numeric argument 'pwm'"));
}

//=============================================================================
// reassert_for: per-device re-issue on a lost device's return (#285)
//=============================================================================

// The setpoints rung is the one the hooks-only walk missed. A machine that
// declares setpoints and no hooks must still get its safe state re-asserted.
TEST_F(SafeStateTest, ReassertForRunsTheSetpointsRungForOneDevice) {
    RegisterActuator();
    safety.safe_state.setpoints.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->reassert_for("sim0/heater");
    EXPECT_EQ(result.kind, control::SafeStateKind::Setpoints);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_TRUE(result.actions[0].success);

    // Re-asserting is not an e-stop: no latch, no FAULT.
    EXPECT_FALSE(controller->is_engaged());
    EXPECT_FALSE(result.fault_requested);
}

TEST_F(SafeStateTest, ReassertForRunsTheZeroRungForOneDevice) {
    RegisterActuator();
    safety.safe_state.zero_is_safe = true;
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->reassert_for("sim0/heater");
    EXPECT_EQ(result.kind, control::SafeStateKind::Zero);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_FALSE(controller->is_engaged());
}

// Only the returning device is driven. A hook for some other device on the
// same provider is not re-run just because this one came back.
TEST_F(SafeStateTest, ReassertForIssuesNothingForOtherDevices) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    // StrictMock: any call() would fail this test.
    const auto result = controller->reassert_for("sim0/pump");
    EXPECT_EQ(result.kind, control::SafeStateKind::Hooks);
    EXPECT_TRUE(result.actions.empty());
}

// The startup run (#362) is the e-stop ladder and verdict without the e-stop:
// no latch, no FAULT. A mode manager is attached, as on an automation
// machine, so "no FAULT" is something the controller could have done.
TEST_F(SafeStateTest, DriveSafeStateRunsTheLadderWithoutLatching) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    controller->set_mode_manager(&mode);

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->drive_safe_state();
    EXPECT_EQ(result.kind, control::SafeStateKind::Hooks);
    ASSERT_EQ(result.actions.size(), 1u);
    EXPECT_TRUE(result.safe_state_achieved);
    EXPECT_FALSE(result.fault_requested);
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::MANUAL);
    EXPECT_FALSE(controller->is_engaged());

    const auto report = controller->startup_report();
    EXPECT_TRUE(report.ran);
    EXPECT_EQ(report.kind, control::SafeStateKind::Hooks);
    EXPECT_TRUE(report.safe_state_achieved);
    EXPECT_TRUE(report.failed_actions.empty());
}

TEST_F(SafeStateTest, StartupReportKeepsOnlyTheFailedCalls) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    auto controller = make_controller();

    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true)).WillOnce(Return(false));
    EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(Return("bus timeout"));
    EXPECT_CALL(*mock_provider, last_status_code())
        .WillRepeatedly(Return(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE));

    controller->drive_safe_state();
    const auto report = controller->startup_report();
    EXPECT_TRUE(report.ran);
    EXPECT_FALSE(report.safe_state_achieved);
    ASSERT_EQ(report.failed_actions.size(), 1u);
    EXPECT_EQ(report.failed_actions[0].device_handle, "sim0/heater");
    EXPECT_FALSE(report.failed_actions[0].success);
    EXPECT_FALSE(report.failed_actions[0].error.empty());
}

TEST_F(SafeStateTest, StartupReportIsNotRunBeforeDriveOrWithNoSafeState) {
    RegisterActuator();
    auto controller = make_controller();
    EXPECT_FALSE(controller->startup_report().ran);

    controller->drive_safe_state();  // no safe state declared: rung none
    const auto report = controller->startup_report();
    EXPECT_FALSE(report.ran);
    EXPECT_EQ(report.kind, control::SafeStateKind::None);
    EXPECT_FALSE(report.safe_state_achieved);
}

//=============================================================================
// -> FAULT mode-transition hooks on the e-stop's FAULT entry (#251)
//=============================================================================

// An automation machine wired as Runtime wires it: the controller is the
// router's latch and drives FAULT through the mode manager, and a `to: FAULT`
// hook drives heater/set_output. The hook's request comes from
// runtime::mode_hook_request, which both of Runtime's hook sites use.
class EstopFaultHookTest : public SafeStateTest {
protected:
    enum class Site { Before, After };

    void Wire(control::SafeStateController& controller, Site site = Site::Before) {
        call_router->set_mode_manager(&mode, "BLOCK");
        call_router->set_actuation_latch(&controller);
        controller.set_mode_manager(&mode);
        if (site == Site::Before) {
            mode.on_before_mode_change([this](automation::RuntimeMode, automation::RuntimeMode next, std::string&) {
                RunHook(next);
                return true;
            });
        } else {
            mode.on_mode_change([this](automation::RuntimeMode, automation::RuntimeMode next) { RunHook(next); });
        }
    }

    void RunHook(automation::RuntimeMode next) {
        hook_results.push_back(call_router->execute_call(HookRequest(next), *provider_registry));
    }

    static control::CallRequest HookRequest(automation::RuntimeMode next) {
        return runtime::mode_hook_request(make_call("sim0/heater", "set_output"), next);
    }

    // Engage the e-stop and expect the ladder's own single call to heater:
    // a second call would be the FAULT hook getting through the latch.
    void TriggerUnderRungAndExpectHookRefused(control::SafeStateKind expected_kind) {
        auto controller = make_controller();
        Wire(*controller);
        EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

        const auto result = controller->trigger("test");
        EXPECT_EQ(result.kind, expected_kind);
        EXPECT_TRUE(result.safe_state_achieved);
        EXPECT_TRUE(result.fault_ok);
        ASSERT_EQ(hook_results.size(), 1u);
        EXPECT_FALSE(hook_results[0].success);
        EXPECT_THAT(hook_results[0].error_message, HasSubstr("Actuation latched by e-stop"));
    }

    automation::ModeManager mode{automation::RuntimeMode::MANUAL};
    std::vector<control::CallResult> hook_results;
};

TEST_F(EstopFaultHookTest, RungNoneRunsTheFaultEntryHooks) {
    RegisterActuator();
    auto controller = make_controller();
    Wire(*controller);
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_TRUE(result.actions.empty());
    EXPECT_FALSE(result.safe_state_achieved) << "the ladder ran nothing";
    EXPECT_TRUE(result.fault_ok);
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::FAULT);
    EXPECT_TRUE(controller->is_engaged());
    ASSERT_EQ(hook_results.size(), 1u);
    EXPECT_TRUE(hook_results[0].success) << hook_results[0].error_message;
    EXPECT_FALSE(controller->admits_fault_entry_hooks()) << "the admission must not outlive the FAULT entry";
}

TEST_F(EstopFaultHookTest, RungHooksStillRefusesTheFaultEntryHooks) {
    RegisterActuator();
    safety.safe_state.hooks.push_back(make_call("sim0/heater", "set_output"));
    TriggerUnderRungAndExpectHookRefused(control::SafeStateKind::Hooks);
}

TEST_F(EstopFaultHookTest, RungSetpointsStillRefusesTheFaultEntryHooks) {
    RegisterActuator();
    safety.safe_state.setpoints.push_back(make_call("sim0/heater", "set_output"));
    TriggerUnderRungAndExpectHookRefused(control::SafeStateKind::Setpoints);
}

TEST_F(EstopFaultHookTest, RungZeroStillRefusesTheFaultEntryHooks) {
    RegisterActuator();
    safety.safe_state.zero_is_safe = true;
    TriggerUnderRungAndExpectHookRefused(control::SafeStateKind::Zero);
}

// The admission belongs to the e-stop's own FAULT entry. A later entry into
// FAULT while the latch is still engaged is refused, as before.
TEST_F(EstopFaultHookTest, LaterFaultEntryWhileLatchedStillRefusesHooks) {
    RegisterActuator();
    auto controller = make_controller();
    Wire(*controller);
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    controller->trigger("test");
    std::string err;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::MANUAL, err)) << err;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::FAULT, err)) << err;

    ASSERT_EQ(hook_results.size(), 3u);  // e-stop's FAULT entry, FAULT->MANUAL, MANUAL->FAULT
    EXPECT_TRUE(hook_results[0].success) << hook_results[0].error_message;
    EXPECT_FALSE(hook_results[2].success);
    EXPECT_THAT(hook_results[2].error_message, HasSubstr("Actuation latched by e-stop"));
}

// No e-stop: an ordinary entry into FAULT runs the hook, as it always has.
TEST_F(EstopFaultHookTest, OrdinaryFaultEntryRunsTheHooks) {
    RegisterActuator();
    auto controller = make_controller();
    Wire(*controller);
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    std::string err;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::FAULT, err)) << err;
    ASSERT_EQ(hook_results.size(), 1u);
    EXPECT_TRUE(hook_results[0].success) << hook_results[0].error_message;
}

// The admission is the trigger thread's alone. A tagged call from any other
// thread while it is live is refused by the latch.
TEST_F(EstopFaultHookTest, TaggedCallFromAnotherThreadDuringAdmissionIsRefused) {
    RegisterActuator();
    auto controller = make_controller();
    call_router->set_mode_manager(&mode, "BLOCK");
    call_router->set_actuation_latch(controller.get());
    controller->set_mode_manager(&mode);
    control::CallResult other;
    mode.on_before_mode_change([&](automation::RuntimeMode, automation::RuntimeMode next, std::string&) {
        std::thread thread([&] { other = call_router->execute_call(HookRequest(next), *provider_registry); });
        thread.join();
        return true;
    });
    // StrictMock: no call() expected -- the other thread's call must not land.

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_FALSE(other.success);
    EXPECT_THAT(other.error_message, HasSubstr("Actuation latched by e-stop"));
}

// A hook that throws something set_mode does not catch unwinds out of
// trigger(). The admission must go with it, or later tagged calls on this
// thread would pass the latch.
TEST_F(EstopFaultHookTest, HookThrowingNonStdExceptionStillWithdrawsAdmission) {
    RegisterActuator();
    auto controller = make_controller();
    call_router->set_mode_manager(&mode, "BLOCK");
    call_router->set_actuation_latch(controller.get());
    controller->set_mode_manager(&mode);
    mode.on_before_mode_change([](automation::RuntimeMode, automation::RuntimeMode, std::string&) -> bool {
        throw 42;  // not a std::exception, so set_mode does not catch it
    });

    EXPECT_ANY_THROW(controller->trigger("test"));
    EXPECT_TRUE(controller->is_engaged());
    EXPECT_FALSE(controller->admits_fault_entry_hooks());
    const auto later = call_router->execute_call(HookRequest(automation::RuntimeMode::FAULT), *provider_registry);
    EXPECT_FALSE(later.success);
    EXPECT_THAT(later.error_message, HasSubstr("Actuation latched by e-stop"));
}

TEST_F(EstopFaultHookTest, RungNoneRunsAfterTransitionFaultHooks) {
    RegisterActuator();
    auto controller = make_controller();
    Wire(*controller, Site::After);
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_FALSE(result.safe_state_achieved);
    ASSERT_EQ(hook_results.size(), 1u);
    EXPECT_TRUE(hook_results[0].success) << hook_results[0].error_message;
}

// Pressed in IDLE under rung none, as on any IDLE -> FAULT entry: a
// before_transition hook runs while the mode is still IDLE and the IDLE gate
// refuses it; an after_transition hook runs once FAULT is committed.
TEST_F(EstopFaultHookTest, PressedInIdleBeforeHookIsRefusedByTheIdleGate) {
    RegisterActuator();
    std::string err;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::IDLE, err)) << err;
    auto controller = make_controller();
    Wire(*controller, Site::Before);
    // StrictMock: no call() expected.

    const auto result = controller->trigger("test");
    EXPECT_EQ(result.kind, control::SafeStateKind::None);
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::FAULT);
    ASSERT_EQ(hook_results.size(), 1u);
    EXPECT_FALSE(hook_results[0].success);
    EXPECT_THAT(hook_results[0].error_message, HasSubstr("blocked in IDLE mode"));
}

TEST_F(EstopFaultHookTest, PressedInIdleAfterHookRuns) {
    RegisterActuator();
    std::string err;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::IDLE, err)) << err;
    auto controller = make_controller();
    Wire(*controller, Site::After);
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _)).WillOnce(Return(true));

    controller->trigger("test");
    ASSERT_EQ(hook_results.size(), 1u);
    EXPECT_TRUE(hook_results[0].success) << hook_results[0].error_message;
}

// The shared builder tags only calls into FAULT.
TEST(ModeHookRequestTest, TagsOnlyTransitionsIntoFault) {
    runtime::ModeTransitionCallConfig call;
    call.device_handle = "sim0/heater";
    call.function_name = "set_output";
    EXPECT_TRUE(runtime::mode_hook_request(call, automation::RuntimeMode::FAULT).fault_entry_hook);
    for (auto next : {automation::RuntimeMode::MANUAL, automation::RuntimeMode::AUTO, automation::RuntimeMode::IDLE}) {
        const auto request = runtime::mode_hook_request(call, next);
        EXPECT_FALSE(request.fault_entry_hook);
        EXPECT_TRUE(request.is_automated);
        EXPECT_FALSE(request.safe_state);
    }
}
