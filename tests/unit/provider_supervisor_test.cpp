/**
 * @file provider_supervisor_test.cpp
 * @brief Unit tests for ProviderSupervisor snapshot API and synchronization.
 *
 * Tests:
 * - get_snapshot returns nullopt for unknown providers
 * - record_heartbeat populates last_seen_ago_ms
 * - recovery reset semantics (attempt_count reset after stability window)
 * - next_restart_in_ms semantics across all
 * four states (healthy, backoff, eligible, circuit-open)
 * - get_all_snapshots collects all registered providers
 * - Concurrent reads and writes do not race (run under TSAN in CI)
 */

#include "provider/provider_supervisor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "provider/provider_config.hpp"

using namespace anolis;
using namespace anolis::provider;

namespace {

RestartPolicyConfig make_enabled_policy(int max_attempts = 3, std::vector<int> backoff_ms = {100, 200, 500}) {
    RestartPolicyConfig policy;
    policy.enabled = true;
    policy.max_attempts = max_attempts;
    policy.backoff_ms = std::move(backoff_ms);
    policy.timeout_ms = 5000;
    policy.success_reset_ms = 1000;
    return policy;
}

RestartPolicyConfig make_disabled_policy() {
    RestartPolicyConfig policy;
    policy.enabled = false;
    return policy;
}

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot returns nullopt for unregistered provider
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, SnapshotForUnknownProviderReturnsNullopt) {
    ProviderSupervisor sup;
    auto snap = sup.get_snapshot("nonexistent");
    EXPECT_FALSE(snap.has_value());
}

TEST(ProviderSupervisorTest, GetAllSnapshotsEmptyBeforeRegistration) {
    ProviderSupervisor sup;
    auto snaps = sup.get_all_snapshots();
    EXPECT_TRUE(snaps.empty());
}

// ---------------------------------------------------------------------------
// record_heartbeat populates last_seen_ago_ms and uptime_seconds
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, BeforeHeartbeatLastSeenAgoMsIsNullopt) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy());

    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    EXPECT_FALSE(snap->last_seen_ago_ms.has_value());
    EXPECT_EQ(int64_t{0}, snap->uptime_seconds);
}

TEST(ProviderSupervisorTest, AfterHeartbeatLastSeenAgoMsIsPresent) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy());

    sup.record_heartbeat("prov");

    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    ASSERT_TRUE(snap->last_seen_ago_ms.has_value());
    EXPECT_GE(snap->last_seen_ago_ms.value(), int64_t{0});
    EXPECT_GE(snap->uptime_seconds, int64_t{0});
}

TEST(ProviderSupervisorTest, HeartbeatDoesNotAffectUnknownProvider) {
    ProviderSupervisor sup;
    // Should not crash when provider is not registered
    sup.record_heartbeat("unknown");
    auto snap = sup.get_snapshot("unknown");
    EXPECT_FALSE(snap.has_value());
}

// ---------------------------------------------------------------------------
// Recovery reset semantics
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, RecordSuccessResetsAttemptCount) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy());

    sup.record_crash("prov");
    {
        auto snap = sup.get_snapshot("prov");
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(1, snap->attempt_count);
    }

    sup.record_success("prov");
    {
        auto snap = sup.get_snapshot("prov");
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(0, snap->attempt_count);
    }
}

TEST(ProviderSupervisorTest, CrashResetsProcessStartTimeForRecoveryWindow) {
    ProviderSupervisor sup;
    auto policy = make_enabled_policy();
    policy.success_reset_ms = 200;
    sup.register_provider("prov", policy);

    // Establish initial healthy heartbeat and let it age past recovery window.
    sup.record_heartbeat("prov");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Crash must reset process_start_time so a fresh heartbeat starts a new window.
    sup.record_crash("prov");
    sup.record_heartbeat("prov");
    EXPECT_FALSE(sup.should_mark_recovered("prov"));
}

TEST(ProviderSupervisorTest, ShouldMarkRecoveredAfterStabilityWindow) {
    ProviderSupervisor sup;
    auto policy = make_enabled_policy();
    policy.success_reset_ms = 0;
    sup.register_provider("prov", policy);

    sup.record_crash("prov");
    sup.record_heartbeat("prov");

    EXPECT_TRUE(sup.should_mark_recovered("prov"));
}

TEST(ProviderSupervisorTest, RecordSuccessCloseCircuitBreaker) {
    ProviderSupervisor sup;
    // max_attempts=1, so two crashes open the circuit
    sup.register_provider("prov", make_enabled_policy(1, {1}));
    sup.record_crash("prov");
    sup.record_crash("prov");
    {
        auto snap = sup.get_snapshot("prov");
        ASSERT_TRUE(snap.has_value());
        EXPECT_TRUE(snap->circuit_open);
    }

    sup.record_success("prov");
    {
        auto snap = sup.get_snapshot("prov");
        ASSERT_TRUE(snap.has_value());
        EXPECT_FALSE(snap->circuit_open);
        EXPECT_EQ(0, snap->attempt_count);
    }
}

// ---------------------------------------------------------------------------
// next_restart_in_ms semantics — all four cases
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, NextRestartInMsIsNulloptWhenHealthy) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy());
    sup.record_heartbeat("prov");

    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    // Healthy, no crashes: no restart scheduled
    EXPECT_FALSE(snap->next_restart_in_ms.has_value());
}

TEST(ProviderSupervisorTest, NextRestartInMsIsPositiveWhileInBackoffWindow) {
    ProviderSupervisor sup;
    // Use long backoffs so the window is clearly still open when we snapshot
    sup.register_provider("prov", make_enabled_policy(3, {30000, 60000, 120000}));
    sup.record_crash("prov");

    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    EXPECT_FALSE(snap->circuit_open);
    ASSERT_TRUE(snap->next_restart_in_ms.has_value());
    EXPECT_GT(snap->next_restart_in_ms.value(), int64_t{0});
}

TEST(ProviderSupervisorTest, NextRestartInMsIsZeroWhenEligibleNow) {
    ProviderSupervisor sup;
    // backoff_ms=[0] so restart is immediately eligible after crash
    sup.register_provider("prov", make_enabled_policy(3, {0, 0, 0}));
    sup.record_crash("prov");

    // With 0ms backoff, next_restart_time == now at crash time, so should_restart() = true
    // and next_restart_in_ms = 0 (past deadline → report 0)
    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    ASSERT_TRUE(snap->next_restart_in_ms.has_value());
    EXPECT_EQ(int64_t{0}, snap->next_restart_in_ms.value());
}

TEST(ProviderSupervisorTest, NextRestartInMsIsNulloptWhenCircuitOpen) {
    ProviderSupervisor sup;
    // max_attempts=1: two crashes → circuit opens
    sup.register_provider("prov", make_enabled_policy(1, {1}));
    sup.record_crash("prov");
    sup.record_crash("prov");

    auto snap = sup.get_snapshot("prov");
    ASSERT_TRUE(snap.has_value());
    EXPECT_TRUE(snap->circuit_open);
    // circuit_open → no further restart ever: null to distinguish from healthy null
    EXPECT_FALSE(snap->next_restart_in_ms.has_value());
}

// ---------------------------------------------------------------------------
// get_all_snapshots correctness
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, GetAllSnapshotsReturnsAllRegistered) {
    ProviderSupervisor sup;
    sup.register_provider("prov_a", make_enabled_policy());
    sup.register_provider("prov_b", make_disabled_policy());

    auto snaps = sup.get_all_snapshots();
    ASSERT_EQ(2u, snaps.size());
    ASSERT_TRUE(snaps.count("prov_a") > 0);
    ASSERT_TRUE(snaps.count("prov_b") > 0);
    EXPECT_TRUE(snaps.at("prov_a").supervision_enabled);
    EXPECT_FALSE(snaps.at("prov_b").supervision_enabled);
}

TEST(ProviderSupervisorTest, GetAllSnapshotsReflectsCurrentState) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy(3, {30000, 60000, 120000}));
    sup.record_crash("prov");

    auto snaps = sup.get_all_snapshots();
    ASSERT_EQ(1u, snaps.size());
    const auto &snap = snaps.at("prov");
    EXPECT_EQ(1, snap.attempt_count);
    EXPECT_TRUE(snap.next_restart_in_ms.has_value());
    EXPECT_GT(snap.next_restart_in_ms.value(), int64_t{0});
}

// ---------------------------------------------------------------------------
// Concurrent reads and writes — validates full mutex coverage (TSAN)
// ---------------------------------------------------------------------------

TEST(ProviderSupervisorTest, ConcurrentReadsAndWritesAreThreadSafe) {
    ProviderSupervisor sup;
    sup.register_provider("prov", make_enabled_policy());

    const int kIterations = 300;

    // Writer threads: interleave heartbeats, crash records, and success records
    std::vector<std::thread> writers;
    writers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        writers.emplace_back([&sup, i, iterations = kIterations]() {
            for (int j = 0; j < iterations; ++j) {
                if (j % 7 != 0 && j % 13 == 0) {
                    // Alternate crash/success to keep state oscillating
                    if (i % 2 == 0) {
                        sup.record_crash("prov");
                    } else {
                        sup.record_success("prov");
                    }
                } else {
                    sup.record_heartbeat("prov");
                }
            }
        });
    }

    // Reader threads: take snapshots and get_all_snapshots concurrently
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&sup, iterations = kIterations]() {
            for (int j = 0; j < iterations; ++j) {
                if (j % 2 == 0) {
                    auto snap = sup.get_snapshot("prov");
                    (void)snap;
                } else {
                    auto snaps = sup.get_all_snapshots();
                    (void)snaps;
                }
            }
        });
    }

    for (auto &t : writers) t.join();
    for (auto &t : readers) t.join();

    // No assertions needed — TSAN races are detected during execution.
    // If this test passes under -fsanitize=thread, the mutex coverage is correct.
}

// ---------------------------------------------------------------------------
// F4: Backoff vector bounds safety
// ---------------------------------------------------------------------------

// record_crash with backoff_ms shorter than max_attempts must not OOB-access
// the vector. Before the fix, attempt 2 reads past the end of a 1-entry vector.
// Under ASan this faults; on plain builds it is UB (and returns garbage/0).
// FAILS before fix under ASan/UBSan.
TEST(ProviderSupervisorTest, RecordCrashWithShortBackoffVectorDoesNotCrash) {
    ProviderSupervisor sup;
    RestartPolicyConfig policy;
    policy.enabled = true;
    policy.max_attempts = 3;
    policy.backoff_ms = {500};  // 1 entry — mismatched; OOB before fix on attempt 2+
    policy.timeout_ms = 5000;
    policy.success_reset_ms = 1000;
    sup.register_provider("prov", policy);

    EXPECT_TRUE(sup.record_crash("prov"));   // attempt 1: index 0 — always safe
    EXPECT_TRUE(sup.record_crash("prov"));   // attempt 2: index 1 — OOB before fix
    EXPECT_TRUE(sup.record_crash("prov"));   // attempt 3: index 2 — OOB before fix
    EXPECT_FALSE(sup.record_crash("prov"));  // attempt 4 > max_attempts: circuit opens
    EXPECT_TRUE(sup.is_circuit_open("prov"));
}

// After fix, the clamped backoff for all out-of-range attempts must equal the
// last entry in backoff_ms. get_backoff_ms must reflect the same clamped value.
// FAILS before fix: get_backoff_ms returns 0 (its own OOB guard) rather than 500,
// or record_crash crashes under sanitizers preventing get_backoff_ms from running.
TEST(ProviderSupervisorTest, RecordCrashWithShortBackoffVectorUsesLastEntryAsBackoff) {
    ProviderSupervisor sup;
    RestartPolicyConfig policy;
    policy.enabled = true;
    policy.max_attempts = 3;
    policy.backoff_ms = {9999};  // 1 entry; all clamped attempts should use 9999ms
    policy.timeout_ms = 5000;
    policy.success_reset_ms = 1000;
    sup.register_provider("prov", policy);

    // Attempt 1 (index 0 — in range): backoff must be 9999ms
    sup.record_crash("prov");
    EXPECT_EQ(9999, sup.get_backoff_ms("prov"));

    sup.record_success("prov");

    // Attempt 1 + 2 after reset: attempt 2 is OOB before fix
    sup.record_crash("prov");
    sup.record_crash("prov");
    // After fix: clamped to last entry (9999ms)
    // Before fix: get_backoff_ms returns 0 (its own guard path) — FAILS
    EXPECT_EQ(9999, sup.get_backoff_ms("prov"));
}
