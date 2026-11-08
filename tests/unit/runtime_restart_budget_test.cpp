// anolishq/anolis#556: restart_policy.timeout_ms bounds the whole restart
// attempt. Hello and WaitReady share it, so a replacement that cannot be ready
// within it is refused while it starts, not after it is up; unset, it is
// hello_timeout_ms + ready_timeout_ms, so a provider slower to be ready than
// the old 30 s default can be restarted when that leaves room for discovery.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>

#include <cerrno>
#endif

#include "runtime/config.hpp"
#include "runtime/runtime.hpp"
#include "runtime/signal_handler.hpp"

namespace {

const char *kEmptyInventoryProviderPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

// `restart_policy` is the YAML of the policy's own keys, indented under it.
std::string config_yaml(const std::string &runtime, const std::string &args, int hello_timeout_ms, int ready_timeout_ms,
                        const std::string &restart_policy) {
    return "runtime: " + runtime + R"(
http:
  enabled: false
providers:
  - id: bread0
    command: ")" +
           std::string(kEmptyInventoryProviderPath) +
           R"("
    args: [)" +
           args + R"(]
    timeout_ms: 2000
    hello_timeout_ms: )" +
           std::to_string(hello_timeout_ms) + R"(
    ready_timeout_ms: )" +
           std::to_string(ready_timeout_ms) + R"(
    restart_policy:
      enabled: true
      max_attempts: 1
      backoff_ms: [50]
      success_reset_ms: 100
)" + restart_policy +
           R"(polling:
  interval_ms: 200
telemetry:
  enabled: false
)";
}

std::filesystem::path scratch_dir(const std::string &name) {
    const auto dir = std::filesystem::path(::testing::TempDir()) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string quoted(const std::filesystem::path &path) { return "\"" + path.generic_string() + "\""; }

anolis::runtime::RuntimeConfig load(const std::filesystem::path &dir, const std::string &yaml) {
    const auto path = dir / "runtime.yaml";
    std::ofstream(path) << yaml;
    anolis::runtime::RuntimeConfig config;
    std::string error;
    EXPECT_TRUE(anolis::runtime::load_config(path.string(), config, error)) << error;
    return config;
}

// Runs the main loop until `done` returns true or `limit` passes, with the log
// captured. Returns whether `done` returned true. The log is read only after the
// loop has joined, so nothing reads it while the runtime writes it.
template <typename Done>
bool run_until(anolis::runtime::Runtime &runtime, const std::filesystem::path &trigger, std::chrono::seconds limit,
               Done done, std::string &log) {
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    // run() ends only on the signal latch; install() clears it before and after.
    anolis::runtime::SignalHandler::install();
    std::thread loop([&runtime] { runtime.run(); });
    std::ofstream(trigger) << "exit";  // the helper removes it and exits

    bool reached = false;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            reached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::raise(SIGTERM);
    loop.join();
    runtime.shutdown();
    anolis::runtime::SignalHandler::install();
    std::cerr.rdbuf(old);
    log = captured.str();
    return reached;
}

}  // namespace

// The issue's case: WaitReady takes longer than the old 30 s default of
// timeout_ms but well inside ready_timeout_ms. Boot accepts it, and with
// timeout_ms unset the restart now has hello_timeout_ms + ready_timeout_ms, so
// it succeeds. Before, every attempt was refused after start() and, with a
// budget of one attempt, the circuit opened for the 60 s probe interval.
TEST(RuntimeRestartBudgetTest, AProviderSlowerToBeReadyThanTheOldDefaultRestartsWithTheDefaultBudget) {
    const auto dir = scratch_dir("anolis-556-default-budget");
    const auto trigger = dir / "exit-now";
    const auto slow = dir / "slow-ready";
    const auto config = load(dir, config_yaml("{}",
                                              "\"--exit-when\", " + quoted(trigger) + ", \"--slow-ready-when\", " +
                                                  quoted(slow) + ", \"--ready-delay-ms\", \"31000\"",
                                              2000, 40000, ""));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    const auto first = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(first, nullptr);
    std::ofstream(slow) << "slow";  // boot was fast; every restart waits 31 s for WaitReady

    std::shared_ptr<anolis::provider::IProviderHandle> current;
    std::string log;
    const bool restarted = run_until(
        runtime, trigger, std::chrono::seconds(45),
        [&] {
            current = runtime.get_provider_registry().get_provider("bread0");
            return current != nullptr && current != first && current->is_available();
        },
        log);

    EXPECT_TRUE(restarted) << "the provider was not restarted within 45 s\n" << log;
    EXPECT_EQ(log.find("Restart timeout exceeded"), std::string::npos) << log;
    std::filesystem::remove_all(dir);
}

#ifndef _WIN32
namespace {

// The helpers' PIDs, from the files `--pid-dir` leaves.
std::vector<pid_t> launched(const std::filesystem::path &dir) {
    std::vector<pid_t> pids;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        pids.push_back(static_cast<pid_t>(std::stol(entry.path().filename().string())));
    }
    return pids;
}

// Gone, and reaped: a zombie still answers kill(pid, 0), so a replacement that
// was killed but never waited for counts as alive.
bool gone(pid_t pid) { return ::kill(pid, 0) == -1 && errno == ESRCH; }

}  // namespace

// An explicit timeout_ms smaller than hello_timeout_ms + ready_timeout_ms
// still bounds the attempt: WaitReady gets what Hello left of the 2 s, so the
// attempt is refused at about 2 s instead of when the helper answers at 6 s.
// The replacement it refused is stopped and reaped before the restart ends, so
// nothing of it runs on beside the provider entry supervision keeps retrying.
TEST(RuntimeRestartBudgetTest, ARestartThatCannotBeReadyWithinTimeoutMsIsRefusedAndItsReplacementReaped) {
    const auto dir = scratch_dir("anolis-556-refused");
    const auto pids = dir / "pids";
    std::filesystem::create_directories(pids);
    const auto trigger = dir / "exit-now";
    const auto slow = dir / "slow-ready";
    const auto config =
        load(dir, config_yaml("{shutdown_timeout_ms: 500}",
                              "\"--exit-when\", " + quoted(trigger) + ", \"--slow-ready-when\", " + quoted(slow) +
                                  ", \"--ready-delay-ms\", \"6000\", \"--pid-dir\", " + quoted(pids),
                              2000, 10000, "      timeout_ms: 2000\n"));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    const auto first = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(launched(pids).size(), 1u);
    const pid_t booted = launched(pids).front();
    std::ofstream(slow) << "slow";

    pid_t replacement = 0;
    std::chrono::steady_clock::time_point spawned;
    bool reaped = false;
    std::shared_ptr<anolis::provider::IProviderHandle> registered;
    std::string log;
    run_until(
        runtime, trigger, std::chrono::seconds(15),
        [&] {
            if (replacement == 0) {
                for (const pid_t pid : launched(pids)) {
                    if (pid != booted) {
                        replacement = pid;
                        spawned = std::chrono::steady_clock::now();
                    }
                }
                return false;
            }
            // 2 s of budget, 0.5 s for stdin EOF the helper ignores, then the
            // kill: about 3 s. Waiting out the 6 s WaitReady would take longer.
            reaped = gone(replacement);
            registered = runtime.get_provider_registry().get_provider("bread0");
            return reaped || std::chrono::steady_clock::now() - spawned > std::chrono::milliseconds(4500);
        },
        log);

    ASSERT_NE(replacement, 0) << "no replacement was launched\n" << log;
    EXPECT_TRUE(reaped) << "replacement " << replacement << " was still running 4.5 s after it was launched\n" << log;
    EXPECT_NE(log.find("Restart timeout exceeded for provider 'bread0'"), std::string::npos) << log;
    EXPECT_EQ(registered, first) << "a refused replacement was published";
    if (!gone(replacement)) {
        ::kill(replacement, SIGKILL);
    }
    std::filesystem::remove_all(dir);
}

// Discovery's RPCs wait their own timeout_ms, not what is left of the budget,
// so an attempt whose ListDevices outlasts the budget is checked once discovery
// is done: it is refused there, its inventory is not published, and the
// replacement is stopped.
TEST(RuntimeRestartBudgetTest, ARestartWhoseDiscoveryOutlastsTimeoutMsIsRefusedAfterDiscovery) {
    const auto dir = scratch_dir("anolis-556-discovery");
    const auto pids = dir / "pids";
    std::filesystem::create_directories(pids);
    const auto trigger = dir / "exit-now";
    const auto slow = dir / "slow";
    const auto config =
        load(dir, config_yaml("{shutdown_timeout_ms: 500}",
                              "\"--exit-when\", " + quoted(trigger) + ", \"--slow-ready-when\", " + quoted(slow) +
                                  ", \"--ready-delay-ms\", \"0\", \"--list-delay-ms\", \"1500\", "
                                  "\"--pid-dir\", " +
                                  quoted(pids),
                              2000, 10000, "      timeout_ms: 1000\n"));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    const auto first = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(launched(pids).size(), 1u);
    const pid_t booted = launched(pids).front();
    std::ofstream(slow) << "slow";  // boot listed at once; a restart's ListDevices takes 1.5 s

    pid_t replacement = 0;
    std::chrono::steady_clock::time_point spawned;
    bool reaped = false;
    std::shared_ptr<anolis::provider::IProviderHandle> registered;
    std::string log;
    run_until(
        runtime, trigger, std::chrono::seconds(15),
        [&] {
            if (replacement == 0) {
                for (const pid_t pid : launched(pids)) {
                    if (pid != booted) {
                        replacement = pid;
                        spawned = std::chrono::steady_clock::now();
                    }
                }
                return false;
            }
            reaped = gone(replacement);
            registered = runtime.get_provider_registry().get_provider("bread0");
            return reaped || std::chrono::steady_clock::now() - spawned > std::chrono::milliseconds(4500);
        },
        log);

    ASSERT_NE(replacement, 0) << "no replacement was launched\n" << log;
    EXPECT_NE(log.find("Restart timeout exceeded for provider 'bread0' after discovery"), std::string::npos) << log;
    EXPECT_EQ(registered, first) << "a replacement that overran its budget in discovery was published\n" << log;
    EXPECT_TRUE(reaped) << "replacement " << replacement << " was still running 4.5 s after it was launched";
    if (!gone(replacement)) {
        ::kill(replacement, SIGKILL);
    }
    std::filesystem::remove_all(dir);
}
#endif
