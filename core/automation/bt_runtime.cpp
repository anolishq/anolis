#include "automation/bt_runtime.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>

#include <chrono>
#include <fstream>
#include <thread>

#include "automation/bt_nodes.hpp"
#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "events/event_emitter.hpp"
#include "logging/logger.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace automation {

BTRuntime::BTRuntime(state::StateCache &state_cache, control::CallRouter &call_router,
                     provider::ProviderRegistry &provider_registry, ModeManager &mode_manager,
                     ParameterManager *parameter_manager)
    : state_cache_(state_cache),
      call_router_(call_router),
      provider_registry_(provider_registry),
      mode_manager_(mode_manager),
      parameter_manager_(parameter_manager),
      factory_(std::make_unique<BT::BehaviorTreeFactory>()) {
    LOG_INFO("[BTRuntime] Initialized");

    // Register custom nodes
    factory_->registerNodeType<ReadSignalNode>("ReadSignal");
    factory_->registerNodeType<CallDeviceNode>("CallDevice");
    factory_->registerNodeType<CheckQualityNode>("CheckQuality");
    factory_->registerNodeType<GetParameterNode>("GetParameter");
    factory_->registerNodeType<GetParameterBoolNode>("GetParameterBool");
    factory_->registerNodeType<GetParameterInt64Node>("GetParameterInt64");
    factory_->registerNodeType<CheckBoolNode>("CheckBool");
    factory_->registerNodeType<PeriodicPulseWindowNode>("PeriodicPulseWindow");
    factory_->registerNodeType<EmitOnChangeOrIntervalNode>("EmitOnChangeOrInterval");
    factory_->registerNodeType<BuildArgsJsonNode>("BuildArgsJson");

    LOG_INFO("[BTRuntime] Registered custom node types");
}

BTRuntime::~BTRuntime() { stop(); }

void BTRuntime::set_event_emitter(const std::shared_ptr<events::EventEmitter> &emitter) { event_emitter_ = emitter; }

bool BTRuntime::load_tree(const std::string &path) {
    // Verify file exists
    std::ifstream file(path);
    if (!file.good()) {
        LOG_ERROR("[BTRuntime] Cannot open BT file: " << path);
        return false;
    }

    tree_path_ = path;

    try {
        tree_ = std::make_unique<BT::Tree>(factory_->createTreeFromFile(path));
        populate_blackboard();
        tree_loaded_ = true;

        LOG_INFO("[BTRuntime] BT loaded successfully: " << path);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR("[BTRuntime] Error loading BT: " << e.what());
        tree_loaded_ = false;
        return false;
    }
}

bool BTRuntime::start(int tick_rate_hz) {
    if (running_) {
        LOG_ERROR("[BTRuntime] Already running");
        return false;
    }

    if (!tree_loaded_) {
        LOG_ERROR("[BTRuntime] No BT loaded, call load_tree() first");
        return false;
    }

    if (tick_rate_hz <= 0 || tick_rate_hz > 1000) {
        LOG_ERROR("[BTRuntime] Invalid tick rate: " << tick_rate_hz << " (must be 1-1000 Hz)");
        return false;
    }

    tick_rate_hz_ = tick_rate_hz;
    running_ = true;

    tick_thread_ = std::make_unique<std::thread>(&BTRuntime::tick_loop, this);

    LOG_INFO("[BTRuntime] Started tick loop at " << tick_rate_hz << " Hz");
    return true;
}

void BTRuntime::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("[BTRuntime] Stopping tick loop...");
    running_ = false;

    if (tick_thread_ && tick_thread_->joinable()) {
        tick_thread_->join();
    }
    tick_thread_.reset();

    LOG_INFO("[BTRuntime] Tick loop stopped");
}

bool BTRuntime::is_running() const { return running_; }

BT::NodeStatus BTRuntime::tick() {
    if (!tree_) {
        LOG_ERROR("[BTRuntime] Cannot tick, no tree loaded");
        return BT::NodeStatus::FAILURE;
    }

    populate_blackboard();
    return tree_->tickOnce();
}

void BTRuntime::tick_loop() {
    using namespace std::chrono;

    const auto tick_period = milliseconds(1000 / tick_rate_hz_);
    auto next_tick = steady_clock::now() + tick_period;

    LOG_INFO("[BTRuntime] Tick loop started (period: " << tick_period.count() << "ms)");

    while (running_) {
        // Check if we're in AUTO mode
        if (mode_manager_.current_mode() != RuntimeMode::AUTO) {
            // Not in AUTO mode, skip tick
            std::this_thread::sleep_until(next_tick);
            next_tick += tick_period;
            continue;
        }

        // Execute single BT tick
        BT::NodeStatus status = BT::NodeStatus::IDLE;
        try {
            status = tick();

            // Update health tracking
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                last_tick_ms_ = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                last_tick_status_ = status;
                total_ticks_++;

                // Track progress: SUCCESS or RUNNING = making progress
                // FAILURE for multiple ticks = stalled
                if (status == BT::NodeStatus::SUCCESS || status == BT::NodeStatus::RUNNING) {
                    ticks_since_progress_ = 0;
                } else if (status == BT::NodeStatus::FAILURE) {
                    ticks_since_progress_++;

                    // Emit bt_error event on first failure in a sequence
                    if (ticks_since_progress_ == 1 && event_emitter_) {
                        events::BTErrorEvent error_event{next_event_id_.fetch_add(1),
                                                         "",  // Node name not available without deep BT introspection
                                                         "BT returned FAILURE", static_cast<int64_t>(last_tick_ms_)};
                        event_emitter_->emit(error_event);
                    }
                }
            }

            // Log terminal states (optional, can be verbose)
            if (status == BT::NodeStatus::SUCCESS) {
                LOG_INFO("[BTRuntime] BT completed successfully");
            } else if (status == BT::NodeStatus::FAILURE) {
                LOG_WARN("[BTRuntime] BT failed");
            }
            // RUNNING status is normal, don't log
        } catch (const std::exception &e) {
            LOG_ERROR("[BTRuntime] Error during tick: " << e.what());

            // Update error tracking
            std::lock_guard<std::mutex> lock(health_mutex_);
            last_error_ = e.what();
            error_count_++;
            last_tick_status_ = BT::NodeStatus::FAILURE;

            // Emit bt_error event
            if (event_emitter_) {
                events::BTErrorEvent error_event{
                    next_event_id_.fetch_add(1),
                    "",  // Node name not available from exception
                    e.what(), duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()};
                event_emitter_->emit(error_event);
            }
        }

        // Sleep until next tick
        std::this_thread::sleep_until(next_tick);
        next_tick += tick_period;
    }

    LOG_INFO("[BTRuntime] Tick loop exiting");
}
void BTRuntime::populate_blackboard() {
    if (!tree_) {
        return;
    }

    auto blackboard = tree_->rootBlackboard();
    if (!blackboard) {
        LOG_ERROR("[BTRuntime] Cannot populate blackboard, root blackboard is null");
        return;
    }

    BTServiceContext services;
    services.state_cache = &state_cache_;
    services.call_router = &call_router_;
    services.provider_registry = &provider_registry_;
    services.parameter_manager = parameter_manager_;
    blackboard->set(kBTServiceContextKey, services);
}

AutomationHealth BTRuntime::get_health() const {
    std::lock_guard<std::mutex> lock(health_mutex_);

    AutomationHealth health;
    health.last_tick_ms = last_tick_ms_;
    health.ticks_since_progress = ticks_since_progress_;
    health.total_ticks = total_ticks_;
    health.last_error = last_error_;
    health.error_count = error_count_;
    health.current_tree = tree_path_;

    // Determine BT status
    if (!tree_loaded_ || !running_) {
        health.bt_status = BTStatus::BT_IDLE;
    } else if (error_count_ > 0 && last_tick_status_ == BT::NodeStatus::FAILURE) {
        health.bt_status = BTStatus::BT_ERROR;
    } else if (ticks_since_progress_ > 10) {
        // If FAILURE for more than 10 ticks (1 second at 10 Hz), consider stalled
        health.bt_status = BTStatus::BT_STALLED;
    } else {
        health.bt_status = BTStatus::BT_RUNNING;
    }

    return health;
}

}  // namespace automation
}  // namespace anolis
