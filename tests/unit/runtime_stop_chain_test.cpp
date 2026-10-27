// anolishq/anolis#483: observing the emergency stop through safety.stop_chain.
// A real Runtime against the empty-inventory helper in --stop-chain mode:
// gpio0/chain_open follows a file the test writes, and every device call the
// helper receives is appended to another, so a hold can be shown to send none.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include "control/device_loss_latch.hpp"
#include "events/event_types.hpp"
#include "runtime/config.hpp"
#include "runtime/runtime.hpp"
#include "runtime/signal_handler.hpp"

namespace {

using anolis::control::StopChainState;

const char *kHelperPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

struct Rig {
    std::filesystem::path root;
    std::filesystem::path chain() const { return root / "chain"; }
    std::filesystem::path calls() const { return root / "calls"; }
};

Rig fresh_rig(const std::string &name) {
    Rig rig{std::filesystem::path(::testing::TempDir()) / name};
    std::filesystem::remove_all(rig.root);
    std::filesystem::create_directories(rig.root / "config");
    return rig;
}

// Retries a filesystem step for up to a second. The helper opens `chain` for
// a moment on every poll, and Windows refuses to replace or remove a file
// another process has open; POSIX does not, so there the first try succeeds.
void retry_briefly(const std::function<void(std::error_code &)> &step) {
    std::error_code ec;
    for (int attempt = 0; attempt < 100; ++attempt) {
        ec.clear();
        step(ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::filesystem::filesystem_error("set_chain", ec);
}

// Replaced by rename, so the helper never reads a half-written file. An empty
// value removes the file.
void set_file(const Rig &rig, const std::string &name, const std::string &value) {
    const auto path = rig.root / name;
    if (value.empty()) {
        retry_briefly([&](std::error_code &ec) { std::filesystem::remove(path, ec); });
        return;
    }
    const auto tmp = rig.root / (name + ".tmp");
    std::ofstream(tmp) << value;
    retry_briefly([&](std::error_code &ec) { std::filesystem::rename(tmp, path, ec); });
}

void set_chain(const Rig &rig, const std::string &value) { set_file(rig, "chain", value); }

std::string calls_made(const Rig &rig) {
    std::ifstream in(rig.calls());
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

anolis::runtime::RuntimeConfig config_for(const Rig &rig, const std::string &scope = "[bread0/motor0]",
                                          const std::string &extra_args = "", const std::string &on_startup = "leave") {
    const auto path = rig.root / "config" / "runtime.yaml";
    std::ofstream(path) << "runtime:\n  data_dir: \"" << (rig.root / "data").generic_string()
                        << "\"\nhttp:\n  enabled: false\nproviders:\n  - id: bread0\n    command: \"" << kHelperPath
                        << "\"\n    args: [\"--stop-chain\", \"" << rig.root.generic_string() << "\"" << extra_args
                        << "]\n    timeout_ms: 2000\n    hello_timeout_ms: 2000\n    ready_timeout_ms: 5000\n"
                           "polling:\n  interval_ms: 100\ntelemetry:\n  enabled: false\n"
                           "safety:\n  on_startup: "
                        << on_startup
                        << "\n  safe_state:\n    zero_is_safe: true\n"  // a ladder would call motor0
                           "  stop_chain:\n    status:\n      signal: bread0/gpio0/chain_open\n"
                           "    devices: "
                        << scope << "\n";
    anolis::runtime::RuntimeConfig config;
    std::string error;
    EXPECT_TRUE(anolis::runtime::load_config(path.string(), config, error)) << error;
    return config;
}

bool initialize_capturing_log(anolis::runtime::Runtime &runtime, std::string &error, std::string &log) {
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    const bool ok = runtime.initialize(error);
    std::cerr.rdbuf(old);
    log = captured.str();
    return ok;
}

bool eventually(const std::function<bool()> &condition, std::chrono::milliseconds within = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// Runs the runtime's main loop for the test body, then stops it.
class MainLoop {
public:
    explicit MainLoop(anolis::runtime::Runtime &runtime) : runtime_(runtime) {
        anolis::runtime::SignalHandler::install();
        thread_ = std::thread([this] { runtime_.run(); });
    }
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

}  // namespace

TEST(RuntimeStopChainTest, StartsHeldWhenTheStopIsInForceAndSendsNoCalls) {
    const auto rig = fresh_rig("anolis-483-start-in-force");
    set_chain(rig, "1");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::InForce)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
    const auto refusal = runtime.get_safe_state().clear();
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("the emergency stop is in force"), std::string::npos) << *refusal;
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
    EXPECT_EQ(calls_made(rig), "");
}

TEST(RuntimeStopChainTest, StartsHeldWhenTheStatusCannotBeRead) {
    const auto rig = fresh_rig("anolis-483-start-unknown");
    set_chain(rig, "");  // the helper reports FAULT quality
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Unknown)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
    const auto refusal = runtime.get_safe_state().clear();
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("status is unavailable"), std::string::npos) << *refusal;
}

TEST(RuntimeStopChainTest, StartsUnheldWhenTheStopIsClear) {
    const auto rig = fresh_rig("anolis-483-start-clear");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Clear)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_FALSE(runtime.get_safe_state().is_engaged());
    EXPECT_FALSE(runtime.get_safe_state().clear().has_value());
}

TEST(RuntimeStopChainTest, AStopHoldsAndReleasingItPermitsButDoesNotPerformTheClear) {
    const auto rig = fresh_rig("anolis-483-transitions");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    auto events = runtime.get_event_emitter().subscribe();
    ASSERT_FALSE(runtime.get_safe_state().is_engaged());

    {
        MainLoop loop(runtime);
        set_chain(rig, "1");
        ASSERT_TRUE(eventually([&] { return runtime.get_safe_state().is_engaged(); }));
        EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::InForce)
            << runtime.get_stop_chain().snapshot().reason;
        EXPECT_TRUE(runtime.get_safe_state().clear().has_value());

        set_chain(rig, "0");
        ASSERT_TRUE(eventually([&] { return runtime.get_stop_chain().snapshot().state == StopChainState::Clear; }));
        EXPECT_TRUE(runtime.get_safe_state().is_engaged()) << "releasing the stop must not clear the latch";
        EXPECT_FALSE(runtime.get_safe_state().clear().has_value());
        EXPECT_FALSE(runtime.get_safe_state().is_engaged());
    }
    EXPECT_EQ(calls_made(rig), "");

    std::vector<std::string> moves;
    while (auto event = events->try_pop()) {
        if (const auto *change = std::get_if<anolis::events::StopChainEvent>(&*event)) {
            moves.push_back(change->previous_state + "->" + change->new_state);
        }
    }
    EXPECT_EQ(moves, (std::vector<std::string>{"clear->in_force", "in_force->clear"}));
}

TEST(RuntimeStopChainTest, AStatusThatGoesStaleHolds) {
    const auto rig = fresh_rig("anolis-483-stale");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    MainLoop loop(runtime);
    set_chain(rig, "stale");
    ASSERT_TRUE(eventually([&] { return runtime.get_safe_state().is_engaged(); }));
    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Unknown)
        << runtime.get_stop_chain().snapshot().reason;
}

TEST(RuntimeStopChainTest, AStatusThatCannotBeReadHolds) {
    const auto rig = fresh_rig("anolis-483-unreadable");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    MainLoop loop(runtime);
    set_chain(rig, "");  // removed for good: the helper reports FAULT quality
    ASSERT_TRUE(eventually([&] { return runtime.get_safe_state().is_engaged(); }));
    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Unknown)
        << runtime.get_stop_chain().snapshot().reason;
}

TEST(RuntimeStopChainTest, WarnsWhenTheStatusSourceIsInItsOwnScopeOrCanBeDriven) {
    const auto rig = fresh_rig("anolis-483-warnings");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig, "[bread0/gpio0, bread0/motor0]", ", \"--stop-chain-actuating\""));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("is a device in its own scope"), std::string::npos) << log;
    EXPECT_NE(log.find("also has actuating functions (set_pwm)"), std::string::npos) << log;
}

TEST(RuntimeStopChainTest, WarnsAndHoldsWhenTheStatusSignalIsNeverPolled) {
    // Only signals with a poll hint are polled: one without stays unread.
    const auto rig = fresh_rig("anolis-483-no-hint");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig, "[bread0/motor0]", ", \"--stop-chain-no-hint\""));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("has no poll hint, so it is never polled"), std::string::npos) << log;
    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Unknown)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
}

TEST(RuntimeStopChainTest, NoWarningForASeparateReadOnlyStatusSource) {
    const auto rig = fresh_rig("anolis-483-no-warnings");
    set_chain(rig, "0");
    anolis::runtime::Runtime runtime(config_for(rig));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(log.find("stop_chain:"), std::string::npos) << log;
}

// safety.on_startup at boot (#483). gpio0 also has an actuating set_pwm and is
// outside the scope; motor0 is in it. The zero rung drives both.

TEST(RuntimeStopChainTest, AStartupSafeStateSkipsOnlyTheDevicesBehindAStopInForce) {
    const auto rig = fresh_rig("anolis-483-startup-in-force");
    set_chain(rig, "1");
    anolis::runtime::Runtime runtime(config_for(rig, "[bread0/motor0]", ", \"--stop-chain-actuating\"", "safe_state"));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    const auto startup = runtime.get_safe_state().startup_report();
    ASSERT_EQ(startup.skipped_actions.size(), 1u);
    EXPECT_EQ(startup.skipped_actions[0].device_handle, "bread0/motor0");
    EXPECT_EQ(startup.skipped_actions[0].reason, "stop chain in force");
    EXPECT_EQ(calls_made(rig), "gpio0/1\n") << "the powered device outside the scope is still driven";
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
}

TEST(RuntimeStopChainTest, AStartupSafeStateRunsInFullWhenTheStatusIsUnknown) {
    // The status source may be down while the devices are powered.
    const auto rig = fresh_rig("anolis-483-startup-unknown");
    set_chain(rig, "");
    anolis::runtime::Runtime runtime(config_for(rig, "[bread0/motor0]", ", \"--stop-chain-actuating\"", "safe_state"));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::Unknown)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_TRUE(runtime.get_safe_state().startup_report().skipped_actions.empty());
    const auto calls = calls_made(rig);
    EXPECT_NE(calls.find("gpio0/1"), std::string::npos) << calls;
    EXPECT_NE(calls.find("motor0/1"), std::string::npos) << calls;
    EXPECT_TRUE(runtime.get_safe_state().is_engaged());
}

TEST(DeviceLossLatchStopChainTest, ALossUnderAStopInForceIsReportedAsTheStop) {
    anolis::control::DeviceLossLatch latch;
    latch.set_expected_loss([](const std::string &handle) -> std::optional<std::string> {
        if (handle == "bread0/motor0") {
            return std::string("the emergency stop in force (bread0/gpio0/chain_open)");
        }
        return std::nullopt;
    });

    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    latch.apply_reachability_edge("bread0/motor0", false, anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE, 1);
    latch.apply_reachability_edge("bread0/rlht0", false, anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE, 2);
    std::cerr.rdbuf(old);
    const auto log = captured.str();

    // Both latch either way: the stop is a hold, not a pass.
    EXPECT_TRUE(latch.is_latched("bread0/motor0"));
    EXPECT_TRUE(latch.is_latched("bread0/rlht0"));
    EXPECT_NE(log.find("bread0/motor0 went unreachable under the emergency stop in force (bread0/gpio0/chain_open) "
                       "(expected, not a device fault)"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("bread0/rlht0 went unreachable; autonomous actuation to it is blocked"), std::string::npos)
        << log;
}

// Per-device expectations (#484). gpio0 is the status; motor0, in scope,
// publishes `estop` from <rig>/estop and goes silent while <rig>/motor0_off
// exists. A device is judged once the stop has been in force for the stale
// bound, max(1000 ms, 3 x 100 ms), so a quiet check waits past it.

namespace {

using anolis::control::StopChainMismatch;
using anolis::runtime::StopChainExpect;

constexpr auto kPastTheGrace = std::chrono::milliseconds(2000);

const char *kPowerLoss = "[{handle: bread0/motor0, expect: power_loss}]";
const char *kReportsEstop = "[{handle: bread0/motor0, expect: reports_estop}]";
const char *kEstopArgs = ", \"--stop-chain-estop\"";

std::vector<std::string> mismatch_moves(anolis::events::Subscription &events) {
    std::vector<std::string> moves;
    while (auto event = events.try_pop()) {
        if (const auto *mismatch = std::get_if<anolis::events::StopChainMismatchEvent>(&*event)) {
            moves.push_back(std::string(mismatch->raised ? "raised " : "resolved ") + mismatch->device + " " +
                            mismatch->expect);
        }
    }
    return moves;
}

// Starts clear, presses the stop, and waits until the runtime reads it in force.
void press_the_stop(const Rig &rig, anolis::runtime::Runtime &runtime) {
    set_chain(rig, "1");
    ASSERT_TRUE(eventually([&] { return runtime.get_stop_chain().snapshot().state == StopChainState::InForce; }));
}

}  // namespace

TEST(RuntimeStopChainExpectTest, APowerLossDeviceThatStillAnswersRaisesStopNotReaching) {
    const auto rig = fresh_rig("anolis-484-power-loss-answers");
    set_chain(rig, "0");
    set_file(rig, "estop", "0");
    anolis::runtime::Runtime runtime(config_for(rig, kPowerLoss, kEstopArgs));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    auto events = runtime.get_event_emitter().subscribe();

    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    {
        MainLoop loop(runtime);
        press_the_stop(rig, runtime);
        // Not judged before the grace: four polls' worth, well inside it.
        for (int sample = 0; sample < 20; ++sample) {
            EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty()) << "judged before the grace";
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        ASSERT_TRUE(eventually([&] { return !runtime.get_stop_chain().mismatches().empty(); }));
        const auto standing = runtime.get_stop_chain().mismatches();
        ASSERT_EQ(standing.size(), 1u);
        EXPECT_EQ(standing[0].device, "bread0/motor0");
        EXPECT_EQ(standing[0].expect, StopChainExpect::PowerLoss);
        EXPECT_NE(standing[0].reason.find("still answers"), std::string::npos) << standing[0].reason;

        set_chain(rig, "0");
        ASSERT_TRUE(eventually([&] { return runtime.get_stop_chain().mismatches().empty(); }));
    }
    std::cerr.rdbuf(old);

    EXPECT_NE(captured.str().find("Stop not reaching bread0/motor0"), std::string::npos) << captured.str();
    EXPECT_EQ(mismatch_moves(*events),
              (std::vector<std::string>{"raised bread0/motor0 power_loss", "resolved bread0/motor0 power_loss"}));
    EXPECT_EQ(calls_made(rig), "");
}

TEST(RuntimeStopChainExpectTest, APowerLossDeviceThatGoesSilentRaisesNothing) {
    // Silent: its reads fail, or its provider still reports it with stale quality.
    for (const std::string off : {"1", "stale"}) {
        SCOPED_TRACE("motor0_off " + off);
        const auto rig = fresh_rig("anolis-484-power-loss-silent-" + off);
        set_chain(rig, "0");
        set_file(rig, "estop", "0");
        anolis::runtime::Runtime runtime(config_for(rig, kPowerLoss, kEstopArgs));
        std::string error;
        std::string log;
        ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
        auto events = runtime.get_event_emitter().subscribe();

        MainLoop loop(runtime);
        set_file(rig, "motor0_off", off);
        press_the_stop(rig, runtime);
        std::this_thread::sleep_for(kPastTheGrace);
        EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty());
        EXPECT_TRUE(mismatch_moves(*events).empty());
    }
}

// #605: on Windows a check of motor0_off can fail while the test renames it
// into place, and the helper died of the exception. A link to itself fails
// the same check on POSIX (ELOOP): motor0 must read as silent, and the helper
// must keep answering, so the stop is still read in force.
TEST(RuntimeStopChainExpectTest, AMotorOffMarkerTheHelperCannotCheckStillReadsSilent) {
#ifdef _WIN32
    GTEST_SKIP() << "creating a symlink needs extra privileges on Windows";
#else
    const auto rig = fresh_rig("anolis-605-motor0-off-unreadable");
    set_chain(rig, "0");
    set_file(rig, "estop", "0");
    anolis::runtime::Runtime runtime(config_for(rig, kPowerLoss, kEstopArgs));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    auto events = runtime.get_event_emitter().subscribe();

    MainLoop loop(runtime);
    std::filesystem::create_symlink("motor0_off", rig.root / "motor0_off");
    press_the_stop(rig, runtime);
    std::this_thread::sleep_for(kPastTheGrace);
    // Still read fresh: a helper that had died would leave the status stale.
    EXPECT_EQ(runtime.get_stop_chain().snapshot().state, StopChainState::InForce)
        << runtime.get_stop_chain().snapshot().reason;
    EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty());
    EXPECT_TRUE(mismatch_moves(*events).empty());
#endif
}

TEST(RuntimeStopChainExpectTest, AReportsEstopDeviceWhoseEstopReadsFalseRaisesStopNotReaching) {
    const auto rig = fresh_rig("anolis-484-reports-estop-false");
    set_chain(rig, "0");
    set_file(rig, "estop", "0");
    anolis::runtime::Runtime runtime(config_for(rig, kReportsEstop, kEstopArgs));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    auto events = runtime.get_event_emitter().subscribe();

    MainLoop loop(runtime);
    press_the_stop(rig, runtime);
    ASSERT_TRUE(eventually([&] { return !runtime.get_stop_chain().mismatches().empty(); }));
    const auto standing = runtime.get_stop_chain().mismatches();
    ASSERT_EQ(standing.size(), 1u);
    EXPECT_EQ(standing[0].device, "bread0/motor0");
    EXPECT_EQ(standing[0].expect, StopChainExpect::ReportsEstop);
    EXPECT_NE(standing[0].reason.find("estop reads false"), std::string::npos) << standing[0].reason;

    // The device starts reporting the stop: the mismatch resolves while in force.
    set_file(rig, "estop", "1");
    ASSERT_TRUE(eventually([&] { return runtime.get_stop_chain().mismatches().empty(); }));
    EXPECT_EQ(mismatch_moves(*events),
              (std::vector<std::string>{"raised bread0/motor0 reports_estop", "resolved bread0/motor0 reports_estop"}));
}

TEST(RuntimeStopChainExpectTest, AReportsEstopDeviceWhoseEstopReadsTrueRaisesNothing) {
    const auto rig = fresh_rig("anolis-484-reports-estop-true");
    set_chain(rig, "0");
    set_file(rig, "estop", "1");
    anolis::runtime::Runtime runtime(config_for(rig, kReportsEstop, kEstopArgs));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    MainLoop loop(runtime);
    press_the_stop(rig, runtime);
    std::this_thread::sleep_for(kPastTheGrace);
    EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty());
}

TEST(RuntimeStopChainExpectTest, AReportsEstopDeviceWithNoFreshEstopRaisesNothing) {
    // Only a fresh estop=false contradicts it: a FAULT reading, or none at all
    // from a device that lost power, is not the stop failing to reach it.
    for (const bool silent : {false, true}) {
        SCOPED_TRACE(silent ? "motor0 silent" : "estop FAULT quality");
        const auto rig = fresh_rig(silent ? "anolis-484-reports-estop-silent" : "anolis-484-reports-estop-fault");
        set_chain(rig, "0");
        set_file(rig, "estop", "0");
        anolis::runtime::Runtime runtime(config_for(rig, kReportsEstop, kEstopArgs));
        std::string error;
        std::string log;
        ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

        MainLoop loop(runtime);
        if (silent) {
            set_file(rig, "motor0_off", "1");
        } else {
            set_file(rig, "estop", "fault");
        }
        press_the_stop(rig, runtime);
        std::this_thread::sleep_for(kPastTheGrace);
        EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty());
    }
}

TEST(RuntimeStopChainExpectTest, ADeviceWithNoExpectationNeverRaises) {
    const auto rig = fresh_rig("anolis-484-no-expect");
    set_chain(rig, "0");
    set_file(rig, "estop", "0");  // it answers, and does not report the stop
    anolis::runtime::Runtime runtime(config_for(rig, "[bread0/motor0]", kEstopArgs));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    auto events = runtime.get_event_emitter().subscribe();

    MainLoop loop(runtime);
    press_the_stop(rig, runtime);
    std::this_thread::sleep_for(kPastTheGrace);
    EXPECT_TRUE(runtime.get_stop_chain().mismatches().empty());
    EXPECT_TRUE(mismatch_moves(*events).empty());
    EXPECT_TRUE(runtime.get_safe_state().is_engaged()) << "held all the same";
}

TEST(RuntimeStopChainExpectTest, WarnsWhenAnExpectationHasNothingToJudgeItBy) {
    // Without --stop-chain-estop, motor0 publishes no signals at all.
    const std::vector<std::tuple<std::string, std::string, std::string>> cases = {
        {kReportsEstop, "", "bread0/motor0 expects reports_estop but has no polled bool estop signal"},
        {kPowerLoss, "", "bread0/motor0 expects power_loss but has no polled signal"},
        {kReportsEstop, kEstopArgs, ""},
        {kPowerLoss, kEstopArgs, ""},
    };
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto &[scope, args, warning] = cases[i];
        SCOPED_TRACE(scope + args);
        const auto rig = fresh_rig("anolis-484-warn-" + std::to_string(i));
        set_chain(rig, "0");
        set_file(rig, "estop", "1");
        anolis::runtime::Runtime runtime(config_for(rig, scope, args));
        std::string error;
        std::string log;
        ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
        if (warning.empty()) {
            EXPECT_EQ(log.find("a mismatch is never raised"), std::string::npos) << log;
        } else {
            EXPECT_NE(log.find(warning), std::string::npos) << log;
        }
    }
}
