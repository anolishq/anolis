#include "runtime/actuation_gate.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "automation/mode_manager.hpp"
#include "control/call_router.hpp"
#include "control/safe_state.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"
#include "state/state_cache.hpp"

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

    // A realistic hook for the `from`->`to` transition with one call (matches what
    // the loader accepts). Empty `from` is a wildcard.
    static runtime::ModeTransitionHookConfig make_hook(const std::string& to, const std::string& from = "") {
        runtime::ModeTransitionHookConfig hook;
        hook.from = from;
        hook.to = to;
        runtime::ModeTransitionCallConfig call;
        call.device_handle = "sim0/dev0";
        call.function_name = "set_output";
        hook.calls.push_back(call);
        return hook;
    }

    // A realistic *->FAULT safe-state hook (wildcard from, covers AUTO->FAULT).
    static runtime::ModeTransitionHookConfig make_fault_hook() { return make_hook("FAULT"); }

    runtime::AutomationConfig automation;  // enabled=false, no hooks by default
    std::unique_ptr<registry::DeviceRegistry> registry;
    std::shared_ptr<MockProviderHandle> mock_provider;

    // Default-constructed: declares no safe state. Tests that care set it.
    runtime::SafetyConfig safety;
};

TEST_F(ActuationGateTest, RefusesEnabledActuatingWithoutHooks) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    EXPECT_TRUE(gate.refused);
    ASSERT_EQ(gate.actuating_functions.size(), 1u);
    EXPECT_EQ(gate.actuating_functions[0], "sim0/dev0/set_output");
    EXPECT_THAT(gate.message, HasSubstr("mode_transition_hooks"));
    EXPECT_THAT(gate.message, HasSubstr("sim0/dev0/set_output"));
}

TEST_F(ActuationGateTest, RefusesUnspecifiedCategoryFailClosed) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_UNSPECIFIED);
    automation.enabled = true;
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, RefusesConfigCategory) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG);
    automation.enabled = true;
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsReadOnlyFunctions) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_READ);
    automation.enabled = true;
    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    EXPECT_FALSE(gate.refused);
    EXPECT_TRUE(gate.actuating_functions.empty());
}

TEST_F(ActuationGateTest, AllowsWhenBeforeTransitionHookDeclared) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsWhenAfterTransitionHookDeclared) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.after_transition.push_back(make_fault_hook());
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, RefusesWhenOnlyNonFaultHookDeclared) {
    // A hook that targets only MANUAL is not an autonomous safe-state path on a
    // -> FAULT transition; the gate must still refuse (#232).
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("MANUAL"));

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    EXPECT_TRUE(gate.refused);
    ASSERT_EQ(gate.actuating_functions.size(), 1u);
    EXPECT_EQ(gate.actuating_functions[0], "sim0/dev0/set_output");
    EXPECT_THAT(gate.message, HasSubstr("FAULT"));
    EXPECT_THAT(gate.message, HasSubstr("mode_transition_hooks"));
    EXPECT_THAT(gate.message, HasSubstr("sim0/dev0/set_output"));
}

TEST_F(ActuationGateTest, RefusesWhenNonFaultHooksInBothLists) {
    // Proves both lists are scanned for a FAULT target, not mere presence.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("MANUAL"));
    automation.mode_transition_hooks.after_transition.push_back(make_hook("IDLE"));
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsWildcardToHook) {
    // to: "*" fires on every transition including -> FAULT, so it satisfies the gate.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("*"));
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsEmptyToHook) {
    // An omitted (empty) `to` is a wildcard and covers -> FAULT.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook(""));
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, RefusesWhenFaultHookRestrictedToNonAutoFrom) {
    // A {from: IDLE, to: FAULT} hook never fires on AUTO->FAULT (autonomous
    // actuation only runs in AUTO), so it does not satisfy the gate (#232).
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("FAULT", "IDLE"));
    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsFaultHookFromAuto) {
    // An explicit {from: AUTO, to: FAULT} hook fires on the transition we gate on.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("FAULT", "AUTO"));
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsFaultHookAmongNonFaultHooks) {
    // any-of, not all-of: a FAULT hook alongside a non-FAULT hook satisfies the gate.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_hook("MANUAL"));
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsWhenAutomationDisabled) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = false;
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, AllowsWhenRegistryEmpty) {
    // No devices discovered: nothing can actuate, so hookless AUTO is allowed.
    automation.enabled = true;
    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

// --- enforce_hookless_gate_in_auto (#233: in-AUTO restart re-check) ----------

TEST_F(ActuationGateTest, EnforceInAuto_RefusalForcesFault) {
    // In AUTO, a restart that exposes a hookless actuator must force FAULT.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation::ModeManager mode(automation::RuntimeMode::AUTO);

    EXPECT_TRUE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "test"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::FAULT);
}

TEST_F(ActuationGateTest, EnforceInAuto_NoOpWhenGatePasses) {
    // A declared AUTO->FAULT hook keeps AUTO after the restart re-check.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());
    automation::ModeManager mode(automation::RuntimeMode::AUTO);

    EXPECT_FALSE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "test"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::AUTO);
}

TEST_F(ActuationGateTest, EnforceInAuto_NoOpForReadOnlyInventory) {
    // The motivating case: read-only rig restarts with the same read-only
    // inventory while in AUTO -> nothing to refuse, stays AUTO.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_READ);
    automation.enabled = true;
    automation::ModeManager mode(automation::RuntimeMode::AUTO);

    EXPECT_FALSE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "test"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::AUTO);
}

TEST_F(ActuationGateTest, EnforceInAuto_NoOpWhenNotInAuto) {
    // Not in AUTO: the entry-time gate guards the next MANUAL->AUTO, so the
    // restart re-check must not touch the mode.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    for (auto m : {automation::RuntimeMode::MANUAL, automation::RuntimeMode::IDLE}) {
        SCOPED_TRACE(automation::mode_to_string(m));
        automation::ModeManager mode(m);
        EXPECT_FALSE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "test"));
        EXPECT_EQ(mode.current_mode(), m);
    }
}

TEST_F(ActuationGateTest, EnforceInAuto_NoOpWhenAutomationDisabled) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = false;
    automation::ModeManager mode(automation::RuntimeMode::AUTO);
    EXPECT_FALSE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "test"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::AUTO);
}

// --- install_hookless_auto_gate (#239: the gate at commit) ------------------

TEST_F(ActuationGateTest, InstalledGate_RefusesAutoWhenTheInventoryTurnsHooklessMidTransition) {
    // The #239 interleaving, made deterministic. MANUAL -> AUTO starts on a
    // read-only inventory, so the early gate passes. The before_transition
    // hooks (a blocking callback here) hold the transition open; meanwhile a
    // provider restart commits an actuating, hookless inventory and runs its
    // in-AUTO re-check, a no-op because the mode is still MANUAL. AUTO must
    // not commit.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_READ);
    automation.enabled = true;
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    runtime::install_hookless_auto_gate(mode, *registry, automation, safety);

    std::promise<void> hooks_running;
    std::promise<void> restart_done;
    auto restart_done_future = restart_done.get_future();
    mode.on_before_mode_change([&](automation::RuntimeMode, automation::RuntimeMode, std::string&) {
        hooks_running.set_value();
        restart_done_future.wait();
        return true;
    });

    bool committed = true;
    std::string error;
    std::thread http([&] { committed = mode.set_mode(automation::RuntimeMode::AUTO, error); });

    hooks_running.get_future().wait();
    auto restarted = std::make_shared<StrictMock<MockProviderHandle>>();
    restarted->_id = "sim1";
    EXPECT_CALL(*restarted, provider_id()).WillRepeatedly(ReturnRef(restarted->_id));
    EXPECT_CALL(*restarted, is_available()).WillRepeatedly(Return(true));
    EXPECT_CALL(*restarted, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
        Device dev;
        dev.set_device_id("motor0");
        devices.push_back(dev);
        return true;
    }));
    EXPECT_CALL(*restarted, describe_device("motor0", _))
        .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
            response.mutable_device()->set_device_id("motor0");
            auto* fn = response.mutable_capabilities()->add_functions();
            fn->set_name("set_pwm");
            fn->set_function_id(1);
            fn->mutable_policy()->set_category(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
            return true;
        }));
    registry->discover_provider("sim1", *restarted);
    EXPECT_FALSE(runtime::enforce_hookless_gate_in_auto(*registry, automation, safety, mode, "restart"));
    restart_done.set_value();
    http.join();

    EXPECT_FALSE(committed);
    EXPECT_THAT(error, HasSubstr("refusing AUTO"));
    EXPECT_THAT(error, HasSubstr("sim1/motor0/set_pwm"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::MANUAL);
}

TEST_F(ActuationGateTest, InstalledGate_RefusesBeforeTheTransitionHooksRun) {
    // The early check stays: a refused AUTO entry issues no hook calls.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    runtime::install_hookless_auto_gate(mode, *registry, automation, safety);
    bool hooks_ran = false;
    mode.on_before_mode_change([&](automation::RuntimeMode, automation::RuntimeMode, std::string&) {
        hooks_ran = true;
        return true;
    });

    std::string error;
    EXPECT_FALSE(mode.set_mode(automation::RuntimeMode::AUTO, error));
    EXPECT_FALSE(hooks_ran);
    EXPECT_THAT(error, HasSubstr("refusing AUTO"));
}

TEST_F(ActuationGateTest, InstalledGate_AdmitsAutoWithAFaultHookAndLeavesOtherTransitionsAlone) {
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    runtime::install_hookless_auto_gate(mode, *registry, automation, safety);

    std::string error;
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::AUTO, error)) << error;
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::MANUAL, error)) << error;
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::IDLE, error)) << error;
}

// --- install_estop_auto_gate (#481: AUTO waits for the latch) ---------------

namespace {
// A latch the test sets by hand, so the mode is left alone (a real trigger()
// also enters FAULT, which would refuse the transition on its own).
struct FakeLatch : control::IActuationLatch {
    std::atomic<bool> engaged{false};
    bool is_engaged() const override { return engaged.load(); }
};
}  // namespace

TEST_F(ActuationGateTest, EstopGate_RefusesAutoWhileLatchedAndAdmitsItOnceCleared) {
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    FakeLatch latch;
    runtime::install_estop_auto_gate(mode, latch);
    latch.engaged = true;

    std::string error;
    EXPECT_FALSE(mode.set_mode(automation::RuntimeMode::AUTO, error));
    EXPECT_THAT(error, HasSubstr("the software stop's latch is engaged"));
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::IDLE, error)) << error;
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::FAULT, error)) << error;

    latch.engaged = false;
    ASSERT_TRUE(mode.set_mode(automation::RuntimeMode::MANUAL, error)) << error;
    EXPECT_TRUE(mode.set_mode(automation::RuntimeMode::AUTO, error)) << error;
}

TEST_F(ActuationGateTest, EstopGate_RefusesAutoWhenTheLatchEngagesMidTransition) {
    // The latch engages while MANUAL -> AUTO's hooks run, after the early
    // check passed. AUTO must not commit.
    automation::ModeManager mode(automation::RuntimeMode::MANUAL);
    FakeLatch latch;
    runtime::install_estop_auto_gate(mode, latch);
    bool hooks_ran = false;
    mode.on_before_mode_change([&](automation::RuntimeMode, automation::RuntimeMode, std::string&) {
        hooks_ran = true;
        latch.engaged = true;  // an operator's software stop, mid-transition
        return true;
    });

    std::string error;
    EXPECT_FALSE(mode.set_mode(automation::RuntimeMode::AUTO, error));
    EXPECT_TRUE(hooks_ran);
    EXPECT_THAT(error, HasSubstr("the software stop's latch is engaged"));
    EXPECT_EQ(mode.current_mode(), automation::RuntimeMode::MANUAL);
}

// --- #251: the predicate is now shared, so it must stay one predicate --------
// The gate admits AUTO on the strength of a fault safe-state hook; the startup
// preflight uses the SAME question to warn that such a machine's e-stop has
// no safe state of its own beyond those very hooks. Two copies of this drifting apart
// is how a machine gets gated as safe while its e-stop does nothing, so pin the
// shapes the gate accepts.

TEST_F(ActuationGateTest, FaultHookPredicateMatchesWhatTheGateAdmits) {
    // Every shape the gate's own refusal message tells operators to write:
    // "from: AUTO/\"*\"/omitted and to: FAULT/\"*\"/omitted".
    for (const std::string& from : {std::string(""), std::string("*"), std::string("AUTO")}) {
        for (const std::string& to : {std::string(""), std::string("*"), std::string("FAULT")}) {
            runtime::AutomationConfig cfg;
            cfg.mode_transition_hooks.before_transition.push_back(make_hook(to, from));
            EXPECT_TRUE(runtime::has_fault_safe_state_hook(cfg))
                << "from='" << from << "' to='" << to << "' is endorsed by the gate message but not recognised";
        }
    }
}

TEST_F(ActuationGateTest, FaultHookPredicateRejectsNonFaultTransitions) {
    runtime::AutomationConfig cfg;
    // Fires when LEAVING AUTO for MANUAL — never on the fault path.
    cfg.mode_transition_hooks.before_transition.push_back(make_hook("MANUAL", "AUTO"));
    EXPECT_FALSE(runtime::has_fault_safe_state_hook(cfg));

    // Reaches FAULT, but only from IDLE, so it cannot cover AUTO -> FAULT.
    cfg.mode_transition_hooks.before_transition.push_back(make_hook("FAULT", "IDLE"));
    EXPECT_FALSE(runtime::has_fault_safe_state_hook(cfg));
}

TEST_F(ActuationGateTest, FaultHookPredicateFindsAfterTransitionHooks) {
    runtime::AutomationConfig cfg;
    cfg.mode_transition_hooks.after_transition.push_back(make_fault_hook());
    EXPECT_TRUE(runtime::has_fault_safe_state_hook(cfg));
}

TEST_F(ActuationGateTest, FaultHookPredicateIsFalseWithNoHooks) {
    runtime::AutomationConfig cfg;
    EXPECT_FALSE(runtime::has_fault_safe_state_hook(cfg));
}

// --- #258: the refusal must not steer operators into the #251 shape ---------

TEST_F(ActuationGateTest, RefusalNamesSafeStateWhenNoneIsDeclared) {
    // Following this message to the letter satisfies the gate and leaves a
    // machine whose e-stop ladder drives nothing, leaving only the hooks the
    // operator just added (#251). The message has to say so.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    ASSERT_TRUE(gate.refused);
    EXPECT_THAT(gate.message, HasSubstr("safety.safe_state"));
    EXPECT_THAT(gate.message, HasSubstr("/v0/estop"));
}

TEST_F(ActuationGateTest, RefusalStaysQuietWhenSafeStateIsDeclared) {
    // An operator who already declared one does not need to be told to.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    safety.safe_state.hooks.push_back({});

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    ASSERT_TRUE(gate.refused) << "safety.safe_state must not satisfy the gate — it is not an autonomous path";
    EXPECT_THAT(gate.message, Not(HasSubstr("safety.safe_state")));
    // The actual fix is still named.
    EXPECT_THAT(gate.message, HasSubstr("mode_transition_hooks"));
}

TEST_F(ActuationGateTest, SafeStateDoesNotChangeTheVerdict) {
    // Text only. The ladder fires on operator request, never autonomously, so
    // it cannot stand in for a fault-safe path.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    safety.safe_state.zero_is_safe = true;

    EXPECT_TRUE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);
}

TEST_F(ActuationGateTest, SetpointsCoveringNothingStillGetTheNote) {
    // The hole a config-level "is safe_state non-empty?" check would leave.
    // SafeStateController honours the setpoints rung only when it covers EVERY
    // actuating output; partial coverage plans None and the ladder drives
    // nothing (see safe_state_test PartialSetpointCoverageFailsClosedToNone).
    // A gate that inspected the config itself would see a non-empty setpoints
    // list, stay quiet, and hand back exactly the #251 machine.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    runtime::ModeTransitionCallConfig setpoint;
    setpoint.device_handle = "sim0/some-other-device";  // covers nothing here
    setpoint.function_name = "set_output";
    safety.safe_state.setpoints.push_back(setpoint);

    ASSERT_EQ(control::planned_safe_state_kind(*registry, safety), control::SafeStateKind::None);

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    ASSERT_TRUE(gate.refused);
    EXPECT_THAT(gate.message, HasSubstr("safety.safe_state"));
}

TEST_F(ActuationGateTest, SetpointsCoveringEverythingSuppressTheNote) {
    // ...and when the ladder really would drive, the note stays off.
    RegisterFunction(anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
    automation.enabled = true;
    runtime::ModeTransitionCallConfig setpoint;
    setpoint.device_handle = "sim0/dev0";
    setpoint.function_name = "set_output";
    safety.safe_state.setpoints.push_back(setpoint);

    ASSERT_EQ(control::planned_safe_state_kind(*registry, safety), control::SafeStateKind::Setpoints);

    const auto gate = runtime::evaluate_hookless_auto_gate(*registry, automation, safety);
    ASSERT_TRUE(gate.refused);
    EXPECT_THAT(gate.message, Not(HasSubstr("safety.safe_state")));
}

// --- #246: startup warning for actuating outputs no FAULT hook drives -------

// Register "dev0" with two ACTUATE functions, set_output and set_other, neither
// taking arguments, so any call that names one passes validate_call.
class FaultHookCoverageTest : public ActuationGateTest {
protected:
    void RegisterTwoActuators() {
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device>& devices) {
            Device dev;
            dev.set_device_id("dev0");
            devices.push_back(dev);
            return true;
        }));
        EXPECT_CALL(*mock_provider, describe_device("dev0", _))
            .WillOnce(Invoke([](const std::string&, DescribeDeviceResponse& response) {
                response.mutable_device()->set_device_id("dev0");
                auto* caps = response.mutable_capabilities();
                for (const auto& [id, name] : {std::pair<uint32_t, const char*>{1, "set_output"}, {2, "set_other"}}) {
                    auto* fn = caps->add_functions();
                    fn->set_name(name);
                    fn->set_function_id(id);
                    fn->mutable_policy()->set_category(
                        anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
                }
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);
    }

    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
};

// The reference shape: the FAULT hook drives one output and leaves the other.
// The gate still passes; the warning names the one left alone.
TEST_F(FaultHookCoverageTest, HookThatSkipsAnOutputIsReportedAndGateVerdictIsUnchanged) {
    RegisterTwoActuators();
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());  // set_output only

    EXPECT_FALSE(runtime::evaluate_hookless_auto_gate(*registry, automation, safety).refused);

    const auto uncovered = runtime::fault_hook_uncovered_functions(*registry, *call_router, automation);
    ASSERT_EQ(uncovered.size(), 1u);
    EXPECT_EQ(uncovered[0], "sim0/dev0/set_other");
}

// A hook on another transition does not count toward AUTO -> FAULT coverage.
TEST_F(FaultHookCoverageTest, NonFaultHookDoesNotCover) {
    RegisterTwoActuators();
    automation.enabled = true;
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());
    auto manual_hook = make_hook("MANUAL", "AUTO");
    manual_hook.calls[0].function_name = "set_other";
    automation.mode_transition_hooks.after_transition.push_back(manual_hook);

    const auto uncovered = runtime::fault_hook_uncovered_functions(*registry, *call_router, automation);
    ASSERT_EQ(uncovered.size(), 1u);
    EXPECT_EQ(uncovered[0], "sim0/dev0/set_other");
}

TEST_F(FaultHookCoverageTest, HooksCoveringEveryOutputReportNothing) {
    RegisterTwoActuators();
    automation.enabled = true;
    auto hook = make_fault_hook();
    auto other = hook.calls[0];
    other.function_name = "set_other";
    hook.calls.push_back(other);
    automation.mode_transition_hooks.after_transition.push_back(hook);

    EXPECT_TRUE(runtime::fault_hook_uncovered_functions(*registry, *call_router, automation).empty());
}

TEST_F(FaultHookCoverageTest, NothingReportedWhenAutomationDisabled) {
    RegisterTwoActuators();
    automation.mode_transition_hooks.before_transition.push_back(make_fault_hook());

    EXPECT_TRUE(runtime::fault_hook_uncovered_functions(*registry, *call_router, automation).empty());
}
