#include "automation/bt_runtime.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <openssl/evp.h>

#include <chrono>
#include <fstream>
#include <iterator>
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

namespace {

std::string sha256_hex(const std::string &data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
        return "";
    }
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(md_len) * 2);
    for (unsigned int i = 0; i < md_len; ++i) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0x0F]);
    }
    return out;
}

std::string file_basename(const std::string &path) {
    auto slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

}  // namespace

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

BTRuntime::~BTRuntime() { stop_impl(); }

void BTRuntime::set_event_sink(const std::shared_ptr<events::EventEmitter> &emitter) { event_emitter_ = emitter; }

void BTRuntime::set_event_emitter(const std::shared_ptr<events::EventEmitter> &emitter) { set_event_sink(emitter); }

void BTRuntime::set_fault_sink(FaultSink sink) { fault_sink_ = std::move(sink); }

LoadOutcome BTRuntime::load(const AutomationDefinitionRef &ref) {
    LoadOutcome outcome;
    const std::string &path = ref.source_path;

    // --- Slow work, done WITHOUT the lock so a swap never holds it during I/O. ---
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        outcome.error = "cannot open automation definition: " + path;
        LOG_ERROR("[BTRuntime] " << outcome.error);
        return outcome;  // previous active definition preserved
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::shared_ptr<BT::Tree> candidate;
    try {
        // Parse from the file (so <include> subtrees resolve); digest_scope stays
        // "top_level_file" because only the top-level bytes are hashed/snapshotted.
        candidate = std::make_shared<BT::Tree>(factory_->createTreeFromFile(path));
    } catch (const std::exception &e) {
        outcome.error = std::string("failed to parse automation definition: ") + e.what();
        LOG_ERROR("[BTRuntime] " << outcome.error);
        return outcome;  // previous active definition preserved
    }

    AutomationVersion version;
    version.engine_kind = "behavior_tree";
    version.id = file_basename(path);
    version.digest = sha256_hex(bytes);
    version.digest_scope = "top_level_file";

    // --- Atomic swap: take the lock only to publish the new active definition. ---
    {
        std::lock_guard<std::mutex> lock(def_mutex_);
        tree_ = std::move(candidate);
        tree_path_ = path;
        loaded_def_bytes_ = std::move(bytes);
        version_ = version;
        tree_loaded_ = true;
        populate_blackboard(*tree_);
    }

    outcome.ok = true;
    outcome.version = version;
    LOG_INFO("[BTRuntime] Loaded automation definition: " << path << " (digest " << version.digest.substr(0, 12)
                                                          << "…)");
    return outcome;
}

bool BTRuntime::load_tree(const std::string &path) { return load(AutomationDefinitionRef{path}).ok; }

bool BTRuntime::start(int tick_rate_hz) {
    if (running_) {
        LOG_ERROR("[BTRuntime] Already running");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(def_mutex_);
        if (!tree_loaded_) {
            LOG_ERROR("[BTRuntime] No BT loaded, call load() first");
            return false;
        }
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

bool BTRuntime::start(std::string &error) {
    if (!start(tick_rate_hz_)) {
        error = "automation engine failed to start (already running, no definition loaded, or invalid tick rate)";
        return false;
    }
    return true;
}

void BTRuntime::stop() { stop_impl(); }

void BTRuntime::stop_impl() {
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
    // Grab the active tree under the lock so a concurrent load() swap can't free
    // it mid-tick (the shared_ptr keeps our copy alive). Parsing/swap happen
    // elsewhere; here we only copy the handle.
    std::shared_ptr<BT::Tree> tree;
    {
        std::lock_guard<std::mutex> lock(def_mutex_);
        tree = tree_;
    }

    if (!tree) {
        LOG_ERROR("[BTRuntime] Cannot tick, no tree loaded");
        return BT::NodeStatus::FAILURE;
    }

    populate_blackboard(*tree);
    return tree->tickOnce();
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
                    // An ordinary BT FAILURE is NOT an automation fault — it is
                    // unsuccessful execution and surfaces through execution_status
                    // (the >N-tick stall heuristic -> blocked/failed), never as a
                    // fault event.
                    ticks_since_progress_++;
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

            const int64_t fault_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            {
                // Update error tracking
                std::lock_guard<std::mutex> lock(health_mutex_);
                last_error_ = e.what();
                error_count_++;
                last_tick_status_ = BT::NodeStatus::FAILURE;
            }

            // A tick exception IS an engine fault. Emit the canonical neutral
            // fault event (the SSE serializer renders the `automation_fault` frame),
            // and notify the decoupled fault sink so it can be journaled durably
            // off this tick thread (the sink never blocks/fsyncs here).
            if (event_emitter_) {
                events::AutomationFaultEvent fault_event{next_event_id_.fetch_add(1), "tick", e.what(), fault_ms};
                event_emitter_->emit(fault_event);
            }
            if (fault_sink_) {
                fault_sink_("tick", e.what(), fault_ms);
            }

            // Halt autonomous actuation (#279). Emitting an event is observation,
            // not intervention: without this the loop simply ticks again, the
            // runtime stays in AUTO, and outputs stay at whatever the tree last
            // commanded. Nothing else catches it -- the firmware command watchdog
            // is a BUS-liveness watchdog, and the state poller (a separate thread,
            // unaffected by this exception) keeps it fed, so it never trips.
            //
            // Any -> FAULT is always a valid transition and cannot be vetoed, so
            // this both stops further ticking (the AUTO gate at the top of the
            // loop) and fires the declared *->FAULT hooks that drive outputs to
            // their safe state. Called outside health_mutex_, and set_mode
            // releases its own lock before dispatching callbacks.
            const auto mode_before = mode_manager_.current_mode();
            std::string mode_error;
            const bool set_ok = mode_manager_.set_mode(RuntimeMode::FAULT, mode_error);
            const auto mode_after = mode_manager_.current_mode();

            // What matters for safety is only whether we are out of AUTO, so
            // report on that rather than on the call's return value. Any -> FAULT
            // is always valid and a before-hook can neither veto it nor abort it
            // by throwing, so a false return means a concurrent transition
            // already moved us -- in which case the gate at the top of this loop
            // has stopped ticking and actuation is halted regardless.
            if (mode_after == RuntimeMode::AUTO) {
                LOG_ERROR(
                    "[BTRuntime] Still in AUTO after a tick exception; autonomous actuation may still be "
                    "running: "
                    << mode_error);
            } else if (mode_before == RuntimeMode::FAULT) {
                // set_mode() no-ops when already in the target mode, so no hooks
                // ran here and this tick changed nothing.
                LOG_WARN("[BTRuntime] Tick exception while already in FAULT; autonomous actuation already halted.");
            } else {
                LOG_ERROR("[BTRuntime] Halted autonomous actuation after a tick exception; mode is now "
                          << mode_to_string(mode_after) << (set_ok ? "." : " (raced a concurrent transition)."));
            }
        }

        // Sleep until next tick
        std::this_thread::sleep_until(next_tick);
        next_tick += tick_period;
    }

    LOG_INFO("[BTRuntime] Tick loop exiting");
}

void BTRuntime::populate_blackboard(BT::Tree &tree) {
    auto blackboard = tree.rootBlackboard();
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

std::string BTRuntime::get_tree_path() const {
    std::lock_guard<std::mutex> lock(def_mutex_);
    return tree_path_;
}

AutomationHealth BTRuntime::get_health() const {
    // Lock order: def_mutex_ then health_mutex_ (consistent everywhere both held).
    std::lock_guard<std::mutex> def_lock(def_mutex_);
    std::lock_guard<std::mutex> health_lock(health_mutex_);

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

AutomationStatusView BTRuntime::status() const {
    // One coherent snapshot. Lock order: def_mutex_ then health_mutex_.
    std::lock_guard<std::mutex> def_lock(def_mutex_);
    std::lock_guard<std::mutex> health_lock(health_mutex_);

    AutomationStatusView view;
    view.version = version_;
    view.last_evaluation_at_epoch_ms = last_tick_ms_;
    view.last_error = last_error_;

    // Neutral mapping of the BT status (the failure-vs-fault refinement that
    // splits an exception from an ordinary FAILURE lands in Phase 1 #114).
    if (!tree_loaded_ || !running_) {
        view.status = AutomationStatus::Idle;
    } else if (error_count_ > 0 && last_tick_status_ == BT::NodeStatus::FAILURE) {
        view.status = AutomationStatus::Failed;
    } else if (ticks_since_progress_ > 10) {
        view.status = AutomationStatus::Blocked;
    } else {
        view.status = AutomationStatus::Running;
    }

    // engine_diagnostics is UNSTABLE / non-contractual.
    view.engine_diagnostics = {{"ticks_since_progress", ticks_since_progress_},
                               {"total_ticks", total_ticks_},
                               {"error_count", error_count_},
                               {"stall_suspected", ticks_since_progress_ > 10}};
    return view;
}

std::optional<DefinitionArtifact> BTRuntime::definition() const {
    std::lock_guard<std::mutex> lock(def_mutex_);
    if (!tree_loaded_) {
        return std::nullopt;
    }
    DefinitionArtifact artifact;
    artifact.media_type = "application/xml";
    artifact.digest = version_.digest;
    artifact.bytes = loaded_def_bytes_;
    return artifact;
}

}  // namespace automation
}  // namespace anolis
