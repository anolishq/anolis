#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// BehaviorTree.CPP includes
#include <behaviortree_cpp/basic_types.h>

#include "automation/automation_engine.hpp"
#include "automation/bt_services.hpp"

// BehaviorTree.CPP forward declarations
namespace BT {
class Tree;
class BehaviorTreeFactory;
}  // namespace BT

namespace anolis {

// Forward declarations
namespace state {
class StateCache;
}
namespace control {
class CallRouter;
}
namespace provider {
class IProviderHandle;
class ProviderRegistry;
}  // namespace provider
namespace events {
class EventEmitter;
}  // namespace events

namespace automation {

class ModeManager;
class ParameterManager;

/**
 * Automation Health Status
 */
enum class BTStatus {
    BT_IDLE,     // No tree loaded or not running
    BT_RUNNING,  // Tree executing normally
    BT_STALLED,  // Tree returning FAILURE for multiple ticks
    BT_ERROR     // Critical error (e.g., exception during tick)
};

/**
 * Automation Health Information
 */
struct AutomationHealth {
    BTStatus bt_status = BTStatus::BT_IDLE;
    uint64_t last_tick_ms = 0;
    uint64_t ticks_since_progress = 0;
    uint64_t total_ticks = 0;
    std::string last_error;
    uint64_t error_count = 0;
    std::string current_tree;
};

/**
 * BT Runtime
 *
 * Manages Behavior Tree lifecycle for the Anolis automation layer.
 *
 * Architecture constraints:
 * - BT nodes read ONLY via StateCache (no direct provider access)
 * - BT nodes act ONLY via CallRouter (validated control path)
 * - Single-threaded tick loop in dedicated thread
 * - Tick rate configurable (default 10 Hz)
 *
 * The BT engine is a CONSUMER of kernel services (StateCache, CallRouter),
 * not a new subsystem layered beneath them.
 *
 * Implements the neutral `IAutomationEngine` seam (Phase 0); the BT-specific
 * surface (`tick`, `get_health`, `get_tree_path`) is retained for the HTTP layer
 * + tests until Phase 1 (#114) moves the wire onto the neutral `status()` view.
 */
class BTRuntime : public IAutomationEngine {
public:
    /**
     * Construct BT runtime with kernel service dependencies.
     *
     * @param state_cache Reference to state cache (for reading device state)
     * @param call_router Reference to call router (for device calls)
     * @param provider_registry Provider registry (for CallRouter::execute_call)
     * @param mode_manager Mode state machine (for AUTO/MANUAL gating)
     * @param parameter_manager Parameter manager (nullptr if not used)
     */
    BTRuntime(state::StateCache& state_cache, control::CallRouter& call_router,
              provider::ProviderRegistry& provider_registry, ModeManager& mode_manager,
              ParameterManager* parameter_manager = nullptr);

    ~BTRuntime();

    // Non-copyable, non-movable (manages thread)
    BTRuntime(const BTRuntime&) = delete;
    BTRuntime& operator=(const BTRuntime&) = delete;
    BTRuntime(BTRuntime&&) = delete;
    BTRuntime& operator=(BTRuntime&&) = delete;

    /**
     * Load behavior tree from XML file.
     *
     * @param path Path to BehaviorTree.CPP XML file
     * @return true if loaded successfully, false otherwise
     */
    bool load_tree(const std::string& path);

    // ---- IAutomationEngine seam (neutral; no BT types) ----
    std::string_view engine_kind() const override { return "behavior_tree"; }

    /// Atomic staged load: read bytes -> digest -> parse candidate -> build
    /// version -> swap under def_mutex_. A failed load preserves the previous
    /// active definition + version.
    LoadOutcome load(const AutomationDefinitionRef& ref) override;

    /// Start using the engine's configured tick rate (set via the BT-specific
    /// start(int) or the construction default).
    bool start(std::string& error) override;

    void stop() override;
    bool is_running() const override;

    /// One coherent neutral status snapshot (read under def_mutex_).
    AutomationStatusView status() const override;

    /// The exact loaded definition snapshot (not a disk re-read).
    std::optional<DefinitionArtifact> definition() const override;

    void set_event_sink(const std::shared_ptr<events::EventEmitter>& emitter) override;

    // ---- BT-specific surface (retained for tests + the not-yet-migrated HTTP
    //      layer; removed in Phase 1 #114 / Phase 4 #117) ----

    /**
     * Start BT tick loop in dedicated thread.
     *
     * @param tick_rate_hz Tick frequency in Hz (e.g., 10 = 100ms period)
     * @return true if started successfully, false if already running or not loaded
     */
    bool start(int tick_rate_hz = 10);

    /**
     * Execute a single BT tick (for testing or manual control).
     *
     * Note: Normally called by tick loop thread, but exposed for unit testing.
     *
     * @return BT::NodeStatus (SUCCESS, FAILURE, RUNNING)
     */
    BT::NodeStatus tick();

    /**
     * Get the path to the currently loaded BT file (returned by value — the
     * underlying field is guarded by def_mutex_).
     *
     * @return Path to loaded BT XML file, or empty string if not loaded
     */
    std::string get_tree_path() const;

    /**
     * Get current automation health status.
     *
     * @return AutomationHealth struct with current health metrics
     */
    AutomationHealth get_health() const;

    /**
     * @brief Set event emitter for fault notifications
     *
     * When set, BTRuntime emits an AutomationFaultEvent on a tick exception.
     * Must be called before start().
     *
     * @param emitter Shared pointer to EventEmitter (can be nullptr to disable)
     */
    void set_event_emitter(const std::shared_ptr<events::EventEmitter>& emitter);

    /**
     * @brief A decoupled sink for engine faults, called as `(locus, message,
     * occurred_at_epoch_ms)` from the tick thread on a tick exception.
     *
     * Keeps BTRuntime independent of the run subsystem: the runtime wires this to
     * the RunJournal's non-blocking enqueue so a fault is journaled durably off
     * the tick thread. The sink MUST NOT block or fsync on the calling thread.
     */
    using FaultSink =
        std::function<void(const std::string& locus, const std::string& message, int64_t occurred_at_epoch_ms)>;
    void set_fault_sink(FaultSink sink);

private:
    /**
     * Non-virtual stop implementation (so the destructor and the virtual stop()
     * override share one body without a virtual call during destruction).
     */
    void stop_impl();

    /**
     * Tick loop thread function.
     * Runs continuously at configured rate until stop() is called.
     */
    void tick_loop();

    /**
     * Populate BT blackboard with typed kernel service references.
     * Called before ticking to keep direct
     * tick() and threaded mode consistent.
     */
    void populate_blackboard(BT::Tree& tree);

    // Kernel service references (non-owning)
    state::StateCache& state_cache_;
    control::CallRouter& call_router_;
    provider::ProviderRegistry& provider_registry_;
    ModeManager& mode_manager_;
    ParameterManager* parameter_manager_;  // nullable

    // Active-definition state, guarded by def_mutex_. The mutex is taken only
    // around the swap (never during file read / XML parse) and around reads in
    // status()/definition()/get_health()/get_tree_path()/tick(). tree_ is a
    // shared_ptr so an in-flight tick keeps its tree alive across a swap.
    mutable std::mutex def_mutex_;
    std::unique_ptr<BT::BehaviorTreeFactory> factory_;
    std::shared_ptr<BT::Tree> tree_;
    std::string tree_path_;
    bool tree_loaded_ = false;
    std::string loaded_def_bytes_;  ///< Exact bytes that produced tree_ (truthful snapshot).
    AutomationVersion version_;     ///< Version of the active definition.

    // Threading
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> tick_thread_;
    int tick_rate_hz_ = 10;

    // Health tracking
    mutable std::mutex health_mutex_;
    uint64_t last_tick_ms_ = 0;
    BT::NodeStatus last_tick_status_ = BT::NodeStatus::IDLE;
    uint64_t ticks_since_progress_ = 0;
    uint64_t total_ticks_ = 0;
    std::string last_error_;
    uint64_t error_count_ = 0;

    // Event emitter (optional, for error notifications)
    std::shared_ptr<events::EventEmitter> event_emitter_;
    FaultSink fault_sink_;
    std::atomic<uint64_t> next_event_id_{1};
};

}  // namespace automation
}  // namespace anolis
