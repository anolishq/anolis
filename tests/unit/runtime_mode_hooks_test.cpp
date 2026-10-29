// anolishq/anolis#542: mode-transition hooks through a real Runtime. The
// empty-inventory helper in --stop-chain mode lists gpio0, which has no
// functions, so a hook call to it fails in the call router, and motor0, whose
// set_pwm it answers OK and appends to <dir>/calls.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "automation/mode_manager.hpp"
#include "runtime/config.hpp"
#include "runtime/runtime.hpp"
#include "runtime/signal_handler.hpp"

namespace {

using anolis::automation::RuntimeMode;

const char *kHelperPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

constexpr const char *kFailingCall =
    "          - { device_handle: bread0/gpio0, function_name: set_pwm, args: { pwm: 0 } }\n";
constexpr const char *kMotorCall =
    "          - { device_handle: bread0/motor0, function_name: set_pwm, args: { pwm: 0 } }\n";

struct Rig {
    std::filesystem::path root;
    std::filesystem::path calls() const { return root / "calls"; }
};

// `body` is the tree's single root node.
Rig fresh_rig(const std::string &name, const std::string &body = "<AlwaysSuccess/>") {
    Rig rig{std::filesystem::path(::testing::TempDir()) / name};
    std::filesystem::remove_all(rig.root);
    std::filesystem::create_directories(rig.root / "config");
    std::ofstream(rig.root / "tree.xml") << "<root BTCPP_format=\"4\">\n  <BehaviorTree ID=\"MainTree\">\n    " << body
                                         << "\n  </BehaviorTree>\n</root>\n";
    return rig;
}

std::string calls_made(const Rig &rig) {
    std::ifstream in(rig.calls());
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

// `hooks` is the body of automation.mode_transition_hooks.before_transition;
// `automation_extra` adds keys to the automation block.
anolis::runtime::RuntimeConfig config_for(const Rig &rig, const std::string &hooks,
                                          const std::string &automation_extra = "") {
    const auto path = rig.root / "config" / "runtime.yaml";
    std::ofstream(path) << "runtime:\n  data_dir: \"" << (rig.root / "data").generic_string()
                        << "\"\nhttp:\n  enabled: false\nproviders:\n  - id: bread0\n    command: \"" << kHelperPath
                        << "\"\n    args: [\"--stop-chain\", \"" << rig.root.generic_string()
                        << "\"]\n    timeout_ms: 2000\n    hello_timeout_ms: 2000\n    ready_timeout_ms: 5000\n"
                           "polling:\n  interval_ms: 100\ntelemetry:\n  enabled: false\n"
                           "safety:\n  on_startup: leave\n"
                           "automation:\n  enabled: true\n  behavior_tree: \""
                        << (rig.root / "tree.xml").generic_string() << "\"\n"
                        << automation_extra << "  mode_transition_hooks:\n    before_transition:\n"
                        << hooks;
    anolis::runtime::RuntimeConfig config;
    std::string error;
    EXPECT_TRUE(anolis::runtime::load_config(path.string(), config, error)) << error;
    return config;
}

// A failing call in a fail_on_error hook must not stop the rest of the FAULT
// safe state: FAULT commits regardless, so a veto would only skip the calls
// after it, in that hook and in every later one.
void expect_fault_hooks_all_run(const std::string &test_name, const std::string &fail_on_error_line) {
    const auto rig = fresh_rig(test_name);
    anolis::runtime::Runtime runtime(config_for(rig, std::string("      - to: FAULT\n") + fail_on_error_line +
                                                         "        calls:\n" + kFailingCall + kMotorCall +
                                                         "      - to: FAULT\n        fail_on_error: false\n"
                                                         "        calls:\n" +
                                                         kMotorCall));
    ASSERT_FALSE(::testing::Test::HasFailure()) << "the config did not load";
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    auto *mode = runtime.get_mode_manager();
    ASSERT_NE(mode, nullptr);
    ASSERT_TRUE(mode->set_mode(RuntimeMode::MANUAL, error)) << error;
    ASSERT_EQ(calls_made(rig), "");

    EXPECT_TRUE(mode->set_mode(RuntimeMode::FAULT, error)) << error;
    EXPECT_EQ(mode->current_mode(), RuntimeMode::FAULT);
    EXPECT_EQ(calls_made(rig), "motor0/1\nmotor0/1\n");
    runtime.shutdown();
}

}  // namespace

TEST(RuntimeModeHooksTest, FaultHookFailureWithDefaultFailOnErrorStillIssuesTheRemainingCalls) {
    expect_fault_hooks_all_run("anolis-542-default", "");
}

TEST(RuntimeModeHooksTest, FaultHookFailureWithFailOnErrorTrueStillIssuesTheRemainingCalls) {
    expect_fault_hooks_all_run("anolis-542-explicit", "        fail_on_error: true\n");
}

// Any other target keeps the veto: the transition is refused and the calls
// after the failure are not issued.
TEST(RuntimeModeHooksTest, NonFaultHookFailureStillVetoesTheTransition) {
    const auto rig = fresh_rig("anolis-542-veto");
    anolis::runtime::Runtime runtime(
        config_for(rig, std::string("      - to: IDLE\n        calls:\n") + kFailingCall + kMotorCall));
    ASSERT_FALSE(::testing::Test::HasFailure()) << "the config did not load";
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    auto *mode = runtime.get_mode_manager();
    ASSERT_NE(mode, nullptr);
    ASSERT_TRUE(mode->set_mode(RuntimeMode::MANUAL, error)) << error;

    EXPECT_FALSE(mode->set_mode(RuntimeMode::IDLE, error));
    EXPECT_NE(error.find("bread0/gpio0 set_pwm"), std::string::npos) << error;
    EXPECT_EQ(mode->current_mode(), RuntimeMode::MANUAL);
    EXPECT_EQ(calls_made(rig), "");
    runtime.shutdown();
}

namespace {

// #596: a tree whose root returns SUCCESS or FAILURE after one device call.
constexpr const char *kOneShotSuccess =
    "<Sequence>\n      <CallDevice device_handle=\"bread0/motor0\" function_name=\"set_pwm\" "
    "args='json:{\"pwm\":0}'/>\n      <AlwaysSuccess/>\n    </Sequence>";
constexpr const char *kOneShotFailure =
    "<Sequence>\n      <CallDevice device_handle=\"bread0/motor0\" function_name=\"set_pwm\" "
    "args='json:{\"pwm\":0}'/>\n      <AlwaysFailure/>\n    </Sequence>";
// A FAULT hook, so the refuse-hookless gate admits AUTO. It never fires here.
const std::string kFaultHook = std::string("      - to: FAULT\n        calls:\n") + kMotorCall;

size_t count_of(const std::string &text, const std::string &needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// Runs the runtime's main loop, and so the BT tick loop, for its lifetime.
class MainLoop {
public:
    explicit MainLoop(anolis::runtime::Runtime &runtime) : runtime_(runtime) {
        anolis::runtime::SignalHandler::install();
        thread_ = std::thread([this] { runtime_.run(); });
    }
    MainLoop(const MainLoop &) = delete;
    MainLoop &operator=(const MainLoop &) = delete;
    ~MainLoop() {
        std::raise(SIGTERM);
        thread_.join();
        runtime_.shutdown();
        anolis::runtime::SignalHandler::install();
    }

private:
    anolis::runtime::Runtime &runtime_;
    std::thread thread_;
};

// Enters AUTO, lets the tree run for 600 ms (6 ticks at the default 10 Hz),
// and returns what the runtime logged meanwhile. The log is captured before the
// main loop starts and released after it joins, so no thread writes to it
// while it is swapped.
std::string run_in_auto(anolis::runtime::Runtime &runtime) {
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    {
        MainLoop loop(runtime);
        std::string error;
        auto *mode = runtime.get_mode_manager();
        EXPECT_NE(mode, nullptr);
        if (mode != nullptr) {
            EXPECT_TRUE(mode->set_mode(RuntimeMode::MANUAL, error)) << error;
            EXPECT_TRUE(mode->set_mode(RuntimeMode::AUTO, error)) << error;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    std::cerr.rdbuf(old);
    return captured.str();
}

}  // namespace

// Under on_terminal: stop, a root that returns SUCCESS ends the run: its call
// is made once, not once per tick.
TEST(RuntimeModeHooksTest, StopOnTerminalCallsAOneShotTreeOnce) {
    const auto rig = fresh_rig("anolis-596-stop-success", kOneShotSuccess);
    anolis::runtime::Runtime runtime(config_for(rig, kFaultHook, "  on_terminal: stop\n"));
    ASSERT_FALSE(::testing::Test::HasFailure()) << "the config did not load";
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;

    const std::string log = run_in_auto(runtime);

    EXPECT_EQ(calls_made(rig), "motor0/1\n") << log;
    EXPECT_EQ(count_of(log, "BT completed successfully"), 1u) << log;
}

TEST(RuntimeModeHooksTest, StopOnTerminalCallsATreeThatFailsOnce) {
    const auto rig = fresh_rig("anolis-596-stop-failure", kOneShotFailure);
    anolis::runtime::Runtime runtime(config_for(rig, kFaultHook, "  on_terminal: stop\n"));
    ASSERT_FALSE(::testing::Test::HasFailure()) << "the config did not load";
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;

    const std::string log = run_in_auto(runtime);

    EXPECT_EQ(calls_made(rig), "motor0/1\n") << log;
    EXPECT_EQ(count_of(log, "BT failed"), 1u) << log;
}

// The default, repeat, keeps re-ticking a terminal root, as control-loop trees
// rely on, but logs the terminal result once per change, not once per tick.
TEST(RuntimeModeHooksTest, RepeatOnTerminalReticksAndLogsTheResultOnce) {
    const auto rig = fresh_rig("anolis-596-repeat", kOneShotSuccess);
    anolis::runtime::Runtime runtime(config_for(rig, kFaultHook));
    ASSERT_FALSE(::testing::Test::HasFailure()) << "the config did not load";
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;

    const std::string log = run_in_auto(runtime);

    EXPECT_GE(count_of(calls_made(rig), "motor0/1\n"), 3u) << log;
    EXPECT_EQ(count_of(log, "BT completed successfully"), 1u) << log;
}
