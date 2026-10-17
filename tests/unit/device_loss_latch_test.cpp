#include "control/device_loss_latch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using anolis::control::DeviceLossLatch;

TEST(DeviceLossLatchTest, StartsUnlatchedAndEngagesPerDevice) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.is_latched("bread0/dcmt0"));

    latch.engage("bread0/dcmt0");
    EXPECT_TRUE(latch.is_latched("bread0/dcmt0"));

    // A latch on one device says nothing about another. A machine whose pH probe
    // blipped must not have its impeller blocked.
    EXPECT_FALSE(latch.is_latched("bread0/dcmt1"));
}

TEST(DeviceLossLatchTest, EngageIsIdempotent) {
    DeviceLossLatch latch;
    latch.engage("bread0/dcmt0");
    latch.engage("bread0/dcmt0");
    latch.engage("bread0/dcmt0");

    // The poll loop re-reports a lost device every cycle; that must not
    // accumulate state or re-log on every pass.
    EXPECT_EQ(latch.latched().size(), 1U);
}

TEST(DeviceLossLatchTest, ReleaseAllReportsWhatItReleased) {
    DeviceLossLatch latch;
    latch.engage("bread0/dcmt0");
    latch.engage("bread0/dcmt1");

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

    std::thread writer([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            latch.engage("bread0/dcmt" + std::to_string(i % 4));
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
// latch out of order (#403). Each test below delivers them in a chosen order.
namespace {
constexpr auto kUnavailable = anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE;
constexpr auto kDeadline = anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
constexpr auto kInternal = anolis::deviceprovider::v1::Status_Code_CODE_INTERNAL;
constexpr auto kOk = anolis::deviceprovider::v1::Status_Code_CODE_OK;
}  // namespace

TEST(DeviceLossLatchEdgeTest, LossThenReturnInOrderLatchesAndAsksForReissue) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.is_latched("sim0/dev1"));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_TRUE(latch.is_latched("sim0/dev1")) << "a return does not re-arm";
}

TEST(DeviceLossLatchEdgeTest, LossOlderThanAnAppliedReturnIsDropped) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_FALSE(latch.is_latched("sim0/dev1")) << "a late loss latched a device that has since answered";
}

TEST(DeviceLossLatchEdgeTest, ReturnOlderThanAnAppliedLossIsDropped) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kDeadline, 3));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 2))
        << "a late return asked for safe state on a device that is gone again";
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 4));
}

// Order is per device: a newer edge for one device says nothing about how old
// another device's edge is.
TEST(DeviceLossLatchEdgeTest, EdgeOrderIsTrackedPerDevice) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 5));
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev2", false, kUnavailable, 3));
    EXPECT_TRUE(latch.is_latched("sim0/dev2"));
    EXPECT_FALSE(latch.is_latched("sim0/dev1"));
}

// A re-arm does not make an edge decided before it any newer.
TEST(DeviceLossLatchEdgeTest, ReleaseAllKeepsEdgeOrder) {
    DeviceLossLatch latch;
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 1));
    EXPECT_TRUE(latch.apply_reachability_edge("sim0/dev1", true, kOk, 3));
    latch.release_all();
    EXPECT_FALSE(latch.apply_reachability_edge("sim0/dev1", false, kUnavailable, 2));
    EXPECT_FALSE(latch.is_latched("sim0/dev1"));
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
