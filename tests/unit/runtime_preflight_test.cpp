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

namespace {

// heater0/set_pwm is ACTUATE, pwm in 0..100. One setpoint with pwm -5.
std::string heater_yaml(bool zero_is_safe) {
    return std::string(R"(
runtime: {}
http:
  enabled: false
providers:
  - id: bread0
    command: ")") +
           kHelperPath + R"("
    args: ["--heater"]
    timeout_ms: 2000
    hello_timeout_ms: 2000
    ready_timeout_ms: 5000
polling:
  interval_ms: 200
telemetry:
  enabled: false
safety:
  safe_state:
    zero_is_safe: )" +
           (zero_is_safe ? "true" : "false") + R"(
    setpoints:
      - device_handle: bread0/heater0
        function_name: set_pwm
        args:
          pwm: -5
)";
}

}  // namespace

// #640 (B2 of its review): a setpoint that cannot dispatch is an ERROR and is
// counted, whether or not a zero covers its output, and the preflight never
// says no safe state is declared when one is.
TEST(RuntimePreflightTest, AnUndispatchableSetpointIsAnErrorAndTheStartupRunFailsOnIt) {
    anolis::runtime::Runtime runtime(load("anolis-640-no-zero.yaml", heater_yaml(false)));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("[ERROR] [Runtime] safety.safe_state.setpoints cannot be dispatched: bread0/heater0/set_pwm - "
                       "Argument 'pwm': Value -5 below minimum 0"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("1 of 1 declared call(s) could not be dispatched"), std::string::npos) << log;
    EXPECT_NE(log.find("leaves 1 of 1 actuating function(s) undriven"), std::string::npos) << log;
    EXPECT_EQ(log.find("no software safe-state is declared"), std::string::npos) << log;
    EXPECT_EQ(log.find("declared but unused"), std::string::npos) << log;

    // What install.sh reads (#634): a config failure in the startup run.
    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_TRUE(startup.ran);
    EXPECT_FALSE(startup.safe_state_achieved);
    ASSERT_EQ(startup.failed_actions.size(), 1u);
    EXPECT_EQ(startup.failed_actions[0].device_handle, "bread0/heater0");
    EXPECT_EQ(startup.failed_actions[0].cause, anolis::control::FailureCause::Config);
}

// The issue's case: zero_is_safe covers the output, so the startup run
// drives it to 0 and is achieved. The broken setpoint is still an ERROR.
TEST(RuntimePreflightTest, ZeroCoversAnOutputWhoseSetpointCannotDispatch) {
    anolis::runtime::Runtime runtime(load("anolis-640-zero.yaml", heater_yaml(true)));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("[ERROR] [Runtime] safety.safe_state.setpoints cannot be dispatched"), std::string::npos) << log;
    EXPECT_NE(log.find("preflight: e-stop plan 'zero'"), std::string::npos) << log;
    EXPECT_EQ(log.find("undriven"), std::string::npos) << log;

    const auto startup = runtime.get_safe_state().startup_report();
    EXPECT_TRUE(startup.ran);
    EXPECT_EQ(startup.kind, anolis::control::SafeStateKind::Zero);
    EXPECT_TRUE(startup.safe_state_achieved);
    EXPECT_TRUE(startup.failed_actions.empty());
}

namespace {

// The helper always reports dcmt0 UNREACHABLE and leaves it out of its
// inventory: a device its provider says is missing. dcmt3 is listed and OK.
std::string absent_hooks_yaml(bool with_present_hook) {
    std::string yaml = std::string(R"(
runtime: {}
http:
  enabled: false
providers:
  - id: bread0
    command: ")") + kHelperPath +
                       R"("
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
      - device_handle: bread0/dcmt0
        function_name: set_mode
        args:
          mode: open_loop
)";
    if (with_present_hook) {
        yaml += R"(      - device_handle: bread0/dcmt3
        function_name: set_mode
        args:
          mode: closed_speed
)";
    }
    return yaml;
}

}  // namespace

// #465: with every declared call absent there is no rest to call dispatchable.
TEST(RuntimePreflightTest, EveryCallAbsentDoesNotClaimTheRestAreDispatchable) {
    anolis::runtime::Runtime runtime(load("anolis-465-all-absent.yaml", absent_hooks_yaml(false)));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("preflight: 1 of 1 declared call(s) target devices absent at startup"), std::string::npos)
        << log;
    EXPECT_EQ(log.find("the rest are dispatchable"), std::string::npos) << log;
}

TEST(RuntimePreflightTest, SomeCallsAbsentSaysTheRestAreDispatchable) {
    anolis::runtime::Runtime runtime(load("anolis-465-some-absent.yaml", absent_hooks_yaml(true)));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_NE(log.find("preflight: 1 of 2 declared call(s) target devices absent at startup; the rest are "
                       "dispatchable against the current inventory"),
              std::string::npos)
        << log;
}

// With nothing declared there is nothing absent to warn about.
TEST(RuntimePreflightTest, NoDeclaredCallsWarnsOfNoAbsentDevices) {
    std::string yaml = absent_hooks_yaml(false);
    yaml = yaml.substr(0, yaml.find("safety:"));
    anolis::runtime::Runtime runtime(load("anolis-465-none-declared.yaml", yaml));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;

    EXPECT_EQ(log.find("target devices absent at startup"), std::string::npos) << log;
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

namespace {

std::string heater_hook_yaml(const std::string &pwm) {
    return std::string(R"(
runtime: {}
http:
  enabled: false
providers:
  - id: bread0
    command: ")") +
           kHelperPath + R"("
    args: ["--heater"]
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
      - device_handle: bread0/heater0
        function_name: set_pwm
        args:
          pwm: )" +
           pwm + "\n";
}

}  // namespace

// Review of #886: a quoted hook argument is a string, so `pwm: "0"` for an
// int64 argument could never dispatch, and the machine started anyway with the
// safe state broken. Startup now refuses it, naming the hook and both types.
TEST(RuntimePreflightTest, AHookArgumentOfTheWrongTypeRefusesStartup) {
    anolis::runtime::Runtime runtime(load("anolis-886-quoted-pwm.yaml", heater_hook_yaml("\"0\"")));
    std::string error;
    std::string log;
    EXPECT_FALSE(initialize_capturing_log(runtime, error, log));
    EXPECT_NE(error.find("wrong type"), std::string::npos) << error;
    EXPECT_NE(error.find("safety.safe_state.hooks bread0/heater0/set_pwm"), std::string::npos) << error;
    EXPECT_NE(error.find("Argument 'pwm': Type mismatch: expected int64, got string"), std::string::npos) << error;
}

// Review of #896: with hooks declared they are the e-stop's whole plan, so a
// safe_state setpoint is unused. A wrong type there is a warning, as any other
// fault in an unused setpoint is, not a reason to refuse startup.
TEST(RuntimePreflightTest, AWrongTypeInAnUnusedSetpointIsAWarningNotARefusal) {
    const std::string yaml = heater_hook_yaml("0") + R"(    setpoints:
      - device_handle: bread0/heater0
        function_name: set_pwm
        args:
          pwm: "0"
)";
    anolis::runtime::Runtime runtime(load("anolis-896-unused-setpoint.yaml", yaml));
    std::string error;
    std::string log;
    ASSERT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    EXPECT_NE(log.find("Type mismatch: expected int64, got string (unused"), std::string::npos) << log;
    // Review of #896: the mismatch was seen only when the type check was the first
    // thing to fail. An unknown or a missing argument in the same hook hid it, and
    // the machine started with an ERROR logged. `aaa` sorts before `pwm`.
    TEST(RuntimePreflightTest, AWrongTypeBehindAnotherArgumentFaultStillRefusesStartup) {
        anolis::runtime::Runtime runtime(
            load("anolis-896-quoted-pwm-and-unknown.yaml", heater_hook_yaml("\"0\"\n          aaa: 1")));
        std::string error;
        std::string log;
        EXPECT_FALSE(initialize_capturing_log(runtime, error, log));
        EXPECT_NE(error.find("Argument 'pwm': Type mismatch: expected int64, got string"), std::string::npos) << error;
    }

    TEST(RuntimePreflightTest, AHookArgumentOfTheRightTypeStarts) {
        anolis::runtime::Runtime runtime(load("anolis-886-plain-pwm.yaml", heater_hook_yaml("0")));
        std::string error;
        std::string log;
        EXPECT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
    }

    // A value of the right type that is out of range is still reported, not
    // refused: only a type mismatch stops startup.
    TEST(RuntimePreflightTest, AHookArgumentOutOfRangeIsReportedNotRefused) {
        anolis::runtime::Runtime runtime(load("anolis-886-range-pwm.yaml", heater_hook_yaml("500")));
        std::string error;
        std::string log;
        EXPECT_TRUE(initialize_capturing_log(runtime, error, log)) << error;
        EXPECT_NE(log.find("safety.safe_state.hooks cannot be dispatched: bread0/heater0/set_pwm"), std::string::npos)
            << log;
    }
