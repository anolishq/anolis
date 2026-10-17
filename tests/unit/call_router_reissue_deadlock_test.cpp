// Probe: does a return edge produced by CallRouter's post-call poll re-enter
// CallRouter on the same provider and self-deadlock?
//
// Wiring mirrors Runtime: CallRouter over StateCache, SafeStateController
// over CallRouter (also the actuation latch), DeviceLossLatch on the router,
// and StateCache's reachability sink doing what Runtime's handler does:
// engage on a transport loss, and on a return of a latched device re-assert
// its safe state (Runtime::reissue_safe_state_for -> reassert_for).
//
// Sequence: a transport loss latches the device; a manual call to it then
// succeeds, and the post-call poll is its first good read since the loss.
// The body runs on a worker thread with a deadline, so a hang is reported
// and the process exits instead of wedging the run.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "control/call_router.hpp"
#include "control/device_loss_latch.hpp"
#include "control/safe_state.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "runtime/config.hpp"
#include "state/state_cache.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

namespace {

constexpr auto kDeadline = std::chrono::seconds(5);

std::atomic<int> g_stage{0};
const char *stage_name(int s) {
    switch (s) {
        case 0:
            return "0: before the manual call";
        case 1:
            return "1: manual call reached the provider (CallRouter holds the provider lock)";
        case 2:
            return "2: return edge dispatched to the sink from the post-call poll";
        case 3:
            return "3: reassert_for entered (ladder about to call execute_call)";
        case 4:
            return "4: safe-state call reached the provider";
        case 5:
            return "5: reassert_for returned";
        default:
            return "?";
    }
}

}  // namespace

class CallRouterReissueDeadlockProbe : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        provider_registry = std::make_unique<provider::ProviderRegistry>();
        mock_provider = std::make_shared<NiceMock<MockProviderHandle>>();
        mock_provider->_id = "sim0";
        ON_CALL(*mock_provider, provider_id()).WillByDefault(ReturnRef(mock_provider->_id));
        ON_CALL(*mock_provider, is_available()).WillByDefault(Return(true));
        ON_CALL(*mock_provider, last_error()).WillByDefault(Return("read failed"));
        provider_registry->add_provider("sim0", mock_provider);

        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device> &devices) {
            Device dev;
            dev.set_device_id("heater");
            devices.push_back(dev);
            return true;
        }));
        EXPECT_CALL(*mock_provider, describe_device("heater", _))
            .WillOnce(Invoke([](const std::string &, DescribeDeviceResponse &response) {
                response.mutable_device()->set_device_id("heater");
                auto *caps = response.mutable_capabilities();
                auto *sig = caps->add_signals();
                sig->set_signal_id("temp");
                sig->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                sig->set_poll_hint_hz(1.0);  // default signal: gives the device a poll config
                auto *fn = caps->add_functions();
                fn->set_name("set_output");
                fn->set_function_id(1);
                fn->mutable_policy()->set_category(
                    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
                auto *arg = fn->add_args();
                arg->set_name("pwm");
                arg->set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                arg->set_required(true);
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);

        runtime::ModeTransitionCallConfig hook;
        hook.device_handle = "sim0/heater";
        hook.function_name = "set_output";
        hook.args["pwm"] = automation::ParameterValue{0.0};
        safety.safe_state.hooks.push_back(hook);

        // Runtime::init_core_services order.
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);
        safe_state_controller =
            std::make_unique<control::SafeStateController>(*registry, *call_router, *provider_registry, safety);
        call_router->set_actuation_latch(safe_state_controller.get());
        device_loss_latch = std::make_unique<control::DeviceLossLatch>();
        call_router->set_device_loss_latch(device_loss_latch.get());
        wire_like_runtime();
        ASSERT_TRUE(state_cache->initialize());
    }

    // Runtime::wire_device_loss_latch on main (7d9f6d3).
    void wire_like_runtime() {
        state_cache->set_device_reachability_sink(
            [this](const std::string &device_handle, bool reachable, anolis::deviceprovider::v1::Status_Code status) {
                if (!reachable) {
                    if (status == anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE ||
                        status == anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED) {
                        device_loss_latch->engage(device_handle);
                    }
                    return;
                }
                g_stage = 2;
                if (device_loss_latch->is_latched(device_handle)) {
                    g_stage = 3;
                    (void)safe_state_controller->reassert_for(device_handle);
                    g_stage = 5;
                }
            });
    }

    runtime::SafetyConfig safety;
    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
    std::unique_ptr<control::SafeStateController> safe_state_controller;
    std::unique_ptr<control::DeviceLossLatch> device_loss_latch;
};

TEST_F(CallRouterReissueDeadlockProbe, ReturnEdgeFromPostCallPollReassertsWithoutDeadlock) {
    bool fail_reads = true;
    EXPECT_CALL(*mock_provider, read_signals("heater", _, _, _))
        .WillRepeatedly(
            Invoke([&fail_reads](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                                 anolis::deviceprovider::v1::Status_Code &status) {
                if (fail_reads) {
                    status = anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE;
                    return false;
                }
                auto *v = response.add_values();
                v->set_signal_id("temp");
                v->mutable_value()->set_double_value(25.5);
                v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                status = anolis::deviceprovider::v1::Status_Code_CODE_OK;
                return true;
            }));

    int provider_calls = 0;
    EXPECT_CALL(*mock_provider, call("heater", _, "set_output", _, _))
        .WillRepeatedly(Invoke(
            [&provider_calls](const std::string &, uint32_t, const std::string &, const ValueMap &, CallResponse &) {
                ++provider_calls;
                g_stage = provider_calls == 1 ? 1 : 4;
                return true;
            }));

    // 1. The poll loop's read fails on transport: loss edge, latch engages.
    std::ostringstream discarded;
    std::streambuf *old = std::cerr.rdbuf(discarded.rdbuf());
    state_cache->poll_device_now("sim0/heater", *provider_registry);
    std::cerr.rdbuf(old);
    ASSERT_TRUE(device_loss_latch->is_latched("sim0/heater"));

    // 2. The device answers again. A manual call succeeds, and its post-call
    //    poll is the device's first good read since the loss.
    fail_reads = false;
    control::CallRequest request;
    request.device_handle = "sim0/heater";
    request.function_name = "set_output";
    request.args["pwm"].set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
    request.args["pwm"].set_double_value(0.5);

    std::promise<control::CallResult> done;
    auto done_f = done.get_future();
    std::thread worker([&] { done.set_value(call_router->execute_call(request, *provider_registry)); });

    if (done_f.wait_for(kDeadline) == std::future_status::timeout) {
        std::cout << "HANG: execute_call did not return within " << kDeadline.count()
                  << " s; last stage reached: " << stage_name(g_stage.load()) << std::endl;
        std::fflush(nullptr);
        if (std::getenv("PROBE_ABORT_ON_HANG") != nullptr) {
            std::abort();  // for gdb: thread apply all bt
        }
        std::_Exit(3);  // the worker holds the provider mutex; nothing can be torn down
    }
    worker.join();

    const auto result = done_f.get();
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(g_stage.load(), 5) << stage_name(g_stage.load());
    EXPECT_EQ(provider_calls, 2) << "manual call plus the re-asserted safe-state call";
}
