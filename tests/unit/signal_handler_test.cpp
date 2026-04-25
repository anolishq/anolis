#include "runtime/signal_handler.hpp"

#include <gtest/gtest.h>

#include <csignal>

using anolis::runtime::SignalHandler;

//=============================================================================
// SignalHandler tests
//
// Note: all tests share the same process-global static latch.  Each test
// calls install() first to ensure a known clean state (after the fix,
// install() resets the latch).  Tests are ordered so that any latch
// mutation is cleaned up by the next install() call.
//=============================================================================

TEST(SignalHandlerTest, IsShutdownRequestedFalseAfterInstall) {
    // A freshly installed handler must not report shutdown.
    SignalHandler::install();
    EXPECT_FALSE(SignalHandler::is_shutdown_requested());
}

TEST(SignalHandlerTest, InstallClearsStaleShutdownLatch) {
    // Arrange: install and fire a real signal to dirty the latch.
    // raise() is synchronous: by the time it returns the signal handler has
    // already executed shutdown_requested_.store(true).
    SignalHandler::install();
    raise(SIGTERM);
    ASSERT_TRUE(SignalHandler::is_shutdown_requested())
        << "Precondition: latch must be set to true after raise(SIGTERM)";

    // Act: re-install — this is the operation under test.
    SignalHandler::install();

    // Assert: stale latch from previous lifetime is cleared.
    // Before fix: install() never touches the latch → still true → FAILS.
    // After fix:  install() stores false before re-arming → false → PASSES.
    EXPECT_FALSE(SignalHandler::is_shutdown_requested());
}
