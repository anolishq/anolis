/**
 * mode_dispatch_test.cpp - how mode transitions interleave with dispatch (#762).
 * Every interleaving is forced through a callback the real code already makes.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "automation/bt_runtime.hpp"
#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "control/i_actuation_latch.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

using namespace anolis;
using namespace anolis::tests;
using automation::RuntimeMode;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

// One provider, one device, two untagged (so actuating) functions, and a real
// router and mode manager. Every dispatch is recorded in `order`.
struct Rig {
    registry::DeviceRegistry registry;
    state::StateCache state_cache{registry, 100};
    control::CallRouter router{registry, state_cache};
    provider::ProviderRegistry providers;
    automation::ModeManager mode;
    std::shared_ptr<NiceMock<MockProviderHandle>> mock = std::make_shared<NiceMock<MockProviderHandle>>();
    std::vector<std::string> order;

    explicit Rig(RuntimeMode initial, const std::string &manual_policy = "OVERRIDE") : mode(initial) {
        mock->_id = "sim0";
        ON_CALL(*mock, provider_id()).WillByDefault(ReturnRef(mock->_id));
        ON_CALL(*mock, is_available()).WillByDefault(Return(true));
        ON_CALL(*mock, list_devices(_)).WillByDefault(Invoke([](std::vector<Device> &devices) {
            Device dev;
            dev.set_device_id("dev1");
            devices.push_back(dev);
            return true;
        }));
        ON_CALL(*mock, describe_device("dev1", _))
            .WillByDefault(Invoke([](const std::string &, DescribeDeviceResponse &response) {
                response.mutable_device()->set_device_id("dev1");
                auto *caps = response.mutable_capabilities();
                auto *one = caps->add_functions();
                one->set_name("step_one");
                one->set_function_id(1);
                auto *two = caps->add_functions();
                two->set_name("step_two");
                two->set_function_id(2);
                return true;
            }));
        ON_CALL(*mock, call(_, _, _, _, _))
            .WillByDefault(Invoke(
                [this](const std::string &, uint32_t, const std::string &name, const ValueMap &, CallResponse &) {
                    order.push_back(name);
                    return true;
                }));
        providers.add_provider("sim0", mock);
        registry.discover_provider("sim0", *mock);
        router.set_mode_manager(&mode, manual_policy);
    }
};

control::CallRequest tree_call(const std::string &function) {
    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = function;
    req.is_automated = true;
    req.is_behavior_tree = true;
    return req;
}

control::CallRequest hook_call(const std::string &function) {
    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = function;
    req.is_automated = true;
    return req;
}

control::CallRequest manual_call(const std::string &function) {
    control::CallRequest req;
    req.device_handle = "sim0/dev1";
    req.function_name = function;
    return req;
}

// is_engaged() is first asked after the router's mode gates and before it takes
// the provider lock: exactly where a call waiting on that lock sits while
// another thread changes the mode.
struct ModeChangingLatch : control::IActuationLatch {
    automation::ModeManager *mode = nullptr;
    RuntimeMode target = RuntimeMode::FAULT;
    mutable std::atomic<bool> fired{false};
    mutable bool committed = false;

    bool is_engaged() const override {
        if (!fired.exchange(true)) {
            std::thread other([this] {
                std::string error;
                committed = mode->set_mode(target, error);
            });
            other.join();
        }
        return false;
    }
};

constexpr char kTwoCallTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <Sequence>
      <CallDevice device_handle="sim0/dev1" function_name="step_one"/>
      <CallDevice device_handle="sim0/dev1" function_name="step_two"/>
    </Sequence>
  </BehaviorTree>
</root>
)";

void expect_tree_refused_during_transition(RuntimeMode leaving_to) {
    SCOPED_TRACE(automation::mode_to_string(leaving_to));
    Rig rig(RuntimeMode::AUTO);
    control::CallResult hook_result;
    control::CallResult tree_result;
    rig.mode.on_before_mode_change([&](RuntimeMode, RuntimeMode next, std::string &) {
        if (next == leaving_to) {
            hook_result = rig.router.execute_call(hook_call("step_one"), rig.providers);
            tree_result = rig.router.execute_call(tree_call("step_two"), rig.providers);
        }
        return true;
    });

    std::string error;
    ASSERT_TRUE(rig.mode.set_mode(leaving_to, error)) << error;

    EXPECT_TRUE(hook_result.success) << "control: the hook's own call must still run: " << hook_result.error_message;
    EXPECT_FALSE(tree_result.success) << "a tree call was accepted after the transition out of AUTO began";
    EXPECT_EQ(rig.order, std::vector<std::string>{"step_one"});
}

}  // namespace

// After-hook shape. The tick has passed its one mode check and is between
// nodes when another thread commits FAULT and the hook runs. The second node
// must not reach the provider.
TEST(ModeDispatchTest, TreeNodeAfterFaultCommitIsNotDispatched) {
    Rig rig(RuntimeMode::AUTO);
    rig.mode.on_mode_change([&](RuntimeMode, RuntimeMode next) {
        if (next == RuntimeMode::FAULT) {
            rig.order.emplace_back("fault_hook");
        }
    });
    ON_CALL(*rig.mock, call(_, _, _, _, _))
        .WillByDefault(
            Invoke([&rig](const std::string &, uint32_t, const std::string &name, const ValueMap &, CallResponse &) {
                rig.order.push_back(name);
                if (name == "step_one") {
                    std::thread other([&rig] {
                        std::string error;
                        ASSERT_TRUE(rig.mode.set_mode(RuntimeMode::FAULT, error)) << error;
                    });
                    other.join();
                }
                return true;
            }));

    automation::ParameterManager parameters;
    automation::BTRuntime runtime(rig.state_cache, rig.router, rig.providers, rig.mode, &parameters);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tree_path =
        std::filesystem::temp_directory_path() / ("anolis-mode-dispatch-" + std::to_string(nonce) + ".xml");
    {
        std::ofstream out(tree_path, std::ios::binary);
        out << kTwoCallTreeXml;
    }
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));

    runtime.tick();

    const std::vector<std::string> expected{"step_one", "fault_hook"};
    EXPECT_EQ(rig.order, expected);
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// Before-hook shape, for the two transitions the bioreactor config hooks
// before commit. The mode still reads AUTO while the hook runs.
TEST(ModeDispatchTest, TreeCallRefusedOnceTransitionToFaultHasStarted) {
    expect_tree_refused_during_transition(RuntimeMode::FAULT);
}

TEST(ModeDispatchTest, TreeCallRefusedOnceTransitionToManualHasStarted) {
    expect_tree_refused_during_transition(RuntimeMode::MANUAL);
}

// In the router: FAULT commits while the tree's call waits for the provider
// lock.
TEST(ModeDispatchTest, TreeCallWaitingForProviderLockIsRefusedAfterFault) {
    Rig rig(RuntimeMode::AUTO);
    ModeChangingLatch latch;
    latch.mode = &rig.mode;
    latch.target = RuntimeMode::FAULT;
    rig.router.set_actuation_latch(&latch);

    const auto result = rig.router.execute_call(tree_call("step_one"), rig.providers);

    ASSERT_TRUE(latch.committed) << "control: the mode change must have happened inside the window";
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(rig.order.empty()) << "dispatched after FAULT committed";
}

// Same window, for a manual call under BLOCK: the mode gates are re-run after
// the provider lock, not only the latches.
TEST(ModeDispatchTest, ManualCallWaitingForProviderLockIsRefusedAfterAuto) {
    Rig rig(RuntimeMode::MANUAL, "BLOCK");
    ModeChangingLatch latch;
    latch.mode = &rig.mode;
    latch.target = RuntimeMode::AUTO;
    rig.router.set_actuation_latch(&latch);

    const auto result = rig.router.execute_call(manual_call("step_one"), rig.providers);

    ASSERT_TRUE(latch.committed) << "control: the mode change must have happened inside the window";
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(rig.order.empty()) << "dispatched in AUTO under BLOCK";
}

// A refused call says nothing about the device: leaving AUTO must not count
// toward the failure limit, or a routine AUTO -> MANUAL could end in FAULT.
TEST(ModeDispatchTest, ACallRefusedWhileLeavingAutoIsNotAFailedCall) {
    Rig rig(RuntimeMode::AUTO);
    automation::ParameterManager parameters;
    automation::BTRuntime runtime(rig.state_cache, rig.router, rig.providers, rig.mode, &parameters);
    runtime.set_max_consecutive_call_failures(1);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tree_path =
        std::filesystem::temp_directory_path() / ("anolis-mode-dispatch-refused-" + std::to_string(nonce) + ".xml");
    {
        std::ofstream out(tree_path, std::ios::binary);
        out << kTwoCallTreeXml;
    }
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
    rig.mode.on_before_mode_change([&](RuntimeMode, RuntimeMode next, std::string &) {
        if (next == RuntimeMode::MANUAL) {
            EXPECT_NO_THROW(runtime.tick());
        }
        return true;
    });

    std::string error;
    ASSERT_TRUE(rig.mode.set_mode(RuntimeMode::MANUAL, error)) << error;

    EXPECT_TRUE(rig.order.empty()) << "control: the tree's call must have been refused";
    EXPECT_TRUE(runtime.status().call_failures.empty()) << "a refusal was recorded as a failed call";
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// The tick loop waits out a transition out of AUTO, so a slow before-hook does
// not leave it ticking a tree whose every call is refused.
TEST(ModeDispatchTest, TheTickLoopPausesWhileLeavingAuto) {
    Rig rig(RuntimeMode::AUTO);
    automation::ParameterManager parameters;
    automation::BTRuntime runtime(rig.state_cache, rig.router, rig.providers, rig.mode, &parameters);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tree_path =
        std::filesystem::temp_directory_path() / ("anolis-mode-dispatch-pause-" + std::to_string(nonce) + ".xml");
    {
        std::ofstream out(tree_path, std::ios::binary);
        out << kTwoCallTreeXml;
    }
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
    std::string error;
    ASSERT_TRUE(runtime.start(error)) << error;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime.get_health().total_ticks < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(runtime.get_health().total_ticks, 2U) << "control: the loop must be ticking";

    std::uint64_t ticks_at_start = 0;
    std::uint64_t ticks_at_end = 0;
    rig.mode.on_before_mode_change([&](RuntimeMode, RuntimeMode next, std::string &) {
        if (next == RuntimeMode::MANUAL) {
            ticks_at_start = runtime.get_health().total_ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ticks_at_end = runtime.get_health().total_ticks;
        }
        return true;
    });
    ASSERT_TRUE(rig.mode.set_mode(RuntimeMode::MANUAL, error)) << error;

    // One tick may already have been in flight when the transition started.
    EXPECT_LE(ticks_at_end, ticks_at_start + 1);
    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}
