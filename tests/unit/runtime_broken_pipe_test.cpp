// anolishq/anolis#549: a write to a provider whose stdin is closed raises
// SIGPIPE, and under the default disposition that ended the whole runtime with
// exit 141, logging nothing. A real Runtime, with HTTP off, against the
// empty-inventory helper, which closes its stdin on cue. Each case starts from
// the default disposition, as a runtime started from a shell does. POSIX only:
// Windows has no SIGPIPE.

#ifndef _WIN32

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "protocol.pb.h"
#include "runtime/config.hpp"
#include "runtime/runtime.hpp"

namespace {

const char *kHelperPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

std::filesystem::path fresh_dir(const std::string &name) {
    const auto root = std::filesystem::path(::testing::TempDir()) / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

// One provider, bread0, listing gpio0 (a polled bool) and motor0, with
// `extra_args` after --stop-chain.
anolis::runtime::RuntimeConfig config_for(const std::filesystem::path &root, const std::string &extra_args) {
    const auto path = root / "runtime.yaml";
    std::ofstream(path) << "runtime:\n  data_dir: \"" << (root / "data").generic_string()
                        << "\"\nhttp:\n  enabled: false\nproviders:\n  - id: bread0\n    command: \"" << kHelperPath
                        << "\"\n    args: [\"--stop-chain\", \"" << root.generic_string() << "\"" << extra_args
                        << "]\n    timeout_ms: 2000\n    hello_timeout_ms: 2000\n    ready_timeout_ms: 5000\n"
                           "polling:\n  interval_ms: 100\ntelemetry:\n  enabled: false\n"
                           "safety:\n  safe_state:\n    zero_is_safe: true\n";
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

TEST(RuntimeBrokenPipeTest, AProviderThatClosesStdinMidRunFailsItsNextCallAndTheRuntimeRunsOn) {
    std::signal(SIGPIPE, SIG_DFL);
    const auto root = fresh_dir("anolis-549-mid-run");
    const auto close_now = root / "close-stdin";
    anolis::runtime::Runtime runtime(
        config_for(root, ", \"--close-stdin-when\", \"" + close_now.generic_string() + "\""));
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;
    auto provider = runtime.get_provider_registry().get_provider("bread0");
    ASSERT_NE(provider, nullptr);

    std::ofstream(close_now) << "1";
    anolis::deviceprovider::v1::ReadSignalsResponse response;
    anolis::deviceprovider::v1::Status_Code status{};
    // Answered; the helper closed its stdin before answering.
    ASSERT_TRUE(provider->read_signals("gpio0", {"chain_open"}, response, status)) << provider->last_error();

    // Under the default disposition, this write ends the test process.
    EXPECT_FALSE(provider->read_signals("gpio0", {"chain_open"}, response, status));
    EXPECT_THAT(provider->last_error(), ::testing::HasSubstr("Broken pipe (provider terminated)"));
    EXPECT_FALSE(provider->is_available());

    runtime.shutdown();
}

// The HTTP server, whose construction happened to leave SIGPIPE ignored, is
// built only after every provider has started, so this case is the same with
// http.enabled set.
TEST(RuntimeBrokenPipeTest, AProviderThatClosesStdinDuringItsHandshakeFailsStartupAndTheProcessLivesOn) {
    std::signal(SIGPIPE, SIG_DFL);
    const auto root = fresh_dir("anolis-549-handshake");
    anolis::runtime::Runtime runtime(config_for(root, ", \"--close-stdin-after-hello\""));

    std::string error;
    std::string log;
    // WaitReady is the first write after the helper closed its stdin.
    EXPECT_FALSE(initialize_capturing_log(runtime, error, log));
    EXPECT_THAT(log, ::testing::HasSubstr("Broken pipe (provider terminated)"));

    runtime.shutdown();
}

TEST(RuntimeBrokenPipeTest, AProviderStartsWithTheDefaultSigpipeDisposition) {
    std::signal(SIGPIPE, SIG_DFL);
    const auto root = fresh_dir("anolis-549-child-disposition");
    const auto report = root / "sigpipe";
    anolis::runtime::Runtime runtime(config_for(root, ", \"--sigpipe-to\", \"" + report.generic_string() + "\""));
    std::string error;
    ASSERT_TRUE(runtime.initialize(error)) << error;

    std::string disposition;
    std::ifstream(report) >> disposition;
    EXPECT_EQ(disposition, "default") << "the provider inherited the runtime's SIGPIPE disposition";

    runtime.shutdown();
}

#endif  // !_WIN32
