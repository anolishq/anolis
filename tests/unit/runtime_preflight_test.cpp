// anolishq/anolis#467: a declared call whose string value is outside the
// provider's allowed_values is refused like a call from HTTP. Startup preflight
// reports it and the startup safe state records it as failed. set_mode is
// CONFIG, as on bread, so it is outside the e-stop's uncovered count, which
// counts ACTUATE functions only. The provider is the empty-inventory helper
// with --allowed-values, which lists dcmt3 with set_mode(mode in {open_loop,
// closed_speed}).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "runtime/config.hpp"
#include "runtime/runtime.hpp"

namespace {

const char *kHelperPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

std::string config_yaml(const std::string &mode) {
    return std::string(R"(
runtime: {}
http:
  enabled: false
providers:
  - id: bread0
    command: ")") +
           kHelperPath + R"("
    args: ["--allowed-values"]
    timeout_ms: 2000
    hello_timeout_ms: 2000
    ready_timeout_ms: 5000
polling:
  interval_ms: 200
telemetry:
  enabled: false
safety:
  safe_state:
    hooks:
      - device_handle: bread0/dcmt3
        function_name: set_mode
        args:
          mode: )" +
           mode + "\n";
}

anolis::runtime::RuntimeConfig load(const std::string &name, const std::string &yaml) {
    const auto path = std::filesystem::path(::testing::TempDir()) / name;
    std::ofstream(path) << yaml;
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

}  // namespace

TEST(RuntimePreflightTest, FlagsAHookValueOutsideTheProvidersAllowedValues) {
    anolis::runtime::Runtime runtime(load("anolis-467-outside.yaml", config_yaml("closed_position")));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    const std::string refusal =
        "Argument 'mode': Value 'closed_position' not in allowed values: open_loop, closed_speed";
    EXPECT_NE(log.find("safety.safe_state.hooks cannot be dispatched: bread0/dcmt3/set_mode - " + refusal),
              std::string::npos)
        << log;

    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_TRUE(startup.ran);
    EXPECT_FALSE(startup.safe_state_achieved);
    ASSERT_EQ(startup.failed_actions.size(), 1u);
    EXPECT_EQ(startup.failed_actions[0].device_handle, "bread0/dcmt3");
    EXPECT_NE(startup.failed_actions[0].error.find(refusal), std::string::npos) << startup.failed_actions[0].error;
}

TEST(RuntimePreflightTest, PassesAHookValueInsideTheProvidersAllowedValues) {
    anolis::runtime::Runtime runtime(load("anolis-467-inside.yaml", config_yaml("closed_speed")));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(log.find("cannot be dispatched"), std::string::npos) << log;
    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_TRUE(startup.ran);
    EXPECT_TRUE(startup.safe_state_achieved);
    EXPECT_TRUE(startup.failed_actions.empty());
}
