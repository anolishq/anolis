// anolishq/anolis#461: the runtime starts, and survives a supervised restart,
// with a provider that answers ready and lists no devices. The provider is a
// helper in the shape of bread 0.8.1 on an unpowered bus (2026-10-23): no
// devices, and rlht0, dcmt0 and dcmt1 reported UNREACHABLE.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "runtime/config.hpp"
#include "runtime/runtime.hpp"
#include "runtime/signal_handler.hpp"

namespace {

const char *kEmptyInventoryProviderPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

// Hooks name the three devices the helper reports missing; the call
// arguments never matter, since none of those calls is issued.
std::string config_yaml(const std::string &extra_args, const std::string &extra_hooks = "") {
    return std::string(R"(
runtime: {}
http:
  enabled: false
providers:
  - id: bread0
    command: ")") +
           kEmptyInventoryProviderPath + R"("
    args: [)" +
           extra_args +
           R"(]
    timeout_ms: 2000
    hello_timeout_ms: 2000
    ready_timeout_ms: 5000
    restart_policy:
      enabled: true
      max_attempts: 3
      backoff_ms: [50, 50, 50]
      timeout_ms: 5000
      success_reset_ms: 100
polling:
  interval_ms: 200
telemetry:
  enabled: false
safety:
  safe_state:
    hooks:
      - device_handle: bread0/rlht0
        function_name: set_power
      - device_handle: bread0/dcmt0
        function_name: set_open_loop
      - device_handle: bread0/dcmt1
        function_name: set_open_loop
)" + extra_hooks;
}

std::filesystem::path write_config(const std::string &name, const std::string &yaml) {
    const auto path = std::filesystem::path(::testing::TempDir()) / name;
    std::ofstream(path) << yaml;
    return path;
}

anolis::runtime::RuntimeConfig load(const std::filesystem::path &path) {
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

std::set<std::string> handles(const std::vector<anolis::control::SkippedAction> &actions) {
    std::set<std::string> out;
    for (const auto &action : actions) {
        out.insert(action.device_handle);
    }
    return out;
}

}  // namespace

TEST(RuntimeZeroDevicesTest, StartsWithAProviderThatListsNoDevicesAndSkipsItsAbsentDevices) {
    const auto config = load(write_config("anolis-461-start.yaml", config_yaml("")));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    // Devices that are absent, and nothing else, are warnings: a powered-off
    // bus is not an error at boot.
    EXPECT_EQ(log.find("[ERROR]"), std::string::npos) << log;

    const auto provider = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(provider, nullptr);
    EXPECT_TRUE(provider->is_available());
    EXPECT_TRUE(runtime.get_registry().get_devices_for_provider("bread0").empty());

    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_TRUE(startup.ran);
    EXPECT_FALSE(startup.safe_state_achieved);
    EXPECT_TRUE(startup.failed_actions.empty());
    ASSERT_EQ(startup.skipped_actions.size(), 3u);
    EXPECT_EQ(startup.skipped_actions[0].device_handle, "bread0/rlht0");
    EXPECT_EQ(startup.skipped_actions[0].reason, "probe failed (version_read_failed): rlht0 did not answer");
}

// Only a device its provider reports UNREACHABLE and the registry lacks is
// skipped: rlht1 is registered though UNREACHABLE, dcmt2 is unregistered but
// OK. Both calls are issued and fail, as an unpowered bus never makes them.
TEST(RuntimeZeroDevicesTest, SkipsOnlyDevicesThatAreBothUnreachableAndUnregistered) {
    const auto config =
        load(write_config("anolis-461-mixed.yaml", config_yaml("\"--mixed\"", R"(      - device_handle: bread0/rlht1
        function_name: set_power
      - device_handle: bread0/dcmt2
        function_name: set_open_loop
)")));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    ASSERT_EQ(runtime.get_registry().get_devices_for_provider("bread0").size(), 1u);

    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_FALSE(startup.safe_state_achieved);
    EXPECT_EQ(handles(startup.skipped_actions),
              (std::set<std::string>{"bread0/rlht0", "bread0/dcmt0", "bread0/dcmt1"}));
    std::set<std::string> failed;
    for (const auto &action : startup.failed_actions) {
        failed.insert(action.device_handle);
    }
    EXPECT_EQ(failed, (std::set<std::string>{"bread0/rlht1", "bread0/dcmt2"}));
}

TEST(RuntimeZeroDevicesTest, ARestartThatListsNoDevicesIsASuccessfulRestart) {
    const auto trigger = std::filesystem::path(::testing::TempDir()) / "anolis-461-exit-now";
    std::filesystem::remove(trigger);
    const auto config = load(
        write_config("anolis-461-restart.yaml", config_yaml("\"--exit-when\", \"" + trigger.generic_string() + "\"")));
    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    const auto first = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(first, nullptr);

    // run() ends only on the signal latch; install() clears it before and after.
    anolis::runtime::SignalHandler::install();
    std::thread loop([&runtime] { runtime.run(); });
    std::ofstream(trigger) << "exit";  // the helper removes it and exits

    // The supervisor restarts it (50 ms backoff); a restart that comes back
    // with no devices used to fail, leaving the dead handle in place.
    std::shared_ptr<anolis::provider::IProviderHandle> current;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        current = runtime.get_provider_registry().get_provider("bread0");
        if (current != nullptr && current != first && current->is_available()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::raise(SIGTERM);
    loop.join();
    runtime.shutdown();
    anolis::runtime::SignalHandler::install();

    ASSERT_NE(current, nullptr);
    EXPECT_NE(current, first) << "the provider was not replaced by a restart";
    EXPECT_TRUE(current->is_available());
    EXPECT_TRUE(runtime.get_registry().get_devices_for_provider("bread0").empty());
    EXPECT_FALSE(std::filesystem::exists(trigger)) << "the helper never saw the trigger";
}
