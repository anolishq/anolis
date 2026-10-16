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
