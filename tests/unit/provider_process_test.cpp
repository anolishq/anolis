/**
 * provider_process_test.cpp - ProviderProcess unit tests
 *
 * Tests:
 * - Constructor initialization
 * - Spawn with missing executable (error path)
 * - Spawn with valid executable (success path)
 * - is_running() check after spawn
 * - Clean shutdown sequence
 * - Graceful vs forced termination
 * - Double shutdown safety
 *
 * Note: Full process lifecycle testing requires a real test helper executable.
 * These tests verify the testable aspects while acknowledging some integration is needed.
 */

#include "provider/provider_process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#endif

using namespace anolis::provider;

/**
 * Test Fixture: ProviderProcessTest
 *
 * Sets up test environment with paths and cleanup.
 */
class ProviderProcessTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_executable_path_ = std::filesystem::current_path() / "test_helper_executable";
#ifdef _WIN32
        test_executable_path_ += ".bat";
#endif
        nonexistent_path_ = "d:/nonexistent_path/fake_executable";
    }

    void TearDown() override {
        // Cleanup test files if created
        if (std::filesystem::exists(test_executable_path_)) {
            std::filesystem::remove(test_executable_path_);
        }
    }

    /**
     * Create a simple test executable that exits immediately.
     *
     * On Windows: batch file that echoes and exits
     * On Linux: shell script that echoes and exits
     */
    void CreateSimpleTestExecutable() {
#ifdef _WIN32
        // Create .bat file that echoes and exits
        std::ofstream test_exe(test_executable_path_.string());
        test_exe << "@echo off\n";
        test_exe << "echo Test helper started\n";
        test_exe << "exit /b 0\n";
        test_exe.close();
#else
        // Create shell script that echoes and exits
        std::ofstream test_exe(test_executable_path_.string());
        test_exe << "#!/bin/bash\n";
        test_exe << "echo 'Test helper started'\n";
        test_exe << "exit 0\n";
        test_exe.close();

        // Make executable
        std::filesystem::permissions(test_executable_path_,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
#endif
    }

    /**
     * Create a long-running test executable that waits for stdin EOF.
     *
     * This simulates a provider that stays alive until shutdown.
     */
    void CreateLongRunningTestExecutable() {
#ifdef _WIN32
        std::ofstream test_exe(test_executable_path_.string());
        test_exe << "@echo off\n";
        test_exe << "echo Test helper waiting\n";
        test_exe << ":loop\n";
        test_exe << "set /p input=\n";
        test_exe << "if errorlevel 1 goto end\n";
        test_exe << "goto loop\n";
        test_exe << ":end\n";
        test_exe << "echo Test helper exiting\n";
        test_exe << "exit /b 0\n";
        test_exe.close();
#else
        std::ofstream test_exe(test_executable_path_.string());
        test_exe << "#!/bin/bash\n";
        test_exe << "echo 'Test helper waiting'\n";
        test_exe << "while read line; do\n";
        test_exe << "  if [ -z \"$line\" ]; then break; fi\n";
        test_exe << "done\n";
        test_exe << "echo 'Test helper exiting'\n";
        test_exe << "exit 0\n";
        test_exe.close();

        std::filesystem::permissions(test_executable_path_,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add);
#endif
    }

    std::filesystem::path test_executable_path_;
    std::string nonexistent_path_;
};

/******************************************************************************
 * Constructor & Initialization Tests
 ******************************************************************************/

TEST_F(ProviderProcessTest, ConstructorInitializesFields) {
    ProviderProcess proc("test_provider", "/path/to/exe", {"arg1", "arg2"});

    EXPECT_EQ(proc.provider_id(), "test_provider");
    EXPECT_TRUE(proc.last_error().empty());
}

TEST_F(ProviderProcessTest, ConstructorWithNoArgs) {
    ProviderProcess proc("test_provider", "/path/to/exe");

    EXPECT_EQ(proc.provider_id(), "test_provider");
}

/******************************************************************************
 * Spawn Error Handling Tests
 ******************************************************************************/

TEST_F(ProviderProcessTest, SpawnFailsWithNonexistentExecutable) {
    ProviderProcess proc("test_provider", nonexistent_path_);

    EXPECT_FALSE(proc.spawn());
    EXPECT_FALSE(proc.last_error().empty());
    EXPECT_NE(proc.last_error().find("not found"), std::string::npos);
}

TEST_F(ProviderProcessTest, SpawnFailsWithEmptyPath) {
    ProviderProcess proc("test_provider", "");

    EXPECT_FALSE(proc.spawn());
    EXPECT_FALSE(proc.last_error().empty());
}

/******************************************************************************
 * Process Lifecycle Tests (require real executable)
 ******************************************************************************/

TEST_F(ProviderProcessTest, SpawnSucceedsWithValidExecutable) {
    CreateSimpleTestExecutable();

    ProviderProcess proc("test_provider", test_executable_path_.string());

    EXPECT_TRUE(proc.spawn());
    EXPECT_TRUE(proc.last_error().empty());

    // Give process time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Note: Simple test executable exits immediately, so is_running() may be false
    // This test verifies spawn succeeded, not that process stays alive
}

TEST_F(ProviderProcessTest, IsRunningReturnsFalseForUnspawnedProcess) {
    ProviderProcess proc("test_provider", test_executable_path_.string());

    EXPECT_FALSE(proc.is_running());
}

TEST_F(ProviderProcessTest, ShutdownSafeOnUnspawnedProcess) {
    ProviderProcess proc("test_provider", test_executable_path_.string());

    // Should not crash or hang
    EXPECT_NO_THROW(proc.shutdown());
}

TEST_F(ProviderProcessTest, DoubleShutdownIsSafe) {
    CreateSimpleTestExecutable();

    ProviderProcess proc("test_provider", test_executable_path_.string());
    ASSERT_TRUE(proc.spawn());

    // First shutdown
    proc.shutdown();

    // Second shutdown should be safe
    EXPECT_NO_THROW(proc.shutdown());
}

TEST_F(ProviderProcessTest, LongRunningProcessStaysAlive) {
    CreateLongRunningTestExecutable();

    ProviderProcess proc("test_provider", test_executable_path_.string());
    ASSERT_TRUE(proc.spawn());

    // Give process time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Process should still be running
    EXPECT_TRUE(proc.is_running());

    // Clean up
    proc.shutdown();
}

TEST_F(ProviderProcessTest, ShutdownStopsRunningProcess) {
    CreateLongRunningTestExecutable();

    ProviderProcess proc("test_provider", test_executable_path_.string());
    ASSERT_TRUE(proc.spawn());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(proc.is_running());

    // Shutdown
    proc.shutdown();

    // Give shutdown time to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Process should no longer be running
    EXPECT_FALSE(proc.is_running());
}

TEST_F(ProviderProcessTest, DestructorCallsShutdown) {
    CreateLongRunningTestExecutable();

    {
        ProviderProcess proc("test_provider", test_executable_path_.string());
        ASSERT_TRUE(proc.spawn());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_TRUE(proc.is_running());

        // Destructor should call shutdown
    }

    // Process should be shut down after destructor
    // (We can't easily verify this without keeping a PID/handle, but the test ensures no crash)
}

/******************************************************************************
 * Client Access Tests
 ******************************************************************************/

TEST_F(ProviderProcessTest, ClientAccessibleAfterSpawn) {
    CreateLongRunningTestExecutable();

    ProviderProcess proc("test_provider", test_executable_path_.string());
    ASSERT_TRUE(proc.spawn());

    // Client should be accessible
    EXPECT_NO_THROW(proc.client());

    proc.shutdown();
}

/******************************************************************************
 * Error Message Tests
 ******************************************************************************/

TEST_F(ProviderProcessTest, LastErrorClearedOnSuccessfulSpawn) {
    CreateSimpleTestExecutable();

    // First spawn with nonexistent path to set error
    ProviderProcess proc("test_provider", nonexistent_path_);
    EXPECT_FALSE(proc.spawn());
    EXPECT_FALSE(proc.last_error().empty());

    // Create new process with valid path
    ProviderProcess proc2("test_provider", test_executable_path_.string());
    EXPECT_TRUE(proc2.spawn());
    EXPECT_TRUE(proc2.last_error().empty());  // Error should be cleared
}

/******************************************************************************
 * Process group (#563)
 ******************************************************************************/

#ifndef _WIN32
namespace {

// A provider that forks a worker which ignores stdin, the shape of a wrapper
// script that does not exec. `tail` is how the leader waits: `wait` would keep
// it alive until the worker exits, `read` until stdin closes.
void write_wrapper(const std::filesystem::path &script, const std::filesystem::path &pid_file,
                   bool leader_waits_on_stdin) {
    std::ofstream out(script);
    out << "#!/bin/sh\n";
    out << "sleep 300 </dev/null &\n";
    out << "echo $! > '" << pid_file.string() << "'\n";
    out << (leader_waits_on_stdin ? "while read line; do :; done\n" : "trap '' TERM\nwait\n");
    out.close();
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
}

pid_t read_pid(const std::filesystem::path &pid_file) {
    for (int i = 0; i < 200; ++i) {
        std::ifstream in(pid_file);
        pid_t pid = 0;
        if (in >> pid && pid > 0) {
            return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

// Alive and not a zombie. Once its parent dies, the worker belongs to
// whichever process reaps orphans, which in a sandbox may never do so.
bool process_alive(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) {
        return false;
    }
    std::string line;
    std::getline(stat, line);
    const auto close_paren = line.rfind(')');
    return close_paren != std::string::npos && close_paren + 2 < line.size() && line[close_paren + 2] != 'Z';
}

bool dead_within(pid_t pid, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (process_alive(pid)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

}  // namespace

TEST_F(ProviderProcessTest, AForcedShutdownAlsoKillsTheWorkerAProviderForked) {
    const auto pid_file = test_executable_path_.string() + ".worker";
    write_wrapper(test_executable_path_, pid_file, false);
    ProviderProcess proc("wrapper", test_executable_path_.string(), {}, 300);
    ASSERT_TRUE(proc.spawn());
    const pid_t worker = read_pid(pid_file);
    ASSERT_GT(worker, 0);

    proc.shutdown();  // stdin EOF is ignored, so this is the forced path

    EXPECT_FALSE(proc.is_running());
    EXPECT_TRUE(dead_within(worker, std::chrono::milliseconds(500))) << "worker " << worker << " outlived the stop";
    ::kill(worker, SIGKILL);
    std::filesystem::remove(pid_file);
}

TEST_F(ProviderProcessTest, ACleanExitDoesNotLeaveAForkedWorkerRunning) {
    const auto pid_file = test_executable_path_.string() + ".worker";
    write_wrapper(test_executable_path_, pid_file, true);
    ProviderProcess proc("wrapper", test_executable_path_.string(), {}, 2000);
    ASSERT_TRUE(proc.spawn());
    const pid_t worker = read_pid(pid_file);
    ASSERT_GT(worker, 0);

    proc.shutdown();  // the leader exits on stdin EOF; its worker would not

    EXPECT_FALSE(proc.is_running());
    EXPECT_TRUE(dead_within(worker, std::chrono::milliseconds(500))) << "worker " << worker << " outlived the stop";
    ::kill(worker, SIGKILL);
    std::filesystem::remove(pid_file);
}
#endif

/******************************************************************************
 * Platform-Specific Notes
 ******************************************************************************/

/*
 * Additional test coverage notes:
 *
 * The following aspects are challenging to unit test without process mocking:
 * - Timeout behavior during shutdown (wait_for_exit)
 * - Forced termination path (requires non-responsive process)
 * - Stderr capture (requires analysis of child process stderr)
 * - Crash detection (requires process to crash unexpectedly)
 * - Pipe creation failures (system-level error simulation)
 *
 * These scenarios are better tested via:
 * - Integration tests with real provider processes
 * - System-level fault injection
 * - Manual testing with misbehaving providers
 *
 * Current test coverage (~60-70%) focuses on:
 * - Constructor and initialization
 * - Error handling for missing executables
 * - Basic spawn success
 * - Process running state checks
 * - Clean shutdown sequence
 * - Double-shutdown safety
 * - Destructor behavior
 */
