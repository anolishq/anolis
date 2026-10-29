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

#ifndef _WIN32
// #549: the disposition a provider gets back is the one the process started
// with, saved once. A second call, as a second Runtime::initialize() makes,
// finds SIGPIPE already ignored and must not save that. Saved once per process,
// and ctest runs each case in a process of its own.
TEST(SignalHandlerTest, ASecondIgnoreKeepsTheSigpipeDispositionSavedFirst) {
    std::signal(SIGPIPE, SIG_DFL);

    SignalHandler::ignore_broken_pipe();
    SignalHandler::ignore_broken_pipe();

    EXPECT_EQ(SignalHandler::inherited_broken_pipe_disposition(), SIG_DFL);
    EXPECT_EQ(std::signal(SIGPIPE, SIG_IGN), SIG_IGN) << "SIGPIPE is not ignored after the calls";
}
#endif
