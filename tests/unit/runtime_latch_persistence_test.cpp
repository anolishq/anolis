// anolishq/anolis#482: a runtime restarted after a software stop comes back
// latched, through the real Runtime and its config file. The latch belongs to
// the machine: it is keyed by the config file's directory, which a project's
// variants share, not by runtime.name, which the workbench's template gives
// every project. The provider is the empty-inventory helper, so no device call
// is ever made.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "control/latch_store.hpp"
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

// A config file `file` in `config_dir`, whose runtime keeps its data in `data_dir`.
anolis::runtime::RuntimeConfig config_at(const std::filesystem::path &config_dir, const std::string &file,
                                         const std::filesystem::path &data_dir, const std::string &name) {
    std::filesystem::create_directories(config_dir);
    const auto path = config_dir / file;
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

bool latch_dir_is_empty(const std::filesystem::path &data_dir) {
    const auto estop = data_dir / "estop";
    return !std::filesystem::exists(estop) || std::filesystem::is_empty(estop);
}

}  // namespace

TEST(RuntimeLatchPersistenceTest, ARestartAfterAStopComesBackLatched) {
    const auto root = fresh_dir("anolis-482-restart");
    int64_t latched_at = 0;
    {
        anolis::runtime::Runtime runtime(config_at(root / "config", "runtime.yaml", root / "data", "rig"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
        ASSERT_FALSE(runtime.get_safe_state().is_engaged());
        runtime.get_safe_state().trigger("operator");
        latched_at = runtime.get_safe_state().capability().latched_at_epoch_ms.value_or(0);
        ASSERT_GT(latched_at, 0);
    }
    EXPECT_FALSE(latch_dir_is_empty(root / "data"));

    anolis::runtime::Runtime restarted(config_at(root / "config", "runtime.yaml", root / "data", "rig"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(restarted, error)) << error;
    const auto cap = restarted.get_safe_state().capability();
    EXPECT_TRUE(cap.latched);
    EXPECT_EQ(cap.latched_at_epoch_ms, latched_at);
    ASSERT_TRUE(cap.restored_reason.has_value());
    EXPECT_NE(cap.restored_reason->find("records a software stop"), std::string::npos) << *cap.restored_reason;
}

TEST(RuntimeLatchPersistenceTest, ARestartAfterAClearComesBackUnlatched) {
    const auto root = fresh_dir("anolis-482-cleared");
    {
        anolis::runtime::Runtime runtime(config_at(root / "config", "runtime.yaml", root / "data", "rig"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
        runtime.get_safe_state().trigger("operator");
        runtime.get_safe_state().clear();
    }
    EXPECT_TRUE(latch_dir_is_empty(root / "data"));

    anolis::runtime::Runtime restarted(config_at(root / "config", "runtime.yaml", root / "data", "rig"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(restarted, error)) << error;
    EXPECT_FALSE(restarted.get_safe_state().is_engaged());
    EXPECT_FALSE(restarted.get_safe_state().capability().restored_reason.has_value());
}

TEST(RuntimeLatchPersistenceTest, ACorruptLatchFileStartsLatchedAndSaysWhy) {
    const auto root = fresh_dir("anolis-482-corrupt");
    const auto config = config_at(root / "config", "runtime.yaml", root / "data", "rig");
    const auto file = anolis::control::LatchStore::path_for(root / "data", config.config_dir);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "{ truncated";

    anolis::runtime::Runtime runtime(config);
    std::string error;
    ASSERT_TRUE(initialize_quietly(runtime, error)) << error;
    const auto cap = runtime.get_safe_state().capability();
    EXPECT_TRUE(cap.latched);
    EXPECT_TRUE(cap.latched_at_epoch_ms.has_value());
    ASSERT_TRUE(cap.restored_reason.has_value());
    EXPECT_NE(cap.restored_reason->find("not a valid latch record"), std::string::npos) << *cap.restored_reason;
}

TEST(RuntimeLatchPersistenceTest, AnotherVariantOfTheSameProjectComesBackLatched) {
    // bioreactor-v1's variants each name their runtime differently, in one
    // config/ directory: restarting the machine in another variant is the
    // same machine.
    const auto root = fresh_dir("anolis-482-variants");
    {
        anolis::runtime::Runtime manual(
            config_at(root / "config", "anolis-runtime.manual.yaml", root / "data", "bioreactor-manual"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(manual, error)) << error;
        manual.get_safe_state().trigger("operator");
    }

    anolis::runtime::Runtime full(
        config_at(root / "config", "anolis-runtime.full.yaml", root / "data", "bioreactor-full"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(full, error)) << error;
    EXPECT_TRUE(full.get_safe_state().is_engaged());
}

TEST(RuntimeLatchPersistenceTest, TwoProjectsWithTheSameRuntimeNameKeepTheirOwnLatch) {
    // The workbench's template names every runtime anolis-main, and starts
    // every project with the same working directory, so data_dir is shared.
    const auto root = fresh_dir("anolis-482-projects");
    {
        anolis::runtime::Runtime a(config_at(root / "a" / "config", "runtime.yaml", root / "data", "anolis-main"));
        std::string error;
        ASSERT_TRUE(initialize_quietly(a, error)) << error;
        a.get_safe_state().trigger("operator");
    }

    anolis::runtime::Runtime b(config_at(root / "b" / "config", "runtime.yaml", root / "data", "anolis-main"));
    std::string error;
    ASSERT_TRUE(initialize_quietly(b, error)) << error;
    EXPECT_FALSE(b.get_safe_state().is_engaged());

    anolis::runtime::Runtime a_again(config_at(root / "a" / "config", "runtime.yaml", root / "data", "anolis-main"));
    ASSERT_TRUE(initialize_quietly(a_again, error)) << error;
    EXPECT_TRUE(a_again.get_safe_state().is_engaged());
}
