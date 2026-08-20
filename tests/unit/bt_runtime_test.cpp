#include "automation/bt_runtime.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

std::filesystem::path write_tree_file(const std::string& file_stem, const std::string& xml) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / (file_stem + "-" + std::to_string(nonce) + ".xml");
    // Binary mode: load() reads the file as bytes, so avoid the platform newline
    // translation that would make the on-disk bytes differ from `xml` on Windows.
    std::ofstream out(path, std::ios::binary);
    out << xml;
    out.close();
    return path;
}

}  // namespace

TEST(BTRuntimeTest, TickFailsWithoutLoadedTree) {
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    anolis::automation::ModeManager mode_manager(anolis::automation::RuntimeMode::MANUAL);

    anolis::automation::BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    EXPECT_EQ(BT::NodeStatus::FAILURE, runtime.tick());
}

TEST(BTRuntimeTest, DirectTickUsesTypedServiceContext) {
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    anolis::automation::ModeManager mode_manager(anolis::automation::RuntimeMode::MANUAL);
    anolis::automation::ParameterManager parameter_manager;

    ASSERT_TRUE(parameter_manager.define("temp_setpoint", anolis::automation::ParameterType::DOUBLE, 25.0));

    anolis::automation::BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager,
                                          &parameter_manager);

    const std::string xml = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameter param="temp_setpoint" value="{target_temp}"/>
  </BehaviorTree>
</root>
)";

    const auto tree_path = write_tree_file("anolis-bt-runtime-test", xml);
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));

    EXPECT_EQ(BT::NodeStatus::SUCCESS, runtime.tick());
    EXPECT_EQ(BT::NodeStatus::SUCCESS, runtime.tick());

    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

namespace {

constexpr char kSimpleTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameter param="temp_setpoint" value="{target_temp}"/>
  </BehaviorTree>
</root>
)";

}  // namespace

// The neutral status()/definition() views are coherent and report a real digest.
TEST(BTRuntimeTest, NeutralStatusAndDefinitionView) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);

    EXPECT_EQ(runtime.engine_kind(), "behavior_tree");
    EXPECT_EQ(runtime.status().status, AutomationStatus::Idle);
    EXPECT_FALSE(runtime.definition().has_value());

    const auto tree_path = write_tree_file("anolis-bt-status-test", kSimpleTreeXml);
    const auto outcome = runtime.load(AutomationDefinitionRef{tree_path.string()});
    ASSERT_TRUE(outcome.ok) << outcome.error;
    EXPECT_EQ(outcome.version.engine_kind, "behavior_tree");
    EXPECT_FALSE(outcome.version.digest.empty());
    EXPECT_EQ(outcome.version.digest_scope, "top_level_file");

    const auto def = runtime.definition();
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->digest, outcome.version.digest);
    EXPECT_EQ(def->bytes, std::string(kSimpleTreeXml));  // exact loaded bytes, not a disk reread

    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// A failed load preserves the previously-active definition and version.
TEST(BTRuntimeTest, FailedLoadPreservesPreviousDefinition) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);

    const auto good = write_tree_file("anolis-bt-good", kSimpleTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{good.string()}).ok);
    const auto good_digest = runtime.definition()->digest;

    // Missing file: load fails, previous definition stays active.
    auto missing = runtime.load(AutomationDefinitionRef{"/nonexistent/path/to/tree.xml"});
    EXPECT_FALSE(missing.ok);
    EXPECT_FALSE(missing.error.empty());
    ASSERT_TRUE(runtime.definition().has_value());
    EXPECT_EQ(runtime.definition()->digest, good_digest);

    // Invalid XML: parse fails, previous definition still preserved.
    const auto bad = write_tree_file("anolis-bt-bad", "<not-a-valid-bt-tree>");
    EXPECT_FALSE(runtime.load(AutomationDefinitionRef{bad.string()}).ok);
    ASSERT_TRUE(runtime.definition().has_value());
    EXPECT_EQ(runtime.definition()->digest, good_digest);

    std::error_code ec;
    std::filesystem::remove(good, ec);
    std::filesystem::remove(bad, ec);
}

// Concurrent neutral reads while the lifecycle churns (stop -> load -> start).
// Exercises def_mutex_; run under TSAN to catch races on the active-definition
// pointers (the latent lock-free race this seam fixes).
TEST(BTRuntimeTest, ConcurrentStatusReadsDuringReloadAreRaceFree) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);

    const auto tree_path = write_tree_file("anolis-bt-reload", kSimpleTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);

    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!stop_readers.load()) {
                (void)runtime.status();
                (void)runtime.get_health();
                (void)runtime.definition();
                (void)runtime.get_tree_path();
            }
        });
    }

    for (int i = 0; i < 50; ++i) {
        runtime.stop();
        ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
        std::string err;
        EXPECT_TRUE(runtime.start(err)) << err;
    }

    stop_readers.store(true);
    for (auto& t : readers) {
        t.join();
    }
    runtime.stop();

    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// A tree that BUILDS but throws on its first tick. SetBlackboard writes a
// std::string into the key populate_blackboard() fills with the service context,
// so the type collision happens at tick, not at load. This matters: the nodes in
// this codebase guard their INPUTS, but every setOutput is unguarded and
// checkPreConditions has no try/catch at all, so a tick exception is reachable
// from ordinary tree edits -- not just from BT.CPP internals.
constexpr char kThrowingTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <SetBlackboard output_key="anolis.bt_service_context" value="not-a-service-context"/>
  </BehaviorTree>
</root>
)";

// #279: a tick exception must HALT autonomous actuation, not merely report it.
// Without the fix the loop simply ticks again and the runtime stays in AUTO with
// outputs latched at whatever the tree last commanded -- and the DCMT firmware
// command watchdog does not catch it, because it is fed by the state poller on a
// separate thread that this exception does not touch.
TEST(BTRuntimeTest, TickExceptionDrivesFaultAndStopsTicking) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-bt-throwing", kThrowingTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok)
        << "the throwing tree must LOAD cleanly, or it proves nothing -- load errors are already handled";

    // The tick loop only ticks in AUTO, so the exception path is only reachable there.
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    // Wait for the fault rather than sleeping a fixed period.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (mode_manager.current_mode() != RuntimeMode::FAULT && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(RuntimeMode::FAULT, mode_manager.current_mode())
        << "a throwing tick left the runtime in " << mode_to_string(mode_manager.current_mode())
        << "; autonomous actuation was not halted";

    // Confirm the tick really threw, so a pass cannot come from the loop never
    // having run.
    EXPECT_GT(runtime.get_health().error_count, 0);

    // The property #279 is actually about: the loop must stop ticking, not just
    // change mode. Sample across several tick periods (10 Hz default).
    const auto ticks_at_fault = runtime.get_health().total_ticks;
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(ticks_at_fault, runtime.get_health().total_ticks)
        << "the tick loop kept running after FAULT; the exception would repeat indefinitely";

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// Pins the C++ AutomationStatus -> wire-string mapping. The wire side
// (execution_status enum in runtime-http.openapi.v0.yaml) is validated against a
// live response by the runtime-http conformance check; this guards the C++ half
// so the two cannot drift silently.
TEST(AutomationStatusEnumTest, ToStringIsTotalAndMatchesWireEnum) {
    using anolis::automation::AutomationStatus;
    using anolis::automation::to_string;
    EXPECT_STREQ(to_string(AutomationStatus::Idle), "idle");
    EXPECT_STREQ(to_string(AutomationStatus::Running), "running");
    EXPECT_STREQ(to_string(AutomationStatus::Blocked), "blocked");
    EXPECT_STREQ(to_string(AutomationStatus::Failed), "failed");
    EXPECT_STREQ(to_string(AutomationStatus::Completed), "completed");
    EXPECT_STREQ(to_string(AutomationStatus::Unknown), "unknown");

    // The exact wire enum set (must equal execution_status in the OpenAPI contract).
    const std::set<std::string> wire_enum{"idle", "running", "blocked", "failed", "completed", "unknown"};
    std::set<std::string> mapped;
    for (auto s : {AutomationStatus::Idle, AutomationStatus::Running, AutomationStatus::Blocked,
                   AutomationStatus::Failed, AutomationStatus::Completed, AutomationStatus::Unknown}) {
        mapped.insert(to_string(s));
    }
    EXPECT_EQ(mapped, wire_enum);
}
