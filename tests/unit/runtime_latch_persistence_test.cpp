// anolishq/anolis#482: a runtime restarted after a software stop comes back
// latched, through the real Runtime and its config. The provider is the
// empty-inventory helper, so no device call is ever made.

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

std::filesystem::path fresh_dir(const std::string &name) {
    const auto dir = std::filesystem::path(::testing::TempDir()) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

anolis::runtime::RuntimeConfig config_for(const std::filesystem::path &data_dir, const std::string &name) {
    const auto path = data_dir / (name + ".yaml");
    std::ofstream(path) << "runtime:\n  name: " << name << "\n  data_dir: \"" << data_dir.generic_string()
                        << "\"\nhttp:\n  enabled: false\nproviders:\n  - id: bread0\n    command: \"" << kHelperPath
                        << "\"\n    timeout_ms: 2000\n    hello_timeout_ms: 2000\n    ready_timeout_ms: 5000\n"
                           "polling:\n  interval_ms: 200\ntelemetry:\n  enabled: false\n";
    anolis::runtime::RuntimeConfig config;
    std::string error;
    EXPECT_TRUE(anolis::runtime::load_config(path.string(), config, error)) << error;
    return config;
}

bool initialize_quietly(anolis::runtime::Runtime &runtime, std::string &error) {
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    const bool ok = runtime.initialize(error);
    std::cerr.rdbuf(old);
    return ok;
}

}  // namespace

TEST(RuntimeLatchPersistenceTest, ARestartAfterAStopComesBackLatched) {
    const auto dir = fresh_dir("anolis-482-restart");
    int64_t latched_at = 0;
    {
        anolis::runtime::Runtime runtime(config_for(dir, "rig"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
        ASSERT_FALSE(runtime.get_safe_state().is_engaged());
        runtime.get_safe_state().trigger("operator");
        latched_at = runtime.get_safe_state().capability().latched_at_epoch_ms.value_or(0);
        ASSERT_GT(latched_at, 0);
    }
    EXPECT_TRUE(std::filesystem::exists(dir / "estop" / "rig.json"));

    anolis::runtime::Runtime restarted(config_for(dir, "rig"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(restarted, error)) << error;
    const auto cap = restarted.get_safe_state().capability();
    EXPECT_TRUE(cap.latched);
    EXPECT_EQ(cap.latched_at_epoch_ms, latched_at);
    ASSERT_TRUE(cap.restored_reason.has_value());
    EXPECT_NE(cap.restored_reason->find("records a software stop"), std::string::npos) << *cap.restored_reason;
}

TEST(RuntimeLatchPersistenceTest, ARestartAfterAClearComesBackUnlatched) {
    const auto dir = fresh_dir("anolis-482-cleared");
    {
        anolis::runtime::Runtime runtime(config_for(dir, "rig"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
        runtime.get_safe_state().trigger("operator");
        runtime.get_safe_state().clear();
    }
    EXPECT_FALSE(std::filesystem::exists(dir / "estop" / "rig.json"));

    anolis::runtime::Runtime restarted(config_for(dir, "rig"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(restarted, error)) << error;
    EXPECT_FALSE(restarted.get_safe_state().is_engaged());
    EXPECT_FALSE(restarted.get_safe_state().capability().restored_reason.has_value());
}

TEST(RuntimeLatchPersistenceTest, ACorruptLatchFileStartsLatchedAndSaysWhy) {
    const auto dir = fresh_dir("anolis-482-corrupt");
    std::filesystem::create_directories(dir / "estop");
    std::ofstream(dir / "estop" / "rig.json") << "{ truncated";

    anolis::runtime::Runtime runtime(config_for(dir, "rig"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
    const auto cap = runtime.get_safe_state().capability();
    EXPECT_TRUE(cap.latched);
    EXPECT_TRUE(cap.latched_at_epoch_ms.has_value());
    ASSERT_TRUE(cap.restored_reason.has_value());
    EXPECT_NE(cap.restored_reason->find("not a valid latch record"), std::string::npos) << *cap.restored_reason;
}

TEST(RuntimeLatchPersistenceTest, TwoConfigsSharingADataDirKeepTheirOwnLatch) {
    // The workbench starts every project with the same working directory.
    const auto dir = fresh_dir("anolis-482-shared");
    {
        anolis::runtime::Runtime a(config_for(dir, "project-a"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(a, error)) << error;
        a.get_safe_state().trigger("operator");
    }

    anolis::runtime::Runtime b(config_for(dir, "project-b"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(b, error)) << error;
    EXPECT_FALSE(b.get_safe_state().is_engaged());

    anolis::runtime::Runtime a_again(config_for(dir, "project-a"));
    ASSERT_TRUE(initialize_quietly(a_again, error)) << error;
    EXPECT_TRUE(a_again.get_safe_state().is_engaged());
}
