#include "health/health_snapshot.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
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

    // Register `count` devices, each with a single default double signal "temp"
    // carrying the given provider-declared stale_after_ms (0 = unset).
    void RegisterDevices(int count, uint32_t stale_after_ms = 0) {
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
            .WillByDefault(Invoke([stale_after_ms](const std::string &id, DescribeDeviceResponse &response) {
                auto *device = response.mutable_device();
                device->set_device_id(id);
                device->set_label(id);
                auto *caps = response.mutable_capabilities();
                auto *sig = caps->add_signals();
                sig->set_signal_id("temp");
                sig->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                sig->set_poll_hint_hz(1.0);  // default => polled
                sig->set_stale_after_ms(stale_after_ms);
                return true;
            }));
        registry->discover_provider("sim0", *mock_provider);
    }

    // Poll once with the "temp" signal set to `quality`. When `sample_epoch_ms`
    // is set, the provider stamps that explicit timestamp so the signal age can
    // differ from the poll age (the cached-sample false-green case). Returns the
    // stamped poll time of dev0.
    std::chrono::system_clock::time_point PollWithQuality(anolis::deviceprovider::v1::SignalValue_Quality quality,
                                                          std::optional<int64_t> sample_epoch_ms = std::nullopt) {
        ON_CALL(*mock_provider, read_signals(_, _, _))
            .WillByDefault(Invoke([quality, sample_epoch_ms](const std::string &, const std::vector<std::string> &,
                                                             ReadSignalsResponse &response) {
                auto *v = response.add_values();
                v->set_signal_id("temp");
                v->set_quality(quality);
                v->mutable_value()->set_double_value(21.0);
                if (sample_epoch_ms) {
                    v->mutable_timestamp()->set_seconds(*sample_epoch_ms / 1000);
                    v->mutable_timestamp()->set_nanos(static_cast<int32_t>((*sample_epoch_ms % 1000) * 1000000));
                }
                return true;
            }));
        state_cache = std::make_unique<state::StateCache>(*registry, 500);
        EXPECT_TRUE(state_cache->initialize());
        state_cache->poll_once(*provider_registry);
        auto st = state_cache->get_device_state("sim0/dev0");
        EXPECT_TRUE(st != nullptr);
        return st->last_poll_time;
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
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr,
                                                 {500, 1000, 2000}, poll_time + std::chrono::milliseconds(1500));
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

// ---------------------------------------------------------------------------
// Phase 2 (#220): per-signal freshness/quality folded into device health.
// ---------------------------------------------------------------------------

namespace {
namespace adpp = anolis::deviceprovider::v1;

const health::DeviceHealthSnapshot *find_dev(const std::vector<health::ProviderHealthSnapshot> &snap,
                                             const std::string &device_id) {
    for (const auto &ps : snap) {
        for (const auto &ds : ps.devices) {
            if (ds.device_id == device_id) {
                return &ds;
            }
        }
    }
    return nullptr;
}
}  // namespace

TEST(SignalFreshness, EffectiveStaleAfterMs) {
    // The liveness stale bound is the floor; declared only extends trust beyond
    // it. This is what keeps the per-signal age check from re-introducing the
    // false-STALE storm (a serialized bus never age-flags before liveness).
    EXPECT_EQ(health::effective_signal_stale_after_ms(0, 5000), 5000);       // unset => liveness bound
    EXPECT_EQ(health::effective_signal_stale_after_ms(900, 8000), 8000);     // declared tighter is subsumed
    EXPECT_EQ(health::effective_signal_stale_after_ms(30000, 5000), 30000);  // declared looser extends trust
}

TEST_F(HealthSnapshotCollectorTest, RegistryIngestsSignalStaleAfterMs) {
    RegisterDevices(1, /*stale_after_ms=*/900);
    const auto devices = registry->get_devices_for_provider("sim0");
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].capabilities.signals_by_id.at("temp").stale_after_ms, 900u);
}

TEST_F(HealthSnapshotCollectorTest, FreshFaultSignalDrivesFault) {
    // Poll is fresh, but the cached sample is QUALITY_FAULT: the false-green case.
    RegisterDevices(1);
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_FAULT);
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(100));
    const auto *d = find_dev(snap, "dev0");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->health, "FAULT");
    EXPECT_EQ(d->fault_signal_count, 1u);
    EXPECT_EQ(d->stale_signal_count, 0u);
}

TEST_F(HealthSnapshotCollectorTest, DegradedQualityDrivesStale) {
    RegisterDevices(1);
    for (auto q : {adpp::SignalValue_Quality_QUALITY_STALE, adpp::SignalValue_Quality_QUALITY_UNKNOWN}) {
        auto poll_time = PollWithQuality(q);
        auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                     poll_time + std::chrono::milliseconds(100));
        const auto *d = find_dev(snap, "dev0");
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->health, "STALE");
        EXPECT_EQ(d->stale_signal_count, 1u);
        EXPECT_EQ(d->fault_signal_count, 0u);
    }
}

TEST_F(HealthSnapshotCollectorTest, UnspecifiedQualityIsNeutral) {
    // A fresh sample with unset quality (legacy provider) stays OK.
    RegisterDevices(1);
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_UNSPECIFIED);
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(100));
    const auto *d = find_dev(snap, "dev0");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->health, "OK");
    EXPECT_EQ(d->stale_signal_count, 0u);
    EXPECT_EQ(d->fault_signal_count, 0u);
}

TEST_F(HealthSnapshotCollectorTest, NormalAgeDoesNotAgeFlagBeforeLiveness) {
    // A signal aged like its poll (no stuck sample) must NOT be age-flagged
    // STALE before the liveness bound, even with a tight declared stale_after_ms
    // (ezo-like 900ms) — this is the anti-storm guarantee. QUALITY stays OK.
    RegisterDevices(2, /*stale_after_ms=*/900);  // N=2 => liveness warn=3000
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_OK);
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(1600));
    const auto *d = find_dev(snap, "dev0");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->health, "OK");  // 1600 > declared 900 but < liveness warn 3000
    EXPECT_EQ(d->stale_signal_count, 0u);
}

TEST_F(HealthSnapshotCollectorTest, FreshPollServingOldSampleIsStale) {
    // Poll age ~0 (fresh) but the provider stamped a decade-old sample timestamp:
    // signal age >> any threshold => STALE even though liveness is OK.
    RegisterDevices(1, /*stale_after_ms=*/60000);
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_OK, /*sample_epoch_ms=*/1600000000000);
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(50));
    const auto *d = find_dev(snap, "dev0");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->health, "STALE");
    EXPECT_EQ(d->stale_signal_count, 1u);
}

TEST_F(HealthSnapshotCollectorTest, FaultBeatsLivenessStale) {
    // Liveness would be STALE (age past stale=5000 at N=1) AND the signal is
    // FAULT: FAULT wins.
    RegisterDevices(1);
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_FAULT);
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(9000));
    EXPECT_EQ(HealthOf(snap, "dev0"), "FAULT");
}

TEST_F(HealthSnapshotCollectorTest, UnavailableBeatsFaultButCountsPopulated) {
    RegisterDevices(1);
    auto poll_time = PollWithQuality(adpp::SignalValue_Quality_QUALITY_FAULT);
    ON_CALL(*mock_provider, is_available()).WillByDefault(Return(false));
    auto snap = health::collect_providers_health(*provider_registry, *registry, *state_cache, nullptr, {500, 0, 0},
                                                 poll_time + std::chrono::milliseconds(100));
    const auto *d = find_dev(snap, "dev0");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->health, "UNAVAILABLE");   // availability wins the string
    EXPECT_EQ(d->fault_signal_count, 1u);  // ...but the count is still truthful
}
