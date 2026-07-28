#include "health/health_snapshot.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

// ---------------------------------------------------------------------------
// resolve_staleness_bounds — the cadence-derived formula (#220 phase 1).
// These are pure and carry the "no regression for fast buses" guarantee.
// ---------------------------------------------------------------------------

TEST(StalenessBounds, DefaultOneDeviceReproducesLegacyLadder) {
    // The historical hardcoded ladder was 2000/5000. At one device @ 500ms the
    // derived bound must reproduce it exactly so fast buses see no change.
    auto b = health::resolve_staleness_bounds({500, 0, 0}, 1);
    EXPECT_EQ(b.warn_ms, 2000);
    EXPECT_EQ(b.stale_ms, 5000);
}

TEST(StalenessBounds, ScalesWithDeviceCount) {
    // Serialized EZO bus at N=2 @ 500ms: cycle_budget = 1000 ->
    // warn = max(3*1000, 2000) = 3000, stale = max(8*1000, 5000) = 8000.
    auto b = health::resolve_staleness_bounds({500, 0, 0}, 2);
    EXPECT_EQ(b.warn_ms, 3000);
    EXPECT_EQ(b.stale_ms, 8000);
}

TEST(StalenessBounds, FloorsApplyForFastSmallBuses) {
    // 100ms poll, 1 device: derived 300/800 -> clamped up to the 2000/5000 floor.
    auto b = health::resolve_staleness_bounds({100, 0, 0}, 1);
    EXPECT_EQ(b.warn_ms, 2000);
    EXPECT_EQ(b.stale_ms, 5000);
}

TEST(StalenessBounds, ExplicitOverridesWin) {
    auto b = health::resolve_staleness_bounds({500, 1500, 9000}, 4);
    EXPECT_EQ(b.warn_ms, 1500);
    EXPECT_EQ(b.stale_ms, 9000);
}

TEST(StalenessBounds, OverrideWarnPastStaleIsClamped) {
    // Explicit warn override greater than the (derived) stale bound must never
    // sit past stale: warn is clamped down to stale.
    auto b = health::resolve_staleness_bounds({500, 9000, 0}, 1);
    EXPECT_EQ(b.stale_ms, 5000);
    EXPECT_EQ(b.warn_ms, 5000);
}

TEST(StalenessBounds, ZeroDeviceCountTreatedAsOne) {
    auto b = health::resolve_staleness_bounds({500, 0, 0}, 0);
    EXPECT_EQ(b.warn_ms, 2000);
    EXPECT_EQ(b.stale_ms, 5000);
}

// ---------------------------------------------------------------------------
// collect_providers_health — end-to-end liveness ladder with a controlled now.
// ---------------------------------------------------------------------------

class HealthSnapshotCollectorTest : public Test {
protected:
    void SetUp() override {
        registry = std::make_unique<registry::DeviceRegistry>();
        provider_registry = std::make_unique<provider::ProviderRegistry>();
        mock_provider = std::make_shared<NiceMock<MockProviderHandle>>();
        ON_CALL(*mock_provider, provider_id()).WillByDefault(ReturnRef(mock_provider->_id));
        ON_CALL(*mock_provider, is_available()).WillByDefault(Return(true));
        // No provider-reported ADPP health in these liveness tests.
        ON_CALL(*mock_provider, get_health(_)).WillByDefault(Return(false));
        provider_registry->add_provider("sim0", mock_provider);
    }

    // Register `count` devices, each with a single default double signal.
    void RegisterDevices(int count) {
        ON_CALL(*mock_provider, list_devices(_)).WillByDefault(Invoke([count](std::vector<Device> &devices) {
            for (int i = 0; i < count; ++i) {
                Device dev;
                dev.set_device_id("dev" + std::to_string(i));
                dev.set_label("Device " + std::to_string(i));
                devices.push_back(dev);
            }
            return true;
        }));
        ON_CALL(*mock_provider, describe_device(_, _))
            .WillByDefault(Invoke([](const std::string &id, DescribeDeviceResponse &response) {
                auto *device = response.mutable_device();
                device->set_device_id(id);
                device->set_label(id);
                auto *caps = response.mutable_capabilities();
                auto *sig = caps->add_signals();
                sig->set_signal_id("temp");
                sig->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                sig->set_poll_hint_hz(1.0);  // default => polled
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);
    }

    // Poll once so every device has a fresh last_poll_time, and return the
    // stamped poll time of dev0 so tests can offset `now` deterministically.
    std::chrono::system_clock::time_point PollAndGetPollTime() {
        ON_CALL(*mock_provider, read_signals(_, _, _))
            .WillByDefault(
                Invoke([](const std::string &, const std::vector<std::string> &, ReadSignalsResponse &response) {
                    auto *v = response.add_values();
                    v->set_signal_id("temp");
                    v->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                    v->mutable_value()->set_double_value(21.0);
                    return true;
                }));
        state_cache = std::make_unique<state::StateCache>(*registry, 500);
        EXPECT_TRUE(state_cache->initialize());
        state_cache->poll_once(*provider_registry);
        auto st = state_cache->get_device_state("sim0/dev0");
        EXPECT_TRUE(st != nullptr);
        return st->last_poll_time;
    }

    std::string HealthOf(const std::vector<health::ProviderHealthSnapshot> &snap, const std::string &device_id) {
        for (const auto &ps : snap) {
            for (const auto &ds : ps.devices) {
                if (ds.device_id == device_id) {
                    return ds.health;
                }
            }
        }
        return "<missing>";
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
    std::unique_ptr<provider::ProviderRegistry> provider_registry;
    std::shared_ptr<NiceMock<MockProviderHandle>> mock_provider;
    std::unique_ptr<state::StateCache> state_cache;
};

TEST_F(HealthSnapshotCollectorTest, SerializedBusNoLongerFlapsStale) {
    // Two devices @ 500ms => warn=3000. A 2.5s-old poll used to be WARNING (and
    // heading to STALE) under the fixed 2s/5s ladder; it must now read OK.
    RegisterDevices(2);
    auto poll_time = PollAndGetPollTime();
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(2500));
    EXPECT_EQ(HealthOf(snap, "dev0"), "OK");
}

TEST_F(HealthSnapshotCollectorTest, LivenessTiersTrackDerivedBounds) {
    // Two devices @ 500ms => warn=3000, stale=8000.
    RegisterDevices(2);
    auto poll_time = PollAndGetPollTime();

    auto at = [&](int age_ms) {
        auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                     poll_time + std::chrono::milliseconds(age_ms));
        return HealthOf(snap, "dev0");
    };

    EXPECT_EQ(at(1000), "OK");       // < warn
    EXPECT_EQ(at(4000), "WARNING");  // warn <= age < stale
    EXPECT_EQ(at(9000), "STALE");    // >= stale
}

TEST_F(HealthSnapshotCollectorTest, ExplicitOverrideDrivesTiers) {
    RegisterDevices(1);
    auto poll_time = PollAndGetPollTime();
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 1000, 2000},
                                                 poll_time + std::chrono::milliseconds(1500));
    EXPECT_EQ(HealthOf(snap, "dev0"), "WARNING");  // 1000 <= 1500 < 2000
}

TEST_F(HealthSnapshotCollectorTest, ProviderUnavailableReportsUnavailable) {
    RegisterDevices(1);
    auto poll_time = PollAndGetPollTime();
    ON_CALL(*mock_provider, is_available()).WillByDefault(Return(false));
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(100));
    EXPECT_EQ(HealthOf(snap, "dev0"), "UNAVAILABLE");
}
