#include "state/state_cache.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "control/device_loss_latch.hpp"
#include "events/event_emitter.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/i_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "runtime/signal_handler.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

class StateCacheTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        provider_registry = std::make_unique<provider::ProviderRegistry>();
        mock_provider = std::make_shared<StrictMock<MockProviderHandle>>();
        EXPECT_CALL(*mock_provider, provider_id()).WillRepeatedly(ReturnRef(mock_provider->_id));
        EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Return(true));
        provider_registry->add_provider("sim0", mock_provider);
    }

    void RegisterMockDevice() {
        // Mock list_devices
        EXPECT_CALL(*mock_provider, list_devices(_)).WillOnce(Invoke([](std::vector<Device> &devices) {
            Device dev;
            dev.set_device_id("dev1");
            dev.set_label("Test Device");
            devices.push_back(dev);
            return true;
        }));

        // Mock describe_device
        EXPECT_CALL(*mock_provider, describe_device("dev1", _))
            .WillOnce(Invoke([](const std::string &, DescribeDeviceResponse &response) {
                auto *device = response.mutable_device();
                device->set_device_id("dev1");
                device->set_label("Test Device");

                // Add a signal
                auto *caps = response.mutable_capabilities();
                auto *sig = caps->add_signals();
                sig->set_signal_id("temp");
                sig->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                sig->set_poll_hint_hz(1.0);  // Implies default
                return true;
            }));

        registry->discover_provider("sim0", *mock_provider);
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<MockProviderHandle> mock_provider;
    std::unique_ptr<state::StateCache> state_cache;
};

TEST_F(StateCacheTest, Initialization) {
    state_cache = std::make_unique<state::StateCache>(*registry, 100);
    EXPECT_TRUE(state_cache->initialize());
}

TEST_F(StateCacheTest, PollAndRead) {
    RegisterMockDevice();

    state_cache = std::make_unique<state::StateCache>(*registry, 100);
    EXPECT_TRUE(state_cache->initialize());

    // Expect polls
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(Invoke([](const std::string &, const std::vector<std::string> &ids, ReadSignalsResponse &response,
                            anolis::deviceprovider::v1::Status_Code &) {
            // Verify we are asked for "temp"
            bool asking_temp = false;
            for (const auto &id : ids)
                if (id == "temp") asking_temp = true;
            if (!asking_temp) {
                return false;
            }

            auto *v = response.add_values();
            v->set_signal_id("temp");
            v->mutable_value()->set_double_value(25.5);
            v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
            return true;
        }));

    state_cache->poll_once(*provider_registry);

    auto result = state_cache->get_signal_value("sim0/dev1", "temp");
    ASSERT_TRUE(result != nullptr);
    EXPECT_DOUBLE_EQ(result->value.double_value(), 25.5);
    EXPECT_FALSE(result->is_stale(std::chrono::seconds(2)));
}

TEST_F(StateCacheTest, Staleness) {
    RegisterMockDevice();
    state_cache = std::make_unique<state::StateCache>(*registry, 100);
    EXPECT_TRUE(state_cache->initialize());

    // 1. Initial State: Stale Quality
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                            anolis::deviceprovider::v1::Status_Code &) {
            auto *v = response.add_values();
            v->set_signal_id("temp");
            v->mutable_value()->set_double_value(25.5);
            v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE);
            return true;
        }));

    state_cache->poll_once(*provider_registry);
    auto result = state_cache->get_signal_value("sim0/dev1", "temp");
    ASSERT_TRUE(result != nullptr);
    EXPECT_TRUE(result->is_stale(std::chrono::seconds(2)));
}

TEST_F(StateCacheTest, TimeBasedStaleness) {
    RegisterMockDevice();
    state_cache = std::make_unique<state::StateCache>(*registry, 100);
    EXPECT_TRUE(state_cache->initialize());

    // 1. Poll with OK quality
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                            anolis::deviceprovider::v1::Status_Code &) {
            auto *v = response.add_values();
            v->set_signal_id("temp");
            v->mutable_value()->set_double_value(25.5);
            v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
            return true;
        }));

    // Perform poll
    state_cache->poll_once(*provider_registry);

    // Capture the time "now" effectively used by the poll (it uses system_clock internally for setting timestamp during
    // poll) Actually, poll_once sets the timestamp = now(). We can't easily inject time into poll_once without
    // refactoring StateCache::poll_once too, BUT we can use the time we pass to is_stale to simulate the passage of
    // time relative to whenever the poll happened.

    auto result = state_cache->get_signal_value("sim0/dev1", "temp");
    ASSERT_TRUE(result != nullptr);

    // Get the timestamp from the result (it's public in the struct usually, or accessible)
    // Wait, CachedSignalValue struct definition in header:
    // struct CachedSignalValue { ... time_point timestamp; ... }
    // It's a struct, so public by default.

    auto poll_time = result->timestamp;

    // Check at poll time -> Not stale
    EXPECT_FALSE(result->is_stale(std::chrono::seconds(2), poll_time));
    EXPECT_EQ(result->age(poll_time).count(), 0);

    // Check 1 second later -> Not stale (limit is 2000ms)
    EXPECT_FALSE(result->is_stale(std::chrono::seconds(2), poll_time + std::chrono::seconds(1)));

    // Check 3 seconds later -> Stale
    EXPECT_TRUE(result->is_stale(std::chrono::seconds(2), poll_time + std::chrono::seconds(3)));
    EXPECT_GE(result->age(poll_time + std::chrono::seconds(3)).count(), 3000);
}

TEST_F(StateCacheTest, ConcurrencyStress) {
    RegisterMockDevice();
    state_cache = std::make_unique<state::StateCache>(*registry, 10);  // Fast poll
    EXPECT_TRUE(state_cache->initialize());

    std::atomic<bool> running{true};
    std::atomic<bool> failed{false};

    // Writer Thread (Simulated Poll)
    std::thread writer([&]() {
        // Setup a persistent expectation that returns changing values
        EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
            .WillRepeatedly(Invoke([](const std::string &, const std::vector<std::string> &,
                                      ReadSignalsResponse &response, anolis::deviceprovider::v1::Status_Code &) {
                auto *v = response.add_values();
                v->set_signal_id("temp");
                v->mutable_value()->set_double_value(rand() % 100);
                v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                return true;
            }));

        while (running) {
            state_cache->poll_once(*provider_registry);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Reader Threads
    std::vector<std::thread> readers;
    readers.reserve(5);
    for (int i = 0; i < 5; ++i) {
        readers.emplace_back([&]() {
            while (running) {
                try {
                    auto res = state_cache->get_signal_value("sim0/dev1", "temp");
                    // Just access it to ensure no segfault
                    if (res && res->value.double_value() < -1) failed = true;
                } catch (...) {
                    failed = true;
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    running = false;

    writer.join();
    for (auto &t : readers) t.join();

    EXPECT_FALSE(failed);
}

TEST_F(StateCacheTest, InitializeIsIdempotentAndDoesNotDuplicatePollConfigs) {
    RegisterMockDevice();
    state_cache = std::make_unique<state::StateCache>(*registry, 100);

    EXPECT_TRUE(state_cache->initialize());
    EXPECT_TRUE(state_cache->initialize());

    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .Times(1)
        .WillOnce(Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                            anolis::deviceprovider::v1::Status_Code &) {
            auto *v = response.add_values();
            v->set_signal_id("temp");
            v->mutable_value()->set_double_value(42.0);
            v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
            return true;
        }));

    state_cache->poll_once(*provider_registry);
}

TEST_F(StateCacheTest, MissingProviderMarksCachedDeviceUnavailable) {
    RegisterMockDevice();
    state_cache = std::make_unique<state::StateCache>(*registry, 100);
    EXPECT_TRUE(state_cache->initialize());

    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .Times(1)
        .WillOnce(Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                            anolis::deviceprovider::v1::Status_Code &) {
            auto *v = response.add_values();
            v->set_signal_id("temp");
            v->mutable_value()->set_double_value(25.5);
            v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
            return true;
        }));

    state_cache->poll_once(*provider_registry);

    ASSERT_TRUE(provider_registry->remove_provider("sim0"));
    state_cache->poll_once(*provider_registry);

    auto state = state_cache->get_device_state("sim0/dev1");
    ASSERT_TRUE(state != nullptr);
    EXPECT_FALSE(state->provider_available);
}

//=============================================================================
// Device reachability sink (#285)
//=============================================================================

namespace {

struct ReachabilityEvent {
    std::string device_handle;
    bool reachable;
    anolis::deviceprovider::v1::Status_Code status;
    std::uint64_t edge_seq;
};

// A read that succeeds with one value, and one that fails with a given code.
auto good_read() {
    return Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                     anolis::deviceprovider::v1::Status_Code &status) {
        auto *v = response.add_values();
        v->set_signal_id("temp");
        v->mutable_value()->set_double_value(25.5);
        v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
        status = anolis::deviceprovider::v1::Status_Code_CODE_OK;
        return true;
    });
}

auto failed_read(anolis::deviceprovider::v1::Status_Code code) {
    return Invoke([code](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &,
                         anolis::deviceprovider::v1::Status_Code &status) {
        status = code;
        return false;
    });
}

}  // namespace

class StateCacheReachabilityTest : public StateCacheTest {
protected:
    void SetUp() override {
        StateCacheTest::SetUp();
        RegisterMockDevice();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        state_cache->set_device_reachability_sink(
            [this](const std::string &handle, bool reachable, anolis::deviceprovider::v1::Status_Code status,
                   std::uint64_t edge_seq) { events.push_back({handle, reachable, status, edge_seq}); });
        ASSERT_TRUE(state_cache->initialize());
        // The poll loop logs last_error() on a failed read.
        EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(Return("read failed"));
    }

    std::vector<ReachabilityEvent> events;
};

// The single most important property: the sink is told the code of the read
// that failed, NOT the provider's shared last-value field. A concurrent RPC on
// another thread (a health probe answering OK, a tree call) can overwrite that
// field between the failing read returning and the poll loop looking. Here
// the shared field says OK while the read itself says DEADLINE_EXCEEDED; the
// latch must see DEADLINE_EXCEEDED or a dark board is never latched.
TEST_F(StateCacheReachabilityTest, LossEdgeCarriesTheFailingReadsStatusNotTheSharedField) {
    EXPECT_CALL(*mock_provider, last_status_code())
        .WillRepeatedly(Return(anolis::deviceprovider::v1::Status_Code_CODE_OK));
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED));

    state_cache->poll_once(*provider_registry);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].device_handle, "sim0/dev1");
    EXPECT_FALSE(events[0].reachable);
    EXPECT_EQ(events[0].status, anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED);
}

// The poll loop re-reports a lost device every cycle. The sink must fire on
// the EDGE only; the latch it feeds is idempotent, but the safe-state re-issue
// on the return edge is not something to run every 2.5 s.
TEST_F(StateCacheReachabilityTest, FiresOncePerEdgeNotOncePerFailedPoll) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read())
        .WillOnce(good_read());

    for (int i = 0; i < 5; ++i) {
        state_cache->poll_once(*provider_registry);
    }

    ASSERT_EQ(events.size(), 2U);
    EXPECT_FALSE(events[0].reachable);
    EXPECT_EQ(events[0].status, anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE);
    EXPECT_TRUE(events[1].reachable);
    EXPECT_EQ(events[1].status, anolis::deviceprovider::v1::Status_Code_CODE_OK);
    // The consumer orders edges by this number (#403).
    EXPECT_GT(events[0].edge_seq, 0U);
    EXPECT_GT(events[1].edge_seq, events[0].edge_seq);
}

// A device that starts reachable and stays reachable produces no edge. In
// particular the FIRST successful poll must not fire "became reachable": that
// would re-issue safe state to every device at startup.
TEST_F(StateCacheReachabilityTest, NoEdgeWhileHealthy) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).Times(3).WillRepeatedly(good_read());

    for (int i = 0; i < 3; ++i) {
        state_cache->poll_once(*provider_registry);
    }

    EXPECT_TRUE(events.empty());
}

// The supervised-provider-restart path marks every device of the provider
// unavailable WITHOUT going through the sink. That is deliberate: the devices
// did not lose power, and latching on a supervised restart would recreate the
// spurious-re-arm problem the transport classification exists to avoid.
TEST_F(StateCacheReachabilityTest, ProviderUnavailableDoesNotFireTheSink) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).WillOnce(good_read());
    state_cache->poll_once(*provider_registry);

    ASSERT_TRUE(provider_registry->remove_provider("sim0"));
    state_cache->poll_once(*provider_registry);

    auto state = state_cache->get_device_state("sim0/dev1");
    ASSERT_TRUE(state != nullptr);
    EXPECT_FALSE(state->provider_available);
    EXPECT_TRUE(events.empty());
}

// A supervised provider restart rebuilds poll configs. The rebuilt state must
// inherit the device's reachability: if it was lost before the restart, its
// first successful poll afterwards IS the return edge, and resetting the flag
// to "reachable" during the rebuild would swallow it.
TEST_F(StateCacheReachabilityTest, ReturnEdgeSurvivesProviderRebuild) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read());

    state_cache->poll_once(*provider_registry);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_FALSE(events[0].reachable);

    state_cache->rebuild_poll_configs("sim0");
    auto state = state_cache->get_device_state("sim0/dev1");
    ASSERT_TRUE(state != nullptr);
    EXPECT_FALSE(state->provider_available) << "rebuild must not claim a device reachable before polling it";

    state_cache->poll_once(*provider_registry);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_TRUE(events[1].reachable);
    EXPECT_GT(events[1].edge_seq, events[0].edge_seq) << "a rebuild must not restart the edge numbers";
}

// An OUT-OF-BAND failure -- the provider itself did not answer and its session
// is now unhealthy -- is provider loss, not device loss, whichever thread
// notices it first. The device goes unavailable but the sink is not told, the
// same outcome as the poll loop's own is_available() check. The status code
// alone cannot make this distinction: a hung provider yields DEADLINE_EXCEEDED
// exactly like a dark board does.
TEST_F(StateCacheReachabilityTest, OutOfBandFailureDoesNotFireTheSink) {
    bool available = true;
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Invoke([&available] { return available; }));
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(Invoke([&available](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &,
                                      anolis::deviceprovider::v1::Status_Code &status) {
            // What ProviderHandle does on a transport failure: mark the session
            // unhealthy and report a transport-class code.
            available = false;
            status = anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
            return false;
        }));

    state_cache->poll_once(*provider_registry);

    auto state = state_cache->get_device_state("sim0/dev1");
    ASSERT_TRUE(state != nullptr);
    EXPECT_FALSE(state->provider_available);
    EXPECT_TRUE(events.empty());
}

// A board that is dark when its provider dies. Provider loss marks the device
// unavailable without a sink call; the supervisor restarts the provider; the
// first poll afterwards fails IN-BAND because the board is still dark. That
// is the loss edge, and it must fire -- deriving it from provider_available,
// which the restart carried over as false, would swallow it and the board
// would never latch for that outage.
TEST_F(StateCacheReachabilityTest, DeviceDarkAcrossProviderRestartStillLatches) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read());

    // Provider loss, seen by the poll loop: no sink call.
    ASSERT_TRUE(provider_registry->remove_provider("sim0"));
    state_cache->poll_once(*provider_registry);
    ASSERT_TRUE(events.empty());
    ASSERT_FALSE(state_cache->get_device_state("sim0/dev1")->provider_available);

    // Supervisor brings the provider back and rebuilds; the board is still dark.
    provider_registry->add_provider("sim0", mock_provider);
    state_cache->rebuild_poll_configs("sim0");
    state_cache->poll_once(*provider_registry);

    ASSERT_EQ(events.size(), 1U) << "the first in-band failure after a provider restart is the loss edge";
    EXPECT_FALSE(events[0].reachable);
    EXPECT_EQ(events[0].status, anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE);

    // And the board's return is still reported once.
    state_cache->poll_once(*provider_registry);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_TRUE(events[1].reachable);
}

//=============================================================================
// Read failures during shutdown (#350)
//
// On a stop, the providers get the signal at the same time as the runtime and
// exit under the poll loop. Those failed reads are the shutdown, not a device
// fault: no ERROR line and no loss edge. Before shutdown, the same failure is
// still both.
//=============================================================================

namespace {

std::string poll_once_capturing_log(state::StateCache &cache, provider::ProviderRegistry &providers) {
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    cache.poll_once(providers);
    std::cerr.rdbuf(old);
    return captured.str();
}

}  // namespace

class StateCacheShutdownTest : public StateCacheReachabilityTest {
protected:
    void SetUp() override {
        StateCacheReachabilityTest::SetUp();
        runtime::SignalHandler::install();
    }

    // install() clears the latch, so later tests start with no shutdown pending.
    void TearDown() override { runtime::SignalHandler::install(); }

    static void request_shutdown() {
        // raise() is synchronous: the handler has set the latch when it returns.
        std::raise(SIGTERM);
        ASSERT_TRUE(runtime::SignalHandler::is_shutdown_requested());
    }
};

TEST_F(StateCacheShutdownTest, ReadFailureBeforeShutdownLogsErrorAndReportsLoss) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE));

    const std::string log = poll_once_capturing_log(*state_cache, *provider_registry);

    EXPECT_NE(log.find("[ERROR] [StateCache] ReadSignals failed for dev1"), std::string::npos) << log;
    ASSERT_EQ(events.size(), 1U);
    EXPECT_FALSE(events[0].reachable);
}

TEST_F(StateCacheShutdownTest, ReadFailureAfterShutdownIsNotAnErrorOrALoss) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE));
    request_shutdown();

    const std::string log = poll_once_capturing_log(*state_cache, *provider_registry);

    EXPECT_EQ(log.find("[ERROR]"), std::string::npos) << log;
    EXPECT_TRUE(events.empty()) << "a read failure during shutdown must not report a device loss";
    auto state = state_cache->get_device_state("sim0/dev1");
    ASSERT_TRUE(state != nullptr);
    EXPECT_FALSE(state->provider_available) << "the failed read still invalidates the cached snapshot";
}

//=============================================================================
// device_availability events (#300)
//
// The event mirrors provider_available: one `available: false` on the first
// loss from either path (a failed read, or the provider found unavailable or
// missing by the poll loop, which is what a supervised restart looks like),
// one `available: true` on the first successful read after it. Nothing on
// the first poll, nothing per repeated failure, and no loss event once
// shutdown is requested -- so never a `true` without a `false` before it.
//=============================================================================

class StateCacheAvailabilityEventTest : public StateCacheShutdownTest {
protected:
    void SetUp() override {
        StateCacheShutdownTest::SetUp();
        emitter = std::make_shared<events::EventEmitter>(100, 4);
        subscription = emitter->subscribe();
        ASSERT_TRUE(subscription != nullptr);
        state_cache->set_event_emitter(emitter);
    }

    // Every device_availability event emitted so far, in order.
    std::vector<events::DeviceAvailabilityEvent> availability() {
        while (auto event = subscription->try_pop()) {
            all_events.push_back(*event);
            if (const auto *a = std::get_if<events::DeviceAvailabilityEvent>(&*event)) {
                seen.push_back(*a);
            }
        }
        return seen;
    }

    std::shared_ptr<events::EventEmitter> emitter;
    std::unique_ptr<events::Subscription> subscription;
    std::vector<events::DeviceAvailabilityEvent> seen;
    std::vector<events::Event> all_events;  // every event, in emission order
};

// A subscriber learns the device is back before it sees the values of the
// read that brought it back, so a value never arrives for a device the
// subscriber still believes is unavailable.
TEST_F(StateCacheAvailabilityEventTest, RecoveryPrecedesTheReadsStateUpdate) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read());

    state_cache->poll_once(*provider_registry);
    state_cache->poll_once(*provider_registry);
    availability();

    ASSERT_EQ(all_events.size(), 3U);
    const auto *loss = std::get_if<events::DeviceAvailabilityEvent>(&all_events[0]);
    const auto *recovery = std::get_if<events::DeviceAvailabilityEvent>(&all_events[1]);
    const auto *value = std::get_if<events::StateUpdateEvent>(&all_events[2]);
    ASSERT_TRUE(loss != nullptr && !loss->available);
    ASSERT_TRUE(recovery != nullptr) << "available:true must come before the read's state_update";
    EXPECT_TRUE(recovery->available);
    ASSERT_TRUE(value != nullptr);
    EXPECT_EQ(value->signal_id, "temp");
}

TEST_F(StateCacheAvailabilityEventTest, FailedReadEmitsOneLossAndOneRecovery) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read())
        .WillOnce(good_read());

    for (int i = 0; i < 5; ++i) {
        state_cache->poll_once(*provider_registry);
    }

    const auto events_seen = availability();
    ASSERT_EQ(events_seen.size(), 2U);
    EXPECT_EQ(events_seen[0].provider_id, "sim0");
    EXPECT_EQ(events_seen[0].device_id, "dev1");
    EXPECT_FALSE(events_seen[0].available);
    EXPECT_GT(events_seen[0].timestamp_ms, 0);
    EXPECT_EQ(events_seen[1].device_id, "dev1");
    EXPECT_TRUE(events_seen[1].available);
}

TEST_F(StateCacheAvailabilityEventTest, NoEventWhileHealthy) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).Times(3).WillRepeatedly(good_read());

    for (int i = 0; i < 3; ++i) {
        state_cache->poll_once(*provider_registry);
    }

    EXPECT_TRUE(availability().empty()) << "the first successful poll is not a recovery";
}

// A crashed provider's handle stays registered and reports unavailable until
// the supervisor replaces it. That path never reads, so the loss must come
// from mark_provider_devices_unavailable, or the restart's recovery event
// would arrive with no loss before it.
TEST_F(StateCacheAvailabilityEventTest, SupervisedRestartEmitsLossThenRecovery) {
    bool available = true;
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Invoke([&available] { return available; }));
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).WillOnce(good_read()).WillOnce(good_read());

    state_cache->poll_once(*provider_registry);
    ASSERT_TRUE(availability().empty());

    available = false;
    for (int i = 0; i < 3; ++i) {
        state_cache->poll_once(*provider_registry);
    }
    auto events_seen = availability();
    ASSERT_EQ(events_seen.size(), 1U) << "one loss for the outage, not one per poll";
    EXPECT_EQ(events_seen[0].device_id, "dev1");
    EXPECT_FALSE(events_seen[0].available);

    available = true;
    state_cache->rebuild_poll_configs("sim0");
    state_cache->poll_once(*provider_registry);

    events_seen = availability();
    ASSERT_EQ(events_seen.size(), 2U);
    EXPECT_TRUE(events_seen[1].available);
}

TEST_F(StateCacheAvailabilityEventTest, MissingProviderEmitsLoss) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).WillOnce(good_read());
    state_cache->poll_once(*provider_registry);

    ASSERT_TRUE(provider_registry->remove_provider("sim0"));
    state_cache->poll_once(*provider_registry);
    state_cache->poll_once(*provider_registry);

    const auto events_seen = availability();
    ASSERT_EQ(events_seen.size(), 1U);
    EXPECT_FALSE(events_seen[0].available);
}

// An out-of-band read failure is provider loss: no sink call, but the device
// is unavailable all the same, and the event follows provider_available.
TEST_F(StateCacheAvailabilityEventTest, OutOfBandReadFailureEmitsLoss) {
    bool available = true;
    EXPECT_CALL(*mock_provider, is_available()).WillRepeatedly(Invoke([&available] { return available; }));
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(Invoke([&available](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &,
                                      anolis::deviceprovider::v1::Status_Code &status) {
            available = false;
            status = anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
            return false;
        }));

    state_cache->poll_once(*provider_registry);
    state_cache->poll_once(*provider_registry);

    const auto events_seen = availability();
    ASSERT_EQ(events_seen.size(), 1U);
    EXPECT_FALSE(events_seen[0].available);
    EXPECT_TRUE(events.empty()) << "provider loss still does not reach the reachability sink";
}

TEST_F(StateCacheAvailabilityEventTest, NoLossEventAfterShutdownAndNoLoneRecovery) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillOnce(failed_read(anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE))
        .WillOnce(good_read());
    request_shutdown();

    poll_once_capturing_log(*state_cache, *provider_registry);
    ASSERT_FALSE(state_cache->get_device_state("sim0/dev1")->provider_available);
    state_cache->poll_once(*provider_registry);

    EXPECT_TRUE(availability().empty()) << "no loss after shutdown, so no recovery to report either";
}

TEST_F(StateCacheAvailabilityEventTest, NoLossEventWhenProviderGoesDownDuringShutdown) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _)).WillOnce(good_read());
    state_cache->poll_once(*provider_registry);
    request_shutdown();

    ASSERT_TRUE(provider_registry->remove_provider("sim0"));
    state_cache->poll_once(*provider_registry);

    EXPECT_TRUE(availability().empty());
}

// The poll thread and a control call's post-call refresh (poll_device_now)
// can read the same device at once. The availability edges must reach
// subscribers in the order the cache made them, or a subscriber is left
// believing the opposite of what the cache says. No reachability sink here:
// it runs outside the lock by design (#285) and would race a test vector.
namespace {
thread_local bool t_fail_reads = false;
}  // namespace

class StateCacheAvailabilityOrderTest : public StateCacheTest {
protected:
    void SetUp() override {
        StateCacheTest::SetUp();
        RegisterMockDevice();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        ASSERT_TRUE(state_cache->initialize());
        emitter = std::make_shared<events::EventEmitter>(100, 4);
        subscription = emitter->subscribe(events::EventFilter::all(), 100000);
        ASSERT_TRUE(subscription != nullptr);
        state_cache->set_event_emitter(emitter);
        EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(Return("read failed"));
    }

    std::shared_ptr<events::EventEmitter> emitter;
    std::unique_ptr<events::Subscription> subscription;
};

TEST_F(StateCacheAvailabilityOrderTest, EdgesStayOrderedAcrossThreads) {
    EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
        .WillRepeatedly(Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response,
                                  anolis::deviceprovider::v1::Status_Code &status) {
            if (t_fail_reads) {
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

    constexpr int kIterations = 2000;
    std::ostringstream discarded;  // one ERROR line per failed read
    std::streambuf *old = std::cerr.rdbuf(discarded.rdbuf());
    std::thread failing([this] {
        t_fail_reads = true;
        for (int i = 0; i < kIterations; ++i) {
            state_cache->poll_device_now("sim0/dev1", *provider_registry);
        }
    });
    std::thread succeeding([this] {
        for (int i = 0; i < kIterations; ++i) {
            state_cache->poll_device_now("sim0/dev1", *provider_registry);
        }
    });
    failing.join();
    succeeding.join();
    std::cerr.rdbuf(old);

    std::vector<bool> edges;
    while (auto event = subscription->try_pop()) {
        if (const auto *a = std::get_if<events::DeviceAvailabilityEvent>(&*event)) {
            edges.push_back(a->available);
        }
    }
    ASSERT_EQ(subscription->dropped_count(), 0U);
    ASSERT_FALSE(edges.empty()) << "the two threads never interleaved";

    // Alternating from false: the first index that breaks it, or -1.
    long first_bad = -1;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edges[i] != (i % 2 == 1)) {
            first_bad = static_cast<long>(i);
            break;
        }
    }
    EXPECT_EQ(first_bad, -1) << "out-of-order availability edge among " << edges.size();
    EXPECT_EQ(edges.back(), state_cache->get_device_state("sim0/dev1")->provider_available)
        << "the last edge a subscriber saw contradicts the cache";
}

//=============================================================================
// Reachability edges reaching the loss latch out of order (#403)
//
// The sink runs outside the cache's lock, and the poll loop and a post-call
// poll read the same device on different threads. A failed read can decide
// the loss edge, and a good read on the other thread decide and deliver the
// return edge, before the loss edge reaches the latch. The sink below is
// Runtime::wire_device_loss_latch's, with one hook that forces that order:
// the loss edge waits, after the cache has made it, until the return edge has
// been applied. No timing is involved.
//=============================================================================

class StateCacheEdgeOrderTest : public StateCacheTest {
protected:
    void SetUp() override {
        StateCacheTest::SetUp();
        RegisterMockDevice();
        state_cache = std::make_unique<state::StateCache>(*registry, 100);
        ASSERT_TRUE(state_cache->initialize());
        EXPECT_CALL(*mock_provider, last_error()).WillRepeatedly(Return("read failed"));
        EXPECT_CALL(*mock_provider, read_signals("dev1", _, _, _))
            .WillRepeatedly(Invoke([](const std::string &, const std::vector<std::string> &,
                                      ReadSignalsResponse &response, anolis::deviceprovider::v1::Status_Code &status) {
                if (t_fail_reads) {
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
    }

    // Runtime's handler: apply the edge, and count a safe-state re-issue
    // where Runtime would run one.
    void apply(const std::string &handle, bool reachable, anolis::deviceprovider::v1::Status_Code status,
               std::uint64_t edge_seq) {
        if (latch.apply_reachability_edge(handle, reachable, status, edge_seq)) {
            ++reissues;
        }
    }

    control::DeviceLossLatch latch;
    int reissues = 0;
};

TEST_F(StateCacheEdgeOrderTest, LateLossEdgeDoesNotLatchADeviceThatAnswered) {
    std::promise<void> loss_made;
    std::promise<void> return_applied;
    auto return_applied_f = return_applied.get_future();

    state_cache->set_device_reachability_sink([&](const std::string &handle, bool reachable,
                                                  anolis::deviceprovider::v1::Status_Code status,
                                                  std::uint64_t edge_seq) {
        if (!reachable) {
            loss_made.set_value();
            return_applied_f.wait();
            apply(handle, reachable, status, edge_seq);
            return;
        }
        apply(handle, reachable, status, edge_seq);
        return_applied.set_value();
    });

    std::ostringstream discarded;  // the failed read logs an ERROR line
    std::streambuf *old = std::cerr.rdbuf(discarded.rdbuf());
    std::thread failing([this] {
        t_fail_reads = true;
        state_cache->poll_device_now("sim0/dev1", *provider_registry);
    });
    loss_made.get_future().wait();
    state_cache->poll_device_now("sim0/dev1", *provider_registry);
    failing.join();
    std::cerr.rdbuf(old);

    ASSERT_TRUE(state_cache->get_device_state("sim0/dev1")->provider_available);
    EXPECT_FALSE(latch.is_latched("sim0/dev1")) << "a stale loss edge latched a device the cache says answered";
    EXPECT_EQ(reissues, 0);
}

// The same two edges delivered in the order they were made: the loss latches
// and the return asks for the safe-state re-issue. The order check must not
// drop current edges.
TEST_F(StateCacheEdgeOrderTest, EdgesInOrderLatchThenReissue) {
    state_cache->set_device_reachability_sink(
        [this](const std::string &handle, bool reachable, anolis::deviceprovider::v1::Status_Code status,
               std::uint64_t edge_seq) { apply(handle, reachable, status, edge_seq); });

    std::ostringstream discarded;
    std::streambuf *old = std::cerr.rdbuf(discarded.rdbuf());
    t_fail_reads = true;
    state_cache->poll_device_now("sim0/dev1", *provider_registry);
    t_fail_reads = false;
    std::cerr.rdbuf(old);
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));

    state_cache->poll_device_now("sim0/dev1", *provider_registry);
    EXPECT_TRUE(latch.is_latched("sim0/dev1")) << "a return does not re-arm; only MANUAL -> AUTO does";
    EXPECT_EQ(reissues, 1);
}
