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
