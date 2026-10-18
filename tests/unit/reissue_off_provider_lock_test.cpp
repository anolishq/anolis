// A latched device's return edge can be delivered by CallRouter's post-call
// poll, on the thread that holds the provider's lock. The safe-state re-assert
// goes back through CallRouter to the same provider, so run on that thread it
// blocks on the lock it already holds (#427).
//
// Wiring is Runtime's: CallRouter over StateCache, SafeStateController over
// CallRouter (also the actuation latch), DeviceLossLatch on the router, and
// StateCache's sink from control::make_reachability_sink(latch, worker),
// posting to a ReissueWorker whose re-assert is SafeStateController::reassert_for. The
// provider is a mock.
//
// Every call that could hang runs on a helper thread with a 5 s deadline. A
// hang prints HANG and exits with status 3, so it fails the test instead of
// wedging the run.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "control/call_router.hpp"
#include "control/device_loss_latch.hpp"
#include "control/reissue_worker.hpp"
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

template <typename F>
auto within_deadline(const char *what, F &&body) {
    using R = decltype(body());
    std::promise<R> done;
    auto done_f = done.get_future();
    std::thread worker([&done, &body] { done.set_value(body()); });
    if (done_f.wait_for(kDeadline) == std::future_status::timeout) {
        std::cout << "HANG: " << what << " did not return within " << kDeadline.count() << " s" << std::endl;
        std::fflush(nullptr);
        std::_Exit(3);  // the stuck thread holds a provider lock; nothing can be torn down
    }
    worker.join();
    return done_f.get();
}

struct ProviderCall {
    std::string function;
    double pwm;
};

}  // namespace

class ReissueOffProviderLockTest : public Test {
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
                auto *out = caps->add_functions();
                out->set_name("set_output");
                out->set_function_id(1);
                out->mutable_policy()->set_category(
                    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE);
                auto *arg = out->add_args();
                arg->set_name("pwm");
                arg->set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                arg->set_required(true);
                auto *status = caps->add_functions();
                status->set_name("read_status");
                status->set_function_id(2);
                status->mutable_policy()->set_category(
                    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_READ);
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);

        ON_CALL(*mock_provider, read_signals("heater", _, _, _))
            .WillByDefault(
                Invoke([this](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
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
        ON_CALL(*mock_provider, call("heater", _, _, _, _))
            .WillByDefault(Invoke([this](const std::string &, uint32_t, const std::string &function,
                                         const ValueMap &args, CallResponse &) {
                std::lock_guard<std::mutex> lock(calls_mutex);
                const auto pwm = args.find("pwm");
                calls.push_back({function, pwm == args.end() ? -1.0 : pwm->second.double_value()});
                return true;
            }));

        runtime::ModeTransitionCallConfig hook;
        hook.device_handle = "sim0/heater";
        hook.function_name = "set_output";
        hook.args["pwm"] = automation::ParameterValue{0.0};
        safety.safe_state.hooks.push_back(hook);

        // Runtime::init_core_services order and Runtime::wire_device_loss_latch.
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        call_router = std::make_unique<control::CallRouter>(*registry, *state_cache);
        safe_state_controller =
            std::make_unique<control::SafeStateController>(*registry, *call_router, *provider_registry, safety);
        call_router->set_actuation_latch(safe_state_controller.get());
        device_loss_latch = std::make_unique<control::DeviceLossLatch>();
        call_router->set_device_loss_latch(device_loss_latch.get());
        reissue_worker = std::make_unique<control::ReissueWorker>([this](const std::string &handle) {
            (void)safe_state_controller->reassert_for(handle);
            if (!reasserted.exchange(true)) {
                reassert_done.set_value();
            }
        });
        state_cache->set_device_reachability_sink(control::make_reachability_sink(*device_loss_latch, *reissue_worker));
        ASSERT_TRUE(state_cache->initialize());

        // A transport loss latches the device; it then answers again.
        std::ostringstream discarded;
        std::streambuf *old = std::cerr.rdbuf(discarded.rdbuf());
        state_cache->poll_device_now("sim0/heater", *provider_registry);
        std::cerr.rdbuf(old);
        ASSERT_TRUE(device_loss_latch->is_latched("sim0/heater"));
        fail_reads = false;
    }

    void TearDown() override { reissue_worker->stop(); }

    // The re-assert ran: its safe-state call reached the provider.
    void expect_reasserted_after(const std::string &first_function, double first_pwm) {
        auto done = reassert_done.get_future();
        if (done.wait_for(kDeadline) == std::future_status::timeout) {
            std::cout << "HANG: the safe-state re-assert did not complete within " << kDeadline.count() << " s"
                      << std::endl;
            std::fflush(nullptr);
            std::_Exit(3);
        }
        std::lock_guard<std::mutex> lock(calls_mutex);
        ASSERT_EQ(calls.size(), 2U);
        EXPECT_EQ(calls[0].function, first_function);
        EXPECT_EQ(calls[0].pwm, first_pwm);
        // The re-assert lands after the call that produced the return edge,
        // so the device ends in its safe state.
        EXPECT_EQ(calls[1].function, "set_output");
        EXPECT_EQ(calls[1].pwm, 0.0);
    }

    std::atomic<bool> fail_reads{true};
    std::atomic<bool> reasserted{false};
    std::mutex calls_mutex;
    std::vector<ProviderCall> calls;
    std::promise<void> reassert_done;

    runtime::SafetyConfig safety;
    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unique_ptr<state::StateCache> state_cache;
    std::unique_ptr<control::CallRouter> call_router;
    std::unique_ptr<control::SafeStateController> safe_state_controller;
    std::unique_ptr<control::DeviceLossLatch> device_loss_latch;
    std::unique_ptr<control::ReissueWorker> reissue_worker;
};

// An operator's manual call to the latched device is the first good read
// since the loss.
TEST_F(ReissueOffProviderLockTest, ManualCallWhosePostCallPollIsTheReturn) {
    control::CallRequest request;
    request.device_handle = "sim0/heater";
    request.function_name = "set_output";
    request.args["pwm"].set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
    request.args["pwm"].set_double_value(0.5);

    const auto result =
        within_deadline("the manual call", [&] { return call_router->execute_call(request, *provider_registry); });
    EXPECT_TRUE(result.success) << result.error_message;
    expect_reasserted_after("set_output", 0.5);
}

// An e-stop ladder call to the latched device is the first good read. The
// ladder also holds SafeStateController's mutex for its whole run.
TEST_F(ReissueOffProviderLockTest, EstopLadderCallWhosePostCallPollIsTheReturn) {
    const auto result = within_deadline("the e-stop", [&] { return safe_state_controller->trigger("test"); });
    EXPECT_EQ(result.kind, control::SafeStateKind::Hooks);
    EXPECT_TRUE(result.safe_state_achieved);
    expect_reasserted_after("set_output", 0.0);
}

// A non-actuating behaviour-tree call is not refused by the loss latch, so it
// can be the first good read too.
TEST_F(ReissueOffProviderLockTest, NonActuatingTreeCallWhosePostCallPollIsTheReturn) {
    control::CallRequest request;
    request.device_handle = "sim0/heater";
    request.function_name = "read_status";
    request.is_automated = true;
    request.is_behavior_tree = true;

    const auto result =
        within_deadline("the tree call", [&] { return call_router->execute_call(request, *provider_registry); });
    EXPECT_TRUE(result.success) << result.error_message;
    expect_reasserted_after("read_status", -1.0);
}
