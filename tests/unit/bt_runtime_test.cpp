#include "automation/bt_runtime.hpp"

#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/xml_parsing.h>
#include <gtest/gtest.h>
#include <tinyxml2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "automation/bt_nodes.hpp"
#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "events/event_emitter.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "runs/run_journal.hpp"
#include "state/state_cache.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

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

// The route #807 reproduced: BTRuntime loads a tree whose GetParameter sits
// in a SubTree without _autoremap, and its tick reads the parameter.
TEST(BTRuntimeTest, AnAnolisNodeInASubTreeGetsItsServices) {
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
<root BTCPP_format="4" main_tree_to_execute="MainTree">
  <BehaviorTree ID="MainTree"><SubTree ID="Inner"/></BehaviorTree>
  <BehaviorTree ID="Inner"><GetParameter param="temp_setpoint" value="{target_temp}"/></BehaviorTree>
</root>
)";
    const auto tree_path = write_tree_file("anolis-bt-subtree-services", xml);
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
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

// A tree that remaps a port onto the service-context key builds, but the
// runtime's blackboard write then throws. load() must report that as a failed
// load and keep the previous definition, not throw after committing (#281).
TEST(BTRuntimeTest, ServiceContextKeyCollisionFailsLoadAndPreservesPrevious) {
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

    const std::string poisoned_xml = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameter param="x" value="{anolis.bt_service_context}"/>
  </BehaviorTree>
</root>
)";
    const auto poisoned = write_tree_file("anolis-bt-poisoned", poisoned_xml);

    LoadOutcome outcome;
    EXPECT_NO_THROW(outcome = runtime.load(AutomationDefinitionRef{poisoned.string()}));
    EXPECT_FALSE(outcome.ok);
    EXPECT_FALSE(outcome.error.empty());
    ASSERT_TRUE(runtime.definition().has_value());
    EXPECT_EQ(runtime.definition()->digest, good_digest);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, runtime.tick());

    std::error_code ec;
    std::filesystem::remove(good, ec);
    std::filesystem::remove(poisoned, ec);
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

    // A failure outranks the mode gate (#270): FAULT is not AUTO, but the
    // engine's own failure is the cause and must stay visible.
    EXPECT_EQ(AutomationStatus::Failed, runtime.status().status);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

namespace {

// Waits until `done` holds or five seconds pass, rather than sleeping a fixed period.
template <typename Pred>
bool wait_for(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

// Throws on tick while the "explode" parameter is true. Otherwise it returns an
// ordinary FAILURE while "pass" is false and SUCCESS once it is true, so one
// loaded tree can fail a run and then run normally in the next. The control
// nodes are reactive so every tick re-reads both parameters.
constexpr char kConditionalThrowTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <ReactiveSequence>
      <GetParameterBool param="explode" value="{explode}"/>
      <GetParameterBool param="pass" value="{pass}"/>
      <ReactiveFallback>
        <CheckBool value="{explode}" expected="false"/>
        <SetBlackboard output_key="anolis.bt_service_context" value="not-a-service-context"/>
      </ReactiveFallback>
      <CheckBool value="{pass}"/>
    </ReactiveSequence>
  </BehaviorTree>
</root>
)";

}  // namespace

// #378: a failure belongs to the run it happened in. Loading a good tree after
// recovering from FAULT starts a new run, so the status must not stay failed.
TEST(BTRuntimeTest, LoadAfterFailureClearsFailedStatus) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto bad_path = write_tree_file("anolis-bt-recover-bad", kThrowingTreeXml);
    const auto good_path = write_tree_file("anolis-bt-recover-good", kSimpleTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{bad_path.string()}).ok);

    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));
    ASSERT_EQ(AutomationStatus::Failed, runtime.status().status);

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{good_path.string()}).ok);
    EXPECT_EQ(AutomationStatus::Idle, runtime.status().status) << "MANUAL, after loading a good tree";

    const auto ticks_before = runtime.get_health().total_ticks;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(wait_for([&] { return runtime.get_health().total_ticks > ticks_before; }));
    EXPECT_EQ(AutomationStatus::Running, runtime.status().status) << "AUTO, after loading a good tree";
    EXPECT_EQ(0U, runtime.get_health().error_count);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(bad_path, ec);
    std::filesystem::remove(good_path, ec);
}

// #378: entering AUTO starts a new run even without a new definition. Once
// the condition that made the tree throw has cleared, an ordinary FAILURE in
// the new run is "blocked", not the previous run's "failed".
TEST(BTRuntimeTest, EnteringAutoAfterFailureClearsFailedStatus) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("explode", ParameterType::BOOL, true));
    ASSERT_TRUE(parameter_manager.define("pass", ParameterType::BOOL, false));

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-bt-reenter", kConditionalThrowTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);

    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));
    ASSERT_EQ(AutomationStatus::Failed, runtime.status().status);

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    EXPECT_EQ(AutomationStatus::Failed, runtime.status().status) << "MANUAL keeps the failure visible";
    ASSERT_TRUE(parameter_manager.set("explode", false, err)) << err;

    // Past the >10-tick stall threshold, every tick an ordinary FAILURE.
    const auto ticks_before = runtime.get_health().total_ticks;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(wait_for([&] { return runtime.get_health().total_ticks > ticks_before + 11; }));
    EXPECT_EQ(RuntimeMode::AUTO, mode_manager.current_mode());
    EXPECT_EQ(AutomationStatus::Blocked, runtime.status().status) << "AUTO, ordinary FAILURE in the new run";
    EXPECT_EQ(0U, runtime.get_health().error_count);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// #270: the tick thread runs in every mode but only ticks in AUTO, so outside
// AUTO the tree is loaded but not advancing -- Idle, not Running.
TEST(BTRuntimeTest, ExecutionStatusIsIdleOutsideAuto) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-bt-mode-gate", kSimpleTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);

    std::string err;
    ASSERT_TRUE(runtime.start(err)) << err;
    EXPECT_EQ(AutomationStatus::Idle, runtime.status().status) << "MANUAL";

    // In AUTO the same tree advances, so the gate is what makes the difference.
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime.get_health().total_ticks == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GT(runtime.get_health().total_ticks, 0);
    EXPECT_EQ(AutomationStatus::Running, runtime.status().status) << "AUTO";

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    EXPECT_EQ(AutomationStatus::Idle, runtime.status().status) << "MANUAL";
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::IDLE, err)) << err;
    EXPECT_EQ(AutomationStatus::Idle, runtime.status().status) << "IDLE";
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::FAULT, err)) << err;
    EXPECT_EQ(AutomationStatus::Idle, runtime.status().status) << "FAULT";

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

namespace {

// Like kConditionalThrowTreeXml, but with plain (non-reactive) control nodes,
// which resume at the child that was RUNNING. A tick that throws in the
// Fallback must not leave the Sequence parked on it (#387).
constexpr char kPlainSequenceThrowTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <Sequence>
      <GetParameterBool param="explode" value="{explode}"/>
      <Fallback>
        <CheckBool value="{explode}" expected="false"/>
        <SetBlackboard output_key="anolis.bt_service_context" value="not-a-service-context"/>
      </Fallback>
    </Sequence>
  </BehaviorTree>
</root>
)";

}  // namespace

// #387: a tick exception halts the tree, so the next AUTO entry ticks from the
// root. Without the halt the Sequence resumes at the Fallback, skips the
// re-read of "explode", and throws again although the cause has cleared.
TEST(BTRuntimeTest, AutoReentryAfterTickExceptionTicksFromRoot) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("explode", ParameterType::BOOL, true));

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-bt-plain-reenter", kPlainSequenceThrowTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);

    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    ASSERT_TRUE(parameter_manager.set("explode", false, err)) << err;

    const auto ticks_before = runtime.get_health().total_ticks;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(wait_for([&] {
        return runtime.get_health().total_ticks > ticks_before + 2 || mode_manager.current_mode() != RuntimeMode::AUTO;
    }));
    EXPECT_EQ(RuntimeMode::AUTO, mode_manager.current_mode())
        << "re-entering AUTO rethrew: " << runtime.status().last_error;
    EXPECT_EQ(0U, runtime.get_health().error_count);
    EXPECT_EQ(AutomationStatus::Running, runtime.status().status);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// #387: entering AUTO starts a new run, so the previous run's stall count and
// last error do not carry into it. The first run stalls (every tick an
// ordinary FAILURE) and then throws; the second only fails ordinarily.
TEST(BTRuntimeTest, EnteringAutoClearsStallCountAndLastError) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("explode", ParameterType::BOOL, false));
    ASSERT_TRUE(parameter_manager.define("pass", ParameterType::BOOL, false));

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-bt-run-reset", kConditionalThrowTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);

    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return runtime.status().status == AutomationStatus::Blocked; }));
    ASSERT_TRUE(parameter_manager.set("explode", true, err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));
    ASSERT_GT(runtime.get_health().ticks_since_progress, 10U);
    ASSERT_FALSE(runtime.status().last_error.empty());

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    ASSERT_TRUE(parameter_manager.set("explode", false, err)) << err;

    const auto ticks_before = runtime.get_health().total_ticks;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(wait_for([&] { return runtime.get_health().total_ticks > ticks_before; }));

    // One snapshot: every tick of the new run is a FAILURE, so the stall count
    // equals the ticks this run has made, and nothing from the first.
    const auto health = runtime.get_health();
    EXPECT_EQ(health.total_ticks - ticks_before, health.ticks_since_progress);
    EXPECT_EQ("", health.last_error);
    EXPECT_EQ("", runtime.status().last_error);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

#ifndef _WIN32
namespace {

// Holds the RunJournal's mutex while it lives, so a test can tell which threads
// wait on a journal write. list_events reads its events file under that mutex,
// and opening a FIFO for reading blocks until a writer opens the other end.
class JournalLock {
public:
    JournalLock(anolis::runs::RunJournal& journal, const std::filesystem::path& data_dir)
        : fifo_(data_dir / "runs" / "lock.events.jsonl") {
        if (::mkfifo(fifo_.c_str(), 0600) != 0) {
            return;
        }
        reader_ = std::thread([&journal] { (void)journal.list_events("lock", 1, 0); });
        // Returns once the reader has opened its end, which it does with the
        // journal mutex held.
        writer_.open(fifo_);
    }
    ~JournalLock() { release(); }
    JournalLock(const JournalLock&) = delete;
    JournalLock& operator=(const JournalLock&) = delete;

    bool held() const { return writer_.is_open(); }
    void release() {
        if (writer_.is_open()) {
            writer_.close();
        }
        if (reader_.joinable()) {
            reader_.join();
        }
    }

private:
    std::filesystem::path fifo_;
    std::thread reader_;
    std::ofstream writer_;
};

std::filesystem::path unique_data_dir() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("anolis-bt-journal-" + std::to_string(nonce));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace
#endif

// #282: a tick exception's FAULT transition is journaled behind the fault that
// caused it, and the tick thread does not wait on the journal to make it.
TEST(BTRuntimeTest, TickFaultJournalsCauseBeforeModeChange) {
#ifdef _WIN32
    GTEST_SKIP() << "holds the journal mutex with a POSIX FIFO";
#else
    using namespace anolis::automation;
    using anolis::runs::RunEventCategory;
    const auto dir = unique_data_dir();
    anolis::runs::RunJournal journal(dir, "rt1", 100);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    const auto opened = journal.open(anolis::runs::RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;

    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);

    // Wired as Runtime wires them.
    runtime.set_fault_sink([&journal](const std::string& locus, const std::string& message, int64_t occurred_at) {
        journal.enqueue_fault(locus, message, static_cast<uint64_t>(occurred_at));
    });
    mode_manager.on_mode_change([&](RuntimeMode prev, RuntimeMode next) {
        journal.record_mode_change(mode_to_string(prev), mode_to_string(next), runtime.on_tick_thread());
    });
    std::atomic<bool> fault_dispatched{false};
    mode_manager.on_mode_change([&](RuntimeMode, RuntimeMode next) {
        if (next == RuntimeMode::FAULT) {
            fault_dispatched = true;
        }
    });

    const auto tree_path = write_tree_file("anolis-bt-journal-order", kThrowingTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;

    {
        JournalLock lock(journal, dir);
        ASSERT_TRUE(lock.held());
        ASSERT_TRUE(runtime.start(err)) << err;
        EXPECT_TRUE(wait_for([&] { return fault_dispatched.load(); }))
            << "the tick thread waited on the journal while dispatching its FAULT transition";
    }

    std::optional<uint64_t> fault_seq;
    std::optional<uint64_t> mode_seq;
    ASSERT_TRUE(wait_for([&] {
        for (const auto& ev : journal.list_events(opened.run.run_id, 100, 0)) {
            if (ev.category == RunEventCategory::AutomationFault) {
                fault_seq = ev.sequence;
            } else if (ev.category == RunEventCategory::ModeChange && ev.payload.value("new_mode", "") == "FAULT") {
                mode_seq = ev.sequence;
            }
        }
        return fault_seq.has_value() && mode_seq.has_value();
    })) << "the fault or the FAULT mode change never reached the journal";
    EXPECT_LT(*fault_seq, *mode_seq) << "the FAULT transition sorts before the fault that caused it";

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
    std::filesystem::remove_all(dir, ec);
#endif
}

// #282: a mode change from any other thread (an HTTP request's) stays
// synchronous: set_mode returns only after the event is in the journal.
TEST(BTRuntimeTest, NonTickModeChangeJournalsSynchronously) {
#ifdef _WIN32
    GTEST_SKIP() << "holds the journal mutex with a POSIX FIFO";
#else
    using namespace anolis::automation;
    using anolis::runs::RunEventCategory;
    const auto dir = unique_data_dir();
    anolis::runs::RunJournal journal(dir, "rt1", 100);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    const auto opened = journal.open(anolis::runs::RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;

    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    ASSERT_TRUE(parameter_manager.define("temp_setpoint", ParameterType::DOUBLE, 25.0));
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    mode_manager.on_mode_change([&](RuntimeMode prev, RuntimeMode next) {
        journal.record_mode_change(mode_to_string(prev), mode_to_string(next), runtime.on_tick_thread());
    });

    // A running tick thread, so the check is against that thread, not against
    // whether the engine runs.
    const auto tree_path = write_tree_file("anolis-bt-journal-sync", kSimpleTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    ASSERT_TRUE(runtime.start(err)) << err;

    std::atomic<bool> returned{false};
    std::thread request;
    {
        JournalLock lock(journal, dir);
        ASSERT_TRUE(lock.held());
        request = std::thread([&] {
            std::string set_err;
            (void)mode_manager.set_mode(RuntimeMode::IDLE, set_err);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_FALSE(returned.load()) << "set_mode returned before its mode change was journaled";
    }
    request.join();

    // No polling: the write happened before set_mode returned.
    bool found = false;
    for (const auto& ev : journal.list_events(opened.run.run_id, 100, 0)) {
        if (ev.category == RunEventCategory::ModeChange && ev.payload.value("new_mode", "") == "IDLE") {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "the MANUAL -> IDLE mode change was not in the journal when set_mode returned";

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
    std::filesystem::remove_all(dir, ec);
#endif
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

namespace {

std::string single_node_tree(const std::string& node) {
    return R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    )" + node +
           R"(
  </BehaviorTree>
</root>
)";
}

}  // namespace

// #596: under on_terminal: stop a root that returns SUCCESS ends the run. No
// more ticks, and the status is completed until a new run.
TEST(BTRuntimeTest, StopOnTerminalReportsCompletedAndTicksNoMore) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    runtime.set_stop_on_terminal(true);
    const auto tree_path = write_tree_file("anolis-596-completed", single_node_tree("<AlwaysSuccess/>"));
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    ASSERT_TRUE(wait_for([&] { return runtime.status().status == AutomationStatus::Completed; }));
    const auto ticks = runtime.get_health().total_ticks;
    EXPECT_EQ(ticks, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(runtime.get_health().total_ticks, ticks) << "the tree was ticked again after it completed";
    EXPECT_EQ(runtime.status().status, AutomationStatus::Completed);

    // Re-entering AUTO starts a new run, which ticks once and completes again.
    // The tick loop samples the mode once per period (100 ms at 10 Hz), so it
    // sees a new run only if it observes MANUAL first: wait past a period.
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    EXPECT_TRUE(wait_for([&] { return runtime.get_health().total_ticks == ticks + 1; }));
    EXPECT_TRUE(wait_for([&] { return runtime.status().status == AutomationStatus::Completed; }));

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

TEST(BTRuntimeTest, StopOnTerminalReportsFailedNotBlocked) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    runtime.set_stop_on_terminal(true);
    const auto tree_path = write_tree_file("anolis-596-failed", single_node_tree("<AlwaysFailure/>"));
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    ASSERT_TRUE(wait_for([&] { return runtime.status().status == AutomationStatus::Failed; }));
    // Past the >10-tick stall heuristic's window at 10 Hz: still failed, one tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_EQ(runtime.status().status, AutomationStatus::Failed);
    EXPECT_EQ(runtime.get_health().total_ticks, 1u);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// The default re-ticks a terminal root, as before.
TEST(BTRuntimeTest, RepeatOnTerminalKeepsTicking) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    const auto tree_path = write_tree_file("anolis-596-repeat", single_node_tree("<AlwaysSuccess/>"));
    ASSERT_TRUE(runtime.load_tree(tree_path.string()));
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    EXPECT_TRUE(wait_for([&] { return runtime.get_health().total_ticks >= 3; }));
    EXPECT_EQ(runtime.status().status, AutomationStatus::Running);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

namespace {

// anolis#678: a call the router refuses on every tick. `ghost0/pump0` is in no
// registry, so the router refuses each tick's call: no provider ghost0.
constexpr char kRefusedCallTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <CallDevice device_handle="ghost0/pump0" function_name="dose_on"/>
  </BehaviorTree>
</root>
)";

}  // namespace

// anolis#678: an automation whose device call is refused or fails, three times
// in a row (the default limit), faults the runtime: the engine-fault path
// enters FAULT, which runs the AUTO -> FAULT hooks that drive the safe state,
// and the reason names the call and its last error. Before, the tree was
// re-ticked forever and nothing said the calls were failing.
TEST(BTRuntimeTest, RepeatedCallFailuresDriveFaultNamingTheCall) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-678-refused", kRefusedCallTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }))
        << "the refused call kept the runtime in " << mode_to_string(mode_manager.current_mode());

    const auto health = runtime.get_health();
    EXPECT_NE(health.last_error.find("ghost0/pump0 dose_on"), std::string::npos) << health.last_error;
    EXPECT_NE(health.last_error.find("3 times in a row"), std::string::npos) << health.last_error;
    EXPECT_NE(health.last_error.find("Provider not found: ghost0"), std::string::npos) << health.last_error;
    // The first two failures were ordinary FAILURE ticks; the third faulted.
    EXPECT_EQ(2u, health.total_ticks);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

namespace {

// anolis#806: args read from the blackboard that are not JSON. Every tick's
// CallDevice fails before any call goes out.
constexpr char kBadArgsTreeXml[] = R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <Sequence>
      <SetBlackboard output_key="pump_args" value="not json"/>
      <CallDevice device_handle="ghost0/pump0" function_name="dose_on" args="{pump_args}"/>
    </Sequence>
  </BehaviorTree>
</root>
)";

}  // namespace

// anolis#806: an args failure is a failed call, so it is counted like one and
// three in a row fault the runtime. Before, the node failed outside the
// accounting and the runtime stayed in AUTO, logging an error every tick.
TEST(BTRuntimeTest, RepeatedArgsFailuresDriveFault) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-806-bad-args", kBadArgsTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;

    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }))
        << "bad args kept the runtime in " << mode_to_string(mode_manager.current_mode());
    const auto health = runtime.get_health();
    EXPECT_NE(health.last_error.find("ghost0/pump0 dose_on"), std::string::npos) << health.last_error;
    EXPECT_NE(health.last_error.find("JSON parse error"), std::string::npos) << health.last_error;

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// anolis#678: entering AUTO starts a new run, so the count starts again: after
// a recovery through MANUAL, it takes another three failures to fault, not one.
TEST(BTRuntimeTest, EnteringAutoRestartsTheCallFailureCount) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto tree_path = write_tree_file("anolis-678-reentry", kRefusedCallTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));
    const auto ticks_at_first_fault = runtime.get_health().total_ticks;

    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::MANUAL, err)) << err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));

    EXPECT_EQ(ticks_at_first_fault + 2, runtime.get_health().total_ticks)
        << "a failure carried over from the previous run counted toward this one";

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// anolis#678: each failed call is an automation_call_failed event and an entry
// in status()'s call_failures, so the operator sees the failures before the
// fault, not only the fault.
TEST(BTRuntimeTest, EachFailedCallEmitsAnEventAndIsCountedInStatus) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    auto emitter = std::make_shared<anolis::events::EventEmitter>();
    auto sub = emitter->subscribe(anolis::events::EventFilter::all(), 0, "test");

    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    runtime.set_event_emitter(emitter);
    const auto tree_path = write_tree_file("anolis-678-events", kRefusedCallTreeXml);
    ASSERT_TRUE(runtime.load(AutomationDefinitionRef{tree_path.string()}).ok);
    std::string err;
    ASSERT_TRUE(mode_manager.set_mode(RuntimeMode::AUTO, err)) << err;
    ASSERT_TRUE(runtime.start(err)) << err;
    ASSERT_TRUE(wait_for([&] { return mode_manager.current_mode() == RuntimeMode::FAULT; }));

    std::vector<uint64_t> counts;
    bool fault_seen = false;
    while (auto event = sub->try_pop()) {
        if (const auto* failed = std::get_if<anolis::events::AutomationCallFailedEvent>(&*event)) {
            EXPECT_EQ("ghost0/pump0", failed->device_handle);
            EXPECT_EQ("dose_on", failed->function);
            EXPECT_EQ("Provider not found: ghost0", failed->error);
            counts.push_back(failed->consecutive_failures);
        } else if (std::holds_alternative<anolis::events::AutomationFaultEvent>(*event)) {
            fault_seen = true;
        }
    }
    EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), counts);
    EXPECT_TRUE(fault_seen);

    const auto status = runtime.status();
    ASSERT_EQ(1u, status.call_failures.size());
    EXPECT_EQ("ghost0/pump0", status.call_failures[0].device_handle);
    EXPECT_EQ("dose_on", status.call_failures[0].function);
    EXPECT_EQ(3u, status.call_failures[0].consecutive_failures);
    EXPECT_EQ("Provider not found: ghost0", status.call_failures[0].last_error);
    EXPECT_GT(status.call_failures[0].last_failure_at_epoch_ms, 0);

    runtime.stop();
    std::error_code ec;
    std::filesystem::remove(tree_path, ec);
}

// anolis#678: the count is per device call and in a row: a success resets it,
// keeping the last error, and another call's failures do not add to it.
TEST(BTRuntimeTest, ASuccessResetsTheCallFailureCount) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);

    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt1", "set_pwm", false, "brake engaged"));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt1", "set_pwm", false, "brake engaged"));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt1", "set_pwm", true, ""));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt0", "set_pwm", false, "other"));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt0", "set_pwm", false, "other"));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt1", "set_pwm", false, "brake engaged"));
    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt1", "set_pwm", false, "brake engaged"));

    auto status = runtime.status();
    ASSERT_EQ(2u, status.call_failures.size());
    EXPECT_EQ(2u, status.call_failures[0].consecutive_failures);
    EXPECT_EQ(2u, status.call_failures[1].consecutive_failures);

    EXPECT_EQ("automation call bread0/dcmt1 set_pwm failed 3 times in a row; last error: brake engaged",
              runtime.record_call_result("bread0/dcmt1", "set_pwm", false, "brake engaged"));

    EXPECT_EQ("", runtime.record_call_result("bread0/dcmt0", "set_pwm", true, ""));
    status = runtime.status();
    EXPECT_EQ(0u, status.call_failures[1].consecutive_failures);
    EXPECT_EQ("other", status.call_failures[1].last_error);
}

// anolis#678: the limit is settable, down to 1, which faults on the first failure.
TEST(BTRuntimeTest, CallFailureLimitOfOneFaultsOnTheFirstFailure) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    runtime.set_max_consecutive_call_failures(1);

    EXPECT_EQ("automation call ghost0/pump0 dose_on failed once; last error: refused",
              runtime.record_call_result("ghost0/pump0", "dose_on", false, "refused"));
}

namespace {

// Loads `xml` into a fresh BTRuntime and returns the outcome (#805).
anolis::automation::LoadOutcome load_tree_xml(const std::string& stem, const std::string& xml) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    const auto path = write_tree_file(stem, xml);
    auto outcome = runtime.load(AutomationDefinitionRef{path.string()});
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return outcome;
}

std::string one_node_tree(const std::string& attribute) {
    return R"(<root BTCPP_format="4" main_tree_to_execute="Main"><BehaviorTree ID="Main">)"
           R"(<AlwaysSuccess name="guarded" )" +
           attribute + R"(/></BehaviorTree></root>)";
}

}  // namespace

// #805: BehaviorTree.CPP stores an underscore attribute it does not know and
// ignores it, so a misspelt guard on a node did nothing. The load refuses it,
// naming the node and the attribute.
TEST(BTRuntimeUnknownAttributeTest, AMisspeltPreconditionIsRefused) {
    const auto outcome = load_tree_xml("anolis-bt-skipif", one_node_tree(R"(_skipif="true")"));
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.error.find("_skipif"), std::string::npos) << outcome.error;
    EXPECT_NE(outcome.error.find("guarded"), std::string::npos) << outcome.error;
}

// A newer BehaviorTree.CPP's condition, which the linked 4.9.0 does not run.
// Linking 4.11 or later, which runs _onlyIf, turns this red on purpose.
TEST(BTRuntimeUnknownAttributeTest, AConditionTheLinkedVersionLacksIsRefused) {
    const auto outcome = load_tree_xml("anolis-bt-onlyif", one_node_tree(R"(_onlyIf="false")"));
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.error.find("_onlyIf"), std::string::npos) << outcome.error;
}

TEST(BTRuntimeUnknownAttributeTest, AnAttributeInsideASubTreeIsRefused) {
    const std::string xml = R"(<root BTCPP_format="4" main_tree_to_execute="Main">)"
                            R"(<BehaviorTree ID="Main"><SubTree ID="Inner"/></BehaviorTree>)"
                            R"(<BehaviorTree ID="Inner"><AlwaysSuccess name="deep" _sucessIf="true"/></BehaviorTree>)"
                            R"(</root>)";
    const auto outcome = load_tree_xml("anolis-bt-subtree-attr", xml);
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.error.find("_sucessIf"), std::string::npos) << outcome.error;
    EXPECT_NE(outcome.error.find("deep"), std::string::npos) << outcome.error;
}

// A tree saved by BehaviorTree.CPP with its metadata, which adds _uid and, on
// a SubTree, _fullpath, loads again: why the check allows those two.
TEST(BTRuntimeUnknownAttributeTest, ATreeSavedWithMetadataLoads) {
    BT::BehaviorTreeFactory factory;
    const auto saved = BT::WriteTreeToXML(
        factory.createTreeFromText(R"(<root BTCPP_format="4" main_tree_to_execute="Main">)"
                                   R"(<BehaviorTree ID="Main"><SubTree ID="Inner"/></BehaviorTree>)"
                                   R"(<BehaviorTree ID="Inner"><AlwaysSuccess/></BehaviorTree></root>)"),
        true, false);
    ASSERT_NE(saved.find("_uid="), std::string::npos) << saved;
    ASSERT_NE(saved.find("_fullpath="), std::string::npos) << saved;
    // The writer leaves out main_tree_to_execute, which a file with two trees
    // needs to load at all.
    std::string loadable = saved;
    const auto root_at = loadable.find("<root");
    ASSERT_NE(root_at, std::string::npos) << saved;
    loadable.insert(root_at + 5, R"( main_tree_to_execute="Main")");
    const auto outcome = load_tree_xml("anolis-bt-saved-metadata", loadable);
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

// BehaviorTree.CPP removes its own conditions before the check sees a node's
// attributes, so these load because it runs them; _description and _uid load
// because the check allows them.
TEST(BTRuntimeUnknownAttributeTest, TheAllowedAttributesLoad) {
    for (const std::string attribute :
         {R"(_skipIf="false")", R"(_failureIf="false")", R"(_successIf="false")", R"(_while="true")",
          R"(_onSuccess="x:=1")", R"(_onFailure="x:=1")", R"(_onHalted="x:=1")", R"(_post="x:=1")",
          R"(_description="what it does")", R"(_uid="7")"}) {
        const auto outcome = load_tree_xml("anolis-bt-allowed", one_node_tree(attribute));
        EXPECT_TRUE(outcome.ok) << attribute << ": " << outcome.error;
    }
    const std::string autoremap = R"(<root BTCPP_format="4" main_tree_to_execute="Main">)"
                                  R"(<BehaviorTree ID="Main"><SubTree ID="Inner" _autoremap="true"/></BehaviorTree>)"
                                  R"(<BehaviorTree ID="Inner"><AlwaysSuccess/></BehaviorTree></root>)";
    const auto outcome = load_tree_xml("anolis-bt-autoremap", autoremap);
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

namespace {

// Loads a one-node tree into a runtime that declares a double parameter
// `setpoint`, a bool parameter `flag` and an int64 parameter `count`, and
// checks it against provider ids {sim0} (#808).
anolis::automation::BTRuntime::ConfigCheck check_one_node(const std::string& node) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    ParameterManager parameter_manager;
    EXPECT_TRUE(parameter_manager.define("setpoint", ParameterType::DOUBLE, 25.0));
    EXPECT_TRUE(parameter_manager.define("flag", ParameterType::BOOL, false));
    EXPECT_TRUE(parameter_manager.define("count", ParameterType::INT64, int64_t{3}));
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, &parameter_manager);
    const auto path = write_tree_file("anolis-bt-config-check", R"(<root BTCPP_format="4"><BehaviorTree ID="Main">)" +
                                                                    node + "</BehaviorTree></root>");
    const auto outcome = runtime.load(AutomationDefinitionRef{path.string()});
    EXPECT_TRUE(outcome.ok) << outcome.error;
    auto check = runtime.check_against_config({"sim0"});
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return check;
}

bool mentions(const anolis::automation::BTRuntime::ConfigCheck& check, const std::string& text) {
    for (const auto& refusal : check.refusals) {
        if (refusal.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(BTRuntimeConfigCheckTest, AnUndeclaredParameterIsRefused) {
    const auto check = check_one_node(R"(<GetParameter param="not_declared" value="{v}"/>)");
    ASSERT_EQ(1U, check.refusals.size());
    EXPECT_TRUE(mentions(check, "not_declared"));
}

TEST(BTRuntimeConfigCheckTest, AParameterOfTheWrongTypeIsRefused) {
    EXPECT_TRUE(mentions(check_one_node(R"(<GetParameter param="flag" value="{v}"/>)"), "flag"));
    EXPECT_TRUE(mentions(check_one_node(R"(<GetParameterBool param="setpoint" value="{v}"/>)"), "setpoint"));
    EXPECT_TRUE(mentions(check_one_node(R"(<GetParameterInt64 param="setpoint" value="{v}"/>)"), "setpoint"));
}

TEST(BTRuntimeConfigCheckTest, DeclaredParametersOfTheRightTypePass) {
    EXPECT_TRUE(check_one_node(R"(<GetParameter param="setpoint" value="{v}"/>)").refusals.empty());
    EXPECT_TRUE(check_one_node(R"(<GetParameter param="count" value="{v}"/>)").refusals.empty())
        << "GetParameter reads an int64 too";
    EXPECT_TRUE(check_one_node(R"(<GetParameterInt64 param="count" value="{v}"/>)").refusals.empty());
    EXPECT_TRUE(check_one_node(R"(<GetParameterBool param="flag" value="{v}"/>)").refusals.empty());
}

TEST(BTRuntimeConfigCheckTest, ADeviceOfAnUndeclaredProviderIsRefused) {
    for (const std::string node : {R"(<CallDevice device_handle="ghost0/dev" function_name="f"/>)",
                                   R"(<ReadSignal device_handle="ghost0/dev" signal_id="s" value="{v}"/>)",
                                   R"(<CheckQuality device_handle="ghost0/dev" signal_id="s"/>)"}) {
        EXPECT_TRUE(mentions(check_one_node(node), "ghost0")) << node;
    }
    for (const std::string handle : {"nodevice", "/dev", "sim0/"}) {
        EXPECT_TRUE(mentions(check_one_node(R"(<CallDevice device_handle=")" + handle + R"(" function_name="f"/>)"),
                             "is not provider_id/device_id"))
            << handle;
    }
    EXPECT_TRUE(check_one_node(R"(<CallDevice device_handle="sim0/dev" function_name="f"/>)").refusals.empty());
}

TEST(BTRuntimeConfigCheckTest, BlackboardPortsAreCountedNotChecked) {
    const auto check = check_one_node(R"(<Sequence><GetParameter param="{name}" value="{v}"/>)"
                                      R"(<CallDevice device_handle="{handle}" function_name="f"/></Sequence>)");
    EXPECT_TRUE(check.refusals.empty());
    EXPECT_EQ(2, check.unchecked_ports);
}

namespace {

std::string main_tree(const std::string& body, const std::string& more_trees = "") {
    return R"(<root BTCPP_format="4" main_tree_to_execute="Main"><BehaviorTree ID="Main">)" + body + "</BehaviorTree>" +
           more_trees + "</root>";
}

bool refused_naming(const anolis::automation::LoadOutcome& outcome, const std::string& node, const std::string& name) {
    // The node is named by its full path, which ends in its name.
    return !outcome.ok && outcome.error.find(node + "' (") != std::string::npos &&
           outcome.error.find("reads '" + name + "'") != std::string::npos;
}

}  // namespace

// #809: a condition that reads a name nothing sets loaded, and its first tick
// threw, which the runtime turns into FAULT. The load refuses it, naming the
// node and the name; behind an initialiser the same tree loads and runs.
TEST(BTRuntimeScriptNameTest, AConditionReadingANameNothingSetsIsRefused) {
    const std::string unset = main_tree(R"(<AlwaysSuccess name="heater" _skipIf="!heater_on"/>)");
    BT::BehaviorTreeFactory factory;
    EXPECT_ANY_THROW(factory.createTreeFromText(unset).tickOnce()) << "BehaviorTree.CPP alone throws at tick";
    EXPECT_TRUE(refused_naming(load_tree_xml("anolis-bt-809-unset", unset), "heater", "heater_on"));

    const std::string initialised = main_tree(
        R"(<Sequence><Script code="heater_on := false"/><AlwaysSuccess name="heater" _skipIf="!heater_on"/></Sequence>)");
    EXPECT_EQ(BT::NodeStatus::SUCCESS, factory.createTreeFromText(initialised).tickOnce());
    const auto outcome = load_tree_xml("anolis-bt-809-initialised", initialised);
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

TEST(BTRuntimeScriptNameTest, ANameAnOutputPortWritesLoads) {
    for (const std::string writer :
         {R"(<BuildArgsJson json="{level}"/>)", R"(<SetBlackboard value="3" output_key="level"/>)"}) {
        const auto outcome =
            load_tree_xml("anolis-bt-809-port",
                          main_tree("<Sequence>" + writer + R"(<ScriptCondition code="level != ''"/></Sequence>)"));
        EXPECT_TRUE(outcome.ok) << writer << ": " << outcome.error;
    }
}

// SetBlackboard reads output_key as the name of the entry it writes, so
// output_key="{level}" writes nothing called level: it reads the name from
// level, which is empty, and throws at tick.
TEST(BTRuntimeScriptNameTest, SetBlackboardWithABlackboardKeyDoesNotSetIt) {
    const std::string xml = main_tree(
        R"(<Sequence><SetBlackboard value="3" output_key="{level}"/><ScriptCondition name="check" code="level > 0"/></Sequence>)");
    BT::BehaviorTreeFactory factory;
    EXPECT_ANY_THROW(factory.createTreeFromText(xml).tickOnce()) << "BehaviorTree.CPP alone throws at tick";
    EXPECT_TRUE(refused_naming(load_tree_xml("anolis-bt-809-setbb-key", xml), "check", "level"));
}

// A literal on a SubTree port is written into the SubTree's blackboard when
// the tree is built, so a script inside reads a set name.
TEST(BTRuntimeScriptNameTest, ALiteralOnASubTreePortLoads) {
    const std::string xml =
        main_tree(R"(<SubTree ID="Inner" setpoint="37"/>)",
                  R"(<BehaviorTree ID="Inner"><ScriptCondition code="setpoint > 0"/></BehaviorTree>)");
    BT::BehaviorTreeFactory factory;
    EXPECT_EQ(BT::NodeStatus::SUCCESS, factory.createTreeFromText(xml).tickOnce());
    const auto outcome = load_tree_xml("anolis-bt-809-subtree-literal", xml);
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

TEST(BTRuntimeScriptNameTest, APreconditionReadingANameNothingSetsIsRefused) {
    EXPECT_TRUE(refused_naming(
        load_tree_xml("anolis-bt-809-precondition",
                      main_tree(R"(<Precondition name="gate" if="armed"><AlwaysSuccess/></Precondition>)")),
        "gate", "armed"));
}

// An entry a port declares exists from the build, so '=' may assign it
// without ':=', and a later read of it is of a set name.
TEST(BTRuntimeScriptNameTest, AnAssignmentToAnExistingEntrySetsIt) {
    const auto outcome =
        load_tree_xml("anolis-bt-809-existing",
                      main_tree(R"(<Sequence><Script code="fn = 'set'"/><ScriptCondition code="fn == 'set'"/>)"
                                R"(<CallDevice device_handle="sim0/d" function_name="{fn}"/></Sequence>)"));
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

TEST(BTRuntimeScriptNameTest, AssignmentToANameNothingCreatesIsRefused) {
    EXPECT_TRUE(
        refused_naming(load_tree_xml("anolis-bt-809-assign", main_tree(R"(<Script name="count" code="total = 1"/>)")),
                       "count", "total"))
        << "'=' needs the entry to exist; ':=' creates it";
    EXPECT_TRUE(load_tree_xml("anolis-bt-809-create", main_tree(R"(<Script code="total := 0; total = 1"/>)")).ok);
    EXPECT_TRUE(refused_naming(
        load_tree_xml("anolis-bt-809-post", main_tree(R"(<AlwaysSuccess name="tally" _onSuccess="total += 1"/>)")),
        "tally", "total"));
}

// A name a port writes in the main tree is not seen in a SubTree that does
// not remap it, where BehaviorTree.CPP reports "Variable not found" at tick;
// with _autoremap it is, and a root name (@) is seen from any SubTree.
TEST(BTRuntimeScriptNameTest, ASubTreeSeesOnlyTheNamesItsBlackboardReaches) {
    const std::string inner =
        R"(<BehaviorTree ID="Inner"><ScriptCondition name="check" code="level != ''"/></BehaviorTree>)";
    const std::string writer = R"(<BuildArgsJson json="{level}"/>)";
    EXPECT_TRUE(
        refused_naming(load_tree_xml("anolis-bt-809-isolated",
                                     main_tree("<Sequence>" + writer + R"(<SubTree ID="Inner"/></Sequence>)", inner)),
                       "check", "level"));
    EXPECT_TRUE(
        load_tree_xml("anolis-bt-809-autoremap",
                      main_tree("<Sequence>" + writer + R"(<SubTree ID="Inner" _autoremap="true"/></Sequence>)", inner))
            .ok);
    EXPECT_TRUE(load_tree_xml("anolis-bt-809-root",
                              main_tree(R"(<Sequence><Script code="@flag := true"/><SubTree ID="Deep"/></Sequence>)",
                                        R"(<BehaviorTree ID="Deep"><ScriptCondition code="@flag"/></BehaviorTree>)"))
                    .ok);
}

// A script given through a SubTree port is read from the literal that built
// it: the names it creates count, and the names it reads are checked.
TEST(BTRuntimeScriptNameTest, AScriptPassedIntoASubTreeIsRead) {
    const std::string creates = main_tree(
        R"(<SubTree ID="Inner" code="y := 1"/>)",
        R"(<BehaviorTree ID="Inner"><Sequence><Script code="{code}"/><ScriptCondition code="y > 0"/></Sequence></BehaviorTree>)");
    BT::BehaviorTreeFactory factory;
    EXPECT_EQ(BT::NodeStatus::SUCCESS, factory.createTreeFromText(creates).tickOnce());
    const auto outcome = load_tree_xml("anolis-bt-809-passed-creates", creates);
    EXPECT_TRUE(outcome.ok) << outcome.error;

    const std::string reads =
        main_tree(R"(<SubTree ID="Inner" code="nope > 1"/>)",
                  R"(<BehaviorTree ID="Inner"><ScriptCondition name="check" code="{code}"/></BehaviorTree>)");
    EXPECT_ANY_THROW(factory.createTreeFromText(reads).tickOnce()) << "BehaviorTree.CPP alone throws at tick";
    EXPECT_TRUE(refused_naming(load_tree_xml("anolis-bt-809-passed-reads", reads), "check", "nope"));
}

namespace {

// The form Groot2 1.10.0 saves with default preferences (#818): every port a
// node's model declares with no default, or an empty one, that the node
// leaves out is written as attr="".
std::string groot2_default_save(const std::string& xml) {
    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal");
    factory.registerNodeType<anolis::automation::CallDeviceNode>("CallDevice");
    factory.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality");
    factory.registerNodeType<anolis::automation::GetParameterNode>("GetParameter");
    factory.registerNodeType<anolis::automation::GetParameterBoolNode>("GetParameterBool");
    factory.registerNodeType<anolis::automation::GetParameterInt64Node>("GetParameterInt64");
    factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
    factory.registerNodeType<anolis::automation::PeriodicPulseWindowNode>("PeriodicPulseWindow");
    factory.registerNodeType<anolis::automation::RequestedPulseWindowNode>("RequestedPulseWindow");
    factory.registerNodeType<anolis::automation::EmitOnChangeOrIntervalNode>("EmitOnChangeOrInterval");
    factory.registerNodeType<anolis::automation::BuildArgsJsonNode>("BuildArgsJson");
    tinyxml2::XMLDocument doc;
    EXPECT_EQ(tinyxml2::XML_SUCCESS, doc.Parse(xml.c_str()));
    std::function<void(tinyxml2::XMLElement&)> fill = [&](tinyxml2::XMLElement& parent) {
        for (auto* element = parent.FirstChildElement(); element != nullptr; element = element->NextSiblingElement()) {
            if (const auto manifest = factory.manifests().find(element->Name());
                manifest != factory.manifests().end()) {
                for (const auto& [port, info] : manifest->second.ports) {
                    if (info.defaultValueString().empty() && element->Attribute(port.c_str()) == nullptr) {
                        element->SetAttribute(port.c_str(), "");
                    }
                }
            }
            fill(*element);
        }
    };
    for (auto* tree = doc.RootElement()->FirstChildElement("BehaviorTree"); tree != nullptr;
         tree = tree->NextSiblingElement("BehaviorTree")) {
        fill(*tree);
    }
    tinyxml2::XMLPrinter printer;
    doc.Print(&printer);
    return printer.CStr();
}

// Loads `xml` into a fresh BTRuntime and ticks it once; "" when it does not load.
std::string load_and_tick(const std::string& stem, const std::string& xml) {
    using namespace anolis::automation;
    anolis::registry::DeviceRegistry registry;
    anolis::state::StateCache state_cache(registry, 100);
    anolis::control::CallRouter call_router(registry, state_cache);
    anolis::provider::ProviderRegistry provider_registry;
    ModeManager mode_manager(RuntimeMode::MANUAL);
    BTRuntime runtime(state_cache, call_router, provider_registry, mode_manager, nullptr);
    const auto path = write_tree_file(stem, xml);
    const auto outcome = runtime.load(AutomationDefinitionRef{path.string()});
    std::string status;
    if (outcome.ok) {
        status = BT::toStr(runtime.tick());
    } else {
        ADD_FAILURE() << outcome.error;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return status;
}

}  // namespace

// #818: Groot2's default save wrote now_ms="" on PeriodicPulseWindow, which
// BehaviorTree.CPP cannot convert, and an empty argN_str beside an argN_num,
// which is two value sources. Saved that way the tree loads and the window
// and the arguments come out as they did before the save.
TEST(BTRuntimeGroot2SaveTest, ATreeSavedWithItsEmptyPortsRunsTheSame) {
    const std::string original = main_tree(
        R"(<Sequence>)"
        R"(<BuildArgsJson arg1_name="pwm" arg1_type="double" arg1_num="40" arg2_name="brake" arg2_bool="false" json="{args}"/>)"
        R"(<PeriodicPulseWindow interval_s="60" pulse_s="60" active="{on}"/>)"
        R"(<ScriptCondition code="on"/>)"
        R"(<ScriptCondition code="args == '{&quot;brake&quot;:false,&quot;pwm&quot;:40.0}'"/>)"
        R"(</Sequence>)");
    const std::string saved = groot2_default_save(original);
    ASSERT_NE(saved.find(R"(now_ms="")"), std::string::npos) << saved;
    ASSERT_NE(saved.find(R"(arg1_str="")"), std::string::npos) << saved;
    EXPECT_EQ("SUCCESS", load_and_tick("anolis-bt-818-original", original));
    EXPECT_EQ("SUCCESS", load_and_tick("anolis-bt-818-saved", saved));
}

TEST(BTRuntimeGroot2SaveTest, EveryTreeInTheRepoLoadsInItsDefaultSaveForm) {
    const std::filesystem::path source(ANOLIS_SOURCE_DIR);
    int trees = 0;
    for (const auto& dir :
         {source / "tests/integration/fixtures/behaviors", source / "tools/derived-signals/tree-probe"}) {
        for (const auto& file : std::filesystem::directory_iterator(dir)) {
            if (file.path().extension() != ".xml") {
                continue;
            }
            std::ifstream in(file.path());
            const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto original = load_tree_xml("anolis-bt-818-repo", xml);
            ASSERT_TRUE(original.ok) << file.path() << ": " << original.error;
            const auto saved = load_tree_xml("anolis-bt-818-repo-saved", groot2_default_save(xml));
            EXPECT_TRUE(saved.ok) << file.path() << ": " << saved.error;
            ++trees;
        }
    }
    EXPECT_GE(trees, 5) << "the fixture trees were not found";
}

// "" cannot mean a default that is not empty, so the load refuses it.
TEST(BTRuntimeGroot2SaveTest, AnEmptyPortWithADefaultIsRefused) {
    const auto outcome = load_tree_xml(
        "anolis-bt-818-default", main_tree(R"(<PeriodicPulseWindow name="dosing" interval_s="" active="{on}"/>)"));
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.error.find("dosing"), std::string::npos) << outcome.error;
    EXPECT_NE(outcome.error.find("interval_s"), std::string::npos) << outcome.error;
    EXPECT_NE(outcome.error.find("'60'"), std::string::npos) << outcome.error;
}

// The cost: an empty argN_str is the port left out, so a lone one is a slot
// with a name and no value, which BuildArgsJson refuses. It gave {"p":""}.
TEST(BTRuntimeGroot2SaveTest, ALoneEmptyStringArgumentIsAMissingValue) {
    EXPECT_EQ("FAILURE", load_and_tick("anolis-bt-818-lone-str",
                                       main_tree(R"(<BuildArgsJson arg1_name="p" arg1_str="" json="{j}"/>)")));
    EXPECT_EQ("SUCCESS",
              load_and_tick("anolis-bt-818-bb-str",
                            main_tree(R"(<Sequence><Script code="empty := ''"/>)"
                                      R"(<BuildArgsJson arg1_name="p" arg1_str="{empty}" json="{j}"/>)"
                                      R"(<ScriptCondition code="j == '{&quot;p&quot;:&quot;&quot;}'"/></Sequence>)")))
        << "an empty string from the blackboard is still an argument";
}

// The rewritten tree is parsed from text, where BehaviorTree.CPP would read a
// relative <include> against the working directory; it still resolves
// against the tree's own directory.
TEST(BTRuntimeGroot2SaveTest, ARelativeIncludeStillResolvesInARewrittenTree) {
    const auto included =
        write_tree_file("anolis-bt-818-included",
                        R"(<root BTCPP_format="4"><BehaviorTree ID="Inner"><AlwaysSuccess/></BehaviorTree></root>)");
    const std::string xml = R"(<root BTCPP_format="4" main_tree_to_execute="Main"><include path=")" +
                            included.filename().string() +
                            R"("/><BehaviorTree ID="Main"><Sequence><PeriodicPulseWindow now_ms="" active="{on}"/>)"
                            R"(<SubTree ID="Inner"/></Sequence></BehaviorTree></root>)";
    EXPECT_EQ("SUCCESS", load_and_tick("anolis-bt-818-include", xml));
    std::error_code ec;
    std::filesystem::remove(included, ec);
}

// Only Anolis nodes are read this way. BehaviorTree.CPP's own nodes keep an
// empty attribute as written: SetBlackboard value="" sets an empty string,
// and dropped it would throw at tick.
TEST(BTRuntimeGroot2SaveTest, ABuiltInNodeKeepsItsEmptyAttribute) {
    EXPECT_EQ("SUCCESS",
              load_and_tick("anolis-bt-818-builtin", main_tree(R"(<Sequence><SetBlackboard value="" output_key="x"/>)"
                                                               R"(<ScriptCondition code="x == ''"/></Sequence>)")));
}

// An empty port that names what a node acts on stays as written, so the
// config check refuses it as before; CallDevice's args="" still means no
// arguments (#806) and loads.
TEST(BTRuntimeGroot2SaveTest, NamingPortsAndEmptyArgsKeepTheirMeaning) {
    EXPECT_TRUE(mentions(check_one_node(R"(<CallDevice device_handle="" function_name="f"/>)"),
                         "is not provider_id/device_id"));
    EXPECT_TRUE(mentions(check_one_node(R"(<GetParameter param="" value="{v}"/>)"), "does not declare"));
    const auto outcome = load_tree_xml("anolis-bt-818-args",
                                       main_tree(R"(<CallDevice device_handle="sim0/d" function_name="f" args=""/>)"));
    EXPECT_TRUE(outcome.ok) << outcome.error;
}

// Every <BehaviorTree> in the file is read, not only the first.
TEST(BTRuntimeGroot2SaveTest, AnEmptyPortInASubTreeIsDropped) {
    EXPECT_EQ("SUCCESS",
              load_and_tick("anolis-bt-818-subtree",
                            main_tree(R"(<SubTree ID="Inner"/>)",
                                      R"(<BehaviorTree ID="Inner"><Sequence><PeriodicPulseWindow interval_s="60" )"
                                      R"(pulse_s="60" now_ms="" active="{on}"/><ScriptCondition code="on"/></Sequence>)"
                                      R"(</BehaviorTree>)")));
}

// An input the node reads with no fallback keeps an empty attribute, as on
// main: EmitOnChangeOrInterval key="" still emits, and CheckBool value=""
// is still refused when the tree is built.
TEST(BTRuntimeGroot2SaveTest, RequiredInputsKeepTheirEmptyAttribute) {
    EXPECT_EQ("SUCCESS", load_and_tick("anolis-bt-818-key",
                                       main_tree(R"(<Sequence><EmitOnChangeOrInterval key="" emit="{e}" reason="{r}"/>)"
                                                 R"(<ScriptCondition code="e"/></Sequence>)")));
    EXPECT_FALSE(load_tree_xml("anolis-bt-818-value", main_tree(R"(<CheckBool value="" expected="true"/>)")).ok);
    EXPECT_FALSE(load_tree_xml("anolis-bt-818-request",
                               main_tree(R"(<RequestedPulseWindow request="" pulse_s="2" active="{a}" queued="{q}"/>)"))
                     .ok);
}

// An output named like a required input is dropped like any other: Groot2's
// value="" on a ReadSignal that uses only its quality loads.
TEST(BTRuntimeGroot2SaveTest, AnEmptyOutputNamedValueIsDropped) {
    for (const std::string node : {R"(<ReadSignal device_handle="sim0/d" signal_id="t" value="" quality="{q}"/>)",
                                   R"(<GetParameter param="sp" value=""/>)"}) {
        const auto outcome = load_tree_xml("anolis-bt-818-output", main_tree(node));
        EXPECT_TRUE(outcome.ok) << node << ": " << outcome.error;
    }
}
