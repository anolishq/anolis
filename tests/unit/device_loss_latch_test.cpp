#include "control/device_loss_latch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using anolis::control::DeviceLossLatch;

namespace {
constexpr auto kUnavailable = anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE;
constexpr auto kDeadline = anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
constexpr auto kInternal = anolis::deviceprovider::v1::Status_Code_CODE_INTERNAL;
constexpr auto kOk = anolis::deviceprovider::v1::Status_Code_CODE_OK;
}  // namespace

TEST(DeviceLossLatchTest, StartsUnlatchedAndEngagesPerDevice) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.is_latched("bread0/dcmt0"));

    latch.apply_reachability_edge("bread0/dcmt0", false, kUnavailable, 1);
    EXPECT_TRUE(latch.is_latched("bread0/dcmt0"));

    // A latch on one device says nothing about another. A machine whose pH probe
    // blipped must not have its impeller blocked.
    EXPECT_FALSE(latch.is_latched("bread0/dcmt1"));
}

TEST(DeviceLossLatchTest, EngageIsIdempotent) {
    DeviceLossLatch latch;
    latch.apply_reachability_edge("bread0/dcmt0", false, kUnavailable, 1);
    latch.apply_reachability_edge("bread0/dcmt0", true, kOk, 2);
    latch.apply_reachability_edge("bread0/dcmt0", false, kUnavailable, 3);

    // A second loss before a re-arm must not accumulate state or re-log.
    EXPECT_EQ(latch.latched().size(), 1U);
}

TEST(DeviceLossLatchTest, ReleaseAllReportsWhatItReleased) {
    DeviceLossLatch latch;
    latch.apply_reachability_edge("bread0/dcmt0", false, kUnavailable, 1);
    latch.apply_reachability_edge("bread0/dcmt1", false, kUnavailable, 2);

    auto released = latch.release_all();
    EXPECT_EQ(released.size(), 2U);
    EXPECT_FALSE(latch.is_latched("bread0/dcmt0"));
    EXPECT_FALSE(latch.is_latched("bread0/dcmt1"));

    // An operator re-arming a machine should be told which devices it covered,
    // so the release is reported rather than silent.
    EXPECT_NE(std::find(released.begin(), released.end(), "bread0/dcmt0"), released.end());
    EXPECT_NE(std::find(released.begin(), released.end(), "bread0/dcmt1"), released.end());

    EXPECT_TRUE(latch.release_all().empty());
}

// The poll thread writes; the automation thread reads through CallRouter.
TEST(DeviceLossLatchTest, ConcurrentEngageAndQueryAreSafe) {
    DeviceLossLatch latch;
    std::atomic<bool> stop{false};

    // Four devices, each seeing alternating loss/return edges in order.
    std::thread writer([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            const bool reachable = (i / 4) % 2 == 1;
            latch.apply_reachability_edge("bread0/dcmt" + std::to_string(i % 4), reachable,
                                          reachable ? kOk : kUnavailable, static_cast<std::uint64_t>(i) + 1);
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 2000; ++i) {
            (void)latch.is_latched("bread0/dcmt0");
            (void)latch.latched();
        }
    });

    writer.join();
    stop.store(true);
    reader.join();

    EXPECT_TRUE(latch.is_latched("bread0/dcmt0"));
}

// Reachability edges, numbered by StateCache under its lock, can reach the
// latch out of order (#403). A device's edges are DECIDED alternating loss,
// return, loss, ... with increasing numbers; each test below delivers such a
// sequence in a chosen arrival order, and every order must end where the
// in-order one does.

TEST(DeviceLossLatchEdgeTest, LossThenReturnInOrderLatchesAndAsksForReissue) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_TRUE(latch.is_latched("sim0/dev1")) << "a return does not re-arm";
}

// Decided L1, R2; arriving R2, L1. The return found the device unlatched and
// re-asserted nothing, so the late loss still latches (fail closed) and asks
// for the re-issue the in-order return would have made.
TEST(DeviceLossLatchEdgeTest, BothArrivalOrdersLatchAndReissueOnce) {
    for (const bool reversed : {false, true}) {
        DeviceLossLatch latch;
        int reissues = 0;
        auto loss = [&] { reissues += latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1) ? 1 : 0; };
        auto ret = [&] { reissues += latch.apply_reachability_edge("sim0/dev1", true, kOk, 2) ? 1 : 0; };
        if (reversed) {
            ret();
            loss();
        } else {
            loss();
            ret();
        }
        EXPECT_TRUE(latch.is_latched("sim0/dev1")) << (reversed ? "reversed" : "in order");
        EXPECT_EQ(reissues, 1) << (reversed ? "reversed" : "in order");
    }
}

// Decided L1 (INTERNAL), R2; arriving R2, L1. A decode failure does not latch
// in order, and does not latch late either.
TEST(DeviceLossLatchEdgeTest, StaleNonTransportLossDoesNotLatch) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kInternal, 1));
    EXPECT_FALSE(latch.is_latched("sim0/dev1"));
}

// A stale loss for a device that is still latched asks for nothing: the
// return that overtook it found the device latched and re-issued then.
// Decided L1, R2, L3, R4; arriving L1, R2, R4, L3.
TEST(DeviceLossLatchEdgeTest, StaleLossOnALatchedDeviceAsksForNothing) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 4)) << "R4 found the device latched";
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 3))
        << "already latched and already re-issued";
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
}

// A stale loss behind a newer loss latches but asks for no re-issue: the
// device is gone again, and its next return re-issues. Decided L1
// (UNAVAILABLE), R2, L3 (INTERNAL); arriving R2, L3, L1.
TEST(DeviceLossLatchEdgeTest, StaleLossBehindANewerLossDoesNotReissue) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kInternal, 3));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
}

// Decided L1, R2, L3, R4; arriving L1, L3, R2, R4. R2 was decided, then
// overtaken by L3 from the other thread. The late return must not re-assert
// safe state for a device that has gone again; R4 does.
TEST(DeviceLossLatchEdgeTest, ReturnOlderThanAnAppliedLossIsDropped) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kDeadline, 3));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2))
        << "a late return asked for safe state on a device that is gone again";
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 4));
}

// Order is per device: a newer edge for one device says nothing about how old
// another device's edge is. dev1 decided R5 (after an earlier loss); dev2
// decided L3, R4.
TEST(DeviceLossLatchEdgeTest, EdgeOrderIsTrackedPerDevice) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 5));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev2", false, kUnavailable, 3))
        << "dev2's first edge is current, and its device has no newer return to re-issue for";
    EXPECT_TRUE(latch.is_latched("sim0/dev2"));
    EXPECT_FALSE(latch.is_latched("sim0/dev1"));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev2", true, kOk, 4)) << "dev2's return is current";
}

// A re-arm does not make an edge decided before it any newer. Decided L1, R2;
// arriving R2, then a re-arm (MANUAL -> AUTO), then L1. The late loss is still
// stale: it latches and asks for the re-issue its return did not make.
TEST(DeviceLossLatchEdgeTest, ReleaseAllKeepsEdgeOrder) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    latch.release_all();
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
}

// Only a transport-level failure latches; a decode failure means the device
// answered. Moved here from Runtime's handler with the order check.
TEST(DeviceLossLatchEdgeTest, OnlyTransportFailuresLatch) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kInternal, 1));
    EXPECT_FALSE(latch.is_latched("sim0/dev1"));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2)) << "nothing latched, nothing to re-issue";

    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev2", false, kUnavailable, 3));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev3", false, kDeadline, 4));
    EXPECT_TRUE(latch.is_latched("sim0/dev2"));
    EXPECT_TRUE(latch.is_latched("sim0/dev3"));
}

//=============================================================================
// The sink Runtime installs (make_reachability_sink)
//=============================================================================

// The sink forwards each edge's number to the latch, and re-issues on the
// latch's say-so. Fed the numbers StateCache would give it, a loss then a
// return re-issues once; a sink that dropped or replaced the number would
// either never latch, or latch and never re-issue.
TEST(DeviceLossSinkTest, ForwardsTheEdgeNumberAndReissuesOnce) {
    DeviceLossLatch latch;
    std::vector<std::string> reissued;
    auto sink = anolis::control::make_reachability_sink(
        latch, [&reissued](const std::string &handle) { reissued.push_back(handle); });

    sink("sim0/dev1", false, kUnavailable, 1);
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
    EXPECT_TRUE(reissued.empty());

    sink("sim0/dev1", true, kOk, 2);
    ASSERT_EQ(reissued.size(), 1U);
    EXPECT_EQ(reissued[0], "sim0/dev1");

    // A second outage after a re-arm latches and re-issues again.
    latch.release_all();
    sink("sim0/dev1", false, kUnavailable, 3);
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
    sink("sim0/dev1", true, kOk, 4);
    EXPECT_EQ(reissued.size(), 2U);
}

// What a constant number does: StateCache never issues 0, and if every edge
// carried it, the first loss would latch only through the stale path and no
// return would ever re-issue. Pinned so that is what the latch, not the sink,
// decides.
TEST(DeviceLossSinkTest, ConstantZeroNumberNeverReissues) {
    DeviceLossLatch latch;
    int reissues = 0;
    auto sink = anolis::control::make_reachability_sink(latch, [&reissues](const std::string &) { ++reissues; });

    sink("sim0/dev1", false, kUnavailable, 0);
    sink("sim0/dev1", true, kOk, 0);
    EXPECT_TRUE(latch.is_latched("sim0/dev1")) << "fails closed";
    EXPECT_EQ(reissues, 0);
}
