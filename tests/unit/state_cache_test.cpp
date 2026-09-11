#include "state/state_cache.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mocks/mock_provider_handle.hpp"
#include "provider/i_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"

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
            [this](const std::string &handle, bool reachable, anolis::deviceprovider::v1::Status_Code status) {
                events.push_back({handle, reachable, status});
            });
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
