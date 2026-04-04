#include "runtime.hpp"

#include <chrono>
#include <thread>
#include <utility>

#if ANOLIS_ENABLE_AUTOMATION
#include "automation/bt_runtime.hpp"
#include "automation/mode_manager.hpp"
#include "automation/parameter_manager.hpp"
#include "automation/parameter_types.hpp"
#endif
#include "logging/logger.hpp"
#include "ownership_validation.hpp"
#include "provider/provider_handle.hpp"  // Required for instantiation
#include "provider/provider_supervisor.hpp"
#include "signal_handler.hpp"

namespace anolis {
namespace runtime {

Runtime::Runtime(const RuntimeConfig &config) : config_(config) {}

Runtime::~Runtime() { shutdown(); }

bool Runtime::initialize(std::string &error) {
    LOG_INFO("[Runtime] Initializing Anolis Core");
    const auto startup_begin = std::chrono::steady_clock::now();
    const int startup_timeout_ms = config_.runtime.startup_timeout_ms;

    auto check_startup_deadline = [&](const char *stage) -> bool {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - startup_begin).count();
        if (elapsed_ms < startup_timeout_ms) {
            return true;
        }

        error = "Runtime startup timeout exceeded after '" + std::string(stage) + "' (" + std::to_string(elapsed_ms) +
                "ms >= " + std::to_string(startup_timeout_ms) + "ms)";
        return false;
    };

    if (!init_core_services(error)) {
        return false;
    }
    if (!check_startup_deadline("init_core_services")) {
        return false;
    }

    if (!init_providers(error)) {
        return false;
    }
    if (!check_startup_deadline("init_providers")) {
        return false;
    }

    if (!state_cache_->initialize()) {
        error = "State cache initialization failed: " + state_cache_->last_error();
        return false;
    }
    if (!check_startup_deadline("state_cache.initialize")) {
        return false;
    }

    // Prime state cache once so initial HTTP calls observe a full snapshot
    state_cache_->poll_once(provider_registry_);
    if (!check_startup_deadline("state_cache.poll_once")) {
        return false;
    }

    if (!init_automation(error)) {
        return false;
    }
    if (!check_startup_deadline("init_automation")) {
        return false;
    }

    if (!init_http(error)) {
        return false;
    }
    if (!check_startup_deadline("init_http")) {
        return false;
    }

    if (!init_telemetry(error)) {
        return false;
    }
    if (!check_startup_deadline("init_telemetry")) {
        return false;
    }

    LOG_INFO("[Runtime] Initialization complete");
    return true;
}

bool Runtime::init_core_services(std::string & /*error*/) {
    // Create registry
    registry_ = std::make_unique<registry::DeviceRegistry>();

    // Create event emitter
    // Default: 100 events per subscriber queue, max 32 SSE clients
    event_emitter_ = std::make_shared<events::EventEmitter>(100, 32);
    LOG_INFO("[Runtime] Event emitter created (max " << event_emitter_->max_subscribers() << " subscribers)");

    // Create state cache
    state_cache_ = std::make_unique<state::StateCache>(*registry_, config_.polling.interval_ms);

    // Wire event emitter to state cache
    state_cache_->set_event_emitter(event_emitter_);

    // Create call router
    call_router_ = std::make_unique<control::CallRouter>(*registry_, *state_cache_);

    // Create provider supervisor
    supervisor_ = std::make_unique<provider::ProviderSupervisor>();
    LOG_INFO("[Runtime] Provider supervisor created");

    return true;
}

bool Runtime::init_providers(std::string &error) {
    // Start all providers and discover
    for (const auto &provider_config : config_.providers) {
        LOG_INFO("[Runtime] Starting provider: " << provider_config.id);
        LOG_DEBUG("[Runtime]   Command: " << provider_config.command);

        auto provider = std::make_shared<provider::ProviderHandle>(
            provider_config.id, provider_config.command, provider_config.args, provider_config.timeout_ms,
            provider_config.hello_timeout_ms, provider_config.ready_timeout_ms, config_.runtime.shutdown_timeout_ms);

        if (!provider->start()) {
            error = "Failed to start provider '" + provider_config.id + "': " + provider->last_error();
            return false;
        }

        LOG_INFO("[Runtime] Provider " << provider_config.id << " started");

        // Register provider with supervisor
        supervisor_->register_provider(provider_config.id, provider_config.restart_policy);

        // Discover devices
        if (!registry_->discover_provider(provider_config.id, *provider)) {
            error = "Discovery failed for provider '" + provider_config.id + "': " + registry_->last_error();
            return false;
        }

        provider_registry_.add_provider(provider_config.id, provider);
    }

    std::string ownership_error;
    if (!validate_i2c_ownership_claims(registry_->get_all_devices(), ownership_error)) {
        LOG_ERROR("[Runtime] " << ownership_error);
        error = ownership_error;
        return false;
    }

    LOG_INFO("[Runtime] Ownership validation passed for discovered I2C devices");
    LOG_INFO("[Runtime] All providers started");
    return true;
}

bool Runtime::init_automation(std::string &error) {
#if ANOLIS_ENABLE_AUTOMATION
    // Create ModeManager and wire to CallRouter if automation enabled
    if (config_.automation.enabled) {
        mode_manager_ = std::make_unique<automation::ModeManager>(automation::RuntimeMode::IDLE);

        std::string policy_str =
            (config_.automation.manual_gating_policy == GatingPolicy::BLOCK) ? "BLOCK" : "OVERRIDE";
        call_router_->set_mode_manager(mode_manager_.get(), policy_str);
    }

    // Create and initialize ParameterManager BEFORE HTTP server
    if (config_.automation.enabled) {
        LOG_INFO("[Runtime] Creating parameter manager");
        parameter_manager_ = std::make_unique<automation::ParameterManager>();

        // Load parameters from config
        for (const auto &param_config : config_.automation.parameters) {
            std::optional<std::vector<std::string>> allowed =
                param_config.allowed_values.empty() ? std::nullopt : std::make_optional(param_config.allowed_values);

            if (!parameter_manager_->define(param_config.name, param_config.type, param_config.default_value,
                                            param_config.min, param_config.max, allowed)) {
                LOG_WARN("[Runtime] Failed to define parameter: " << param_config.name);
            }
        }

        LOG_INFO("[Runtime] Parameter manager initialized with " << parameter_manager_->parameter_count()
                                                                 << " parameters");
    }

    // Register mode change callback to emit telemetry events (only if automation enabled)
    if (mode_manager_) {
        mode_manager_->on_mode_change([this](automation::RuntimeMode prev, automation::RuntimeMode next) {
            if (event_emitter_) {
                events::ModeChangeEvent event;
                event.event_id = event_emitter_->next_event_id();
                event.previous_mode = automation::mode_to_string(prev);
                event.new_mode = automation::mode_to_string(next);
                event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();

                event_emitter_->emit(event);
                LOG_INFO("[Runtime] Mode change event emitted: " << event.previous_mode << " -> " << event.new_mode);
            }
        });
    }

    // Register callback to emit parameter change events (if parameter manager exists)
    if (parameter_manager_) {
        parameter_manager_->on_parameter_change([this](const std::string &name,
                                                       const automation::ParameterValue &old_value,
                                                       const automation::ParameterValue &new_value) {
            if (event_emitter_) {
                events::ParameterChangeEvent event;
                event.event_id = event_emitter_->next_event_id();
                event.parameter_name = name;
                event.old_value_str = automation::parameter_value_to_string(old_value);
                event.new_value_str = automation::parameter_value_to_string(new_value);
                event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();

                event_emitter_->emit(event);
                LOG_INFO("[Runtime] Parameter '" << name << "' changed: " << event.old_value_str << " -> "
                                                 << event.new_value_str);
            }
        });
    }

    // Create and initialize BTRuntime if enabled
    if (config_.automation.enabled) {
        LOG_INFO("[Runtime] Creating BT runtime");
        bt_runtime_ =
            std::make_unique<automation::BTRuntime>(*state_cache_, *call_router_, provider_registry_, *mode_manager_,
                                                    parameter_manager_.get()  // Pass parameter manager
            );

        // Set event emitter for error notifications
        if (event_emitter_) {
            bt_runtime_->set_event_emitter(event_emitter_);
        }

        if (!bt_runtime_->load_tree(config_.automation.behavior_tree)) {
            error = "Failed to load behavior tree: " + config_.automation.behavior_tree;
            return false;
        }

        LOG_INFO("[Runtime] Behavior tree loaded: " << config_.automation.behavior_tree);
    } else {
        LOG_INFO("[Runtime] Automation disabled in config");
    }

    return true;
#else
    (void)error;
    if (config_.automation.enabled) {
        LOG_WARN("[Runtime] Automation configured but disabled at build time (ANOLIS_ENABLE_AUTOMATION=OFF)");
    } else {
        LOG_INFO("[Runtime] Automation disabled in config");
    }
    return true;
#endif
}

bool Runtime::init_http(std::string &error) {
    // Create and start HTTP server if enabled
    if (config_.http.enabled) {
        LOG_INFO("[Runtime] Creating HTTP server");
        http::HttpServerDependencies dependencies(*registry_, *state_cache_, *call_router_, provider_registry_);
        dependencies.supervisor = supervisor_.get();  // optional: supervision snapshots
        dependencies.event_emitter = event_emitter_;  // optional: SSE emitter
#if ANOLIS_ENABLE_AUTOMATION
        dependencies.mode_manager = mode_manager_.get();            // optional: automation mode
        dependencies.parameter_manager = parameter_manager_.get();  // optional: runtime parameters
        dependencies.bt_runtime = bt_runtime_.get();                // optional: automation runtime
#endif
        http_server_ =
            std::make_unique<http::HttpServer>(config_.http, config_.polling.interval_ms, std::move(dependencies));

        std::string http_error;
        if (!http_server_->start(http_error)) {
            error = "HTTP server failed to start: " + http_error;
            return false;
        }
        LOG_INFO("[Runtime] HTTP server started on " << config_.http.bind << ":" << config_.http.port);
    } else {
        LOG_INFO("[Runtime] HTTP server disabled in config");
    }
    return true;
}

bool Runtime::init_telemetry(std::string & /*error*/) {
    // Start telemetry sink if enabled
    if (config_.telemetry.enabled) {
        LOG_INFO("[Runtime] Creating telemetry sink");

        telemetry::InfluxConfig influx_config;
        influx_config.enabled = true;
        influx_config.url = config_.telemetry.influx_url;
        influx_config.org = config_.telemetry.influx_org;
        influx_config.bucket = config_.telemetry.influx_bucket;
        influx_config.token = config_.telemetry.influx_token;
        influx_config.batch_size = config_.telemetry.batch_size;
        influx_config.flush_interval_ms = config_.telemetry.flush_interval_ms;
        influx_config.queue_size = config_.telemetry.queue_size;
        influx_config.max_retry_buffer_size = config_.telemetry.max_retry_buffer_size;

        telemetry_sink_ = std::make_unique<telemetry::InfluxSink>(influx_config);

        if (!telemetry_sink_->start(event_emitter_)) {
            LOG_WARN("[Runtime] Telemetry sink failed to start");
            // Don't fail runtime initialization - telemetry is optional
        } else {
            LOG_INFO("[Runtime] Telemetry sink started");
        }
    } else {
        LOG_INFO("[Runtime] Telemetry disabled in config");
    }
    return true;
}

void Runtime::run() {
    LOG_INFO("[Runtime] Starting main loop");
    running_ = true;

    // start_polling() requires initialize() to have run first.
    // initialize() already primes one synchronous poll_once() snapshot.
    state_cache_->start_polling(provider_registry_);

    LOG_INFO("[Runtime] State cache polling active");

    // Start BT tick loop if automation enabled
#if ANOLIS_ENABLE_AUTOMATION
    if (bt_runtime_) {
        if (!bt_runtime_->start(config_.automation.tick_rate_hz)) {
            LOG_WARN("[Runtime] BT runtime failed to start");
        } else {
            LOG_INFO("[Runtime] BT runtime started (tick rate: " << config_.automation.tick_rate_hz << " Hz)");
        }
    }
#endif

    LOG_INFO("[Runtime] Press Ctrl+C to exit");

    // Main loop: polling, provider health monitoring, crash recovery
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check for shutdown signal
        if (anolis::runtime::SignalHandler::is_shutdown_requested()) {
            LOG_INFO("[Runtime] Signal received, stopping...");
            running_ = false;
            break;
        }

        // Check provider health and handle restarts
        for (const auto &provider_config : config_.providers) {
            const std::string &id = provider_config.id;
            auto provider = provider_registry_.get_provider(id);

            if (!provider) {
                continue;  // Provider not initialized (shouldn't happen)
            }

            // Check if provider is available
            if (!provider->is_available()) {
                // Provider crashed or unavailable

                // Check if circuit breaker is open
                if (supervisor_->is_circuit_open(id)) {
                    continue;  // Circuit open, no more attempts
                }

                // Mark that we've detected this crash (only records once per crash)
                if (supervisor_->mark_crash_detected(id)) {
                    // This is a new crash - record it and schedule restart
                    if (!supervisor_->record_crash(id)) {
                        // Max attempts exceeded - circuit breaker opened
                        continue;
                    }
                }

                // Check if we should attempt restart (backoff period elapsed)
                if (supervisor_->should_restart(id)) {
                    // Clear crash detected flag before restart attempt
                    // (so next crash will be properly detected)
                    supervisor_->clear_crash_detected(id);

                    // Attempt restart
                    LOG_INFO("[Runtime] Attempting to restart provider: " << id);
                    if (restart_provider(id, provider_config)) {
                        LOG_INFO("[Runtime] Provider restarted successfully: " << id);
                    } else {
                        LOG_ERROR("[Runtime] Failed to restart provider: " << id);
                        // Don't mark crash detected - will retry on next iteration
                    }
                }
            } else {
                // Provider is healthy: record heartbeat for timing/observability.
                supervisor_->record_heartbeat(id);
                // Reset attempts only after the provider has remained healthy for the
                // configured stability window (avoids clearing crash streak too early).
                if (supervisor_->should_mark_recovered(id)) {
                    supervisor_->record_success(id);
                }
            }
        }
    }

    LOG_INFO("[Runtime] Shutting down");
    state_cache_->stop_polling();
}

void Runtime::shutdown() {
    // Stop BT runtime first
#if ANOLIS_ENABLE_AUTOMATION
    if (bt_runtime_) {
        LOG_INFO("[Runtime] Stopping BT runtime");
        bt_runtime_->stop();
    }
#endif

    // Stop HTTP server
    if (http_server_) {
        LOG_INFO("[Runtime] Stopping HTTP server");
        http_server_->stop();
    }

    // Stop telemetry sink
    if (telemetry_sink_) {
        LOG_INFO("[Runtime] Stopping telemetry sink");
        telemetry_sink_->stop();
    }

    if (state_cache_) {
        state_cache_->stop_polling();
    }

    // Provider cleanup handled by ProviderRegistry destructor
    provider_registry_.clear();
}

bool Runtime::restart_provider(const std::string &provider_id, const provider::ProviderConfig &provider_config) {
    // Start timeout clock (timeout starts after backoff completes, when restart attempt begins)
    auto restart_start_time = std::chrono::steady_clock::now();
    int timeout_ms = provider_config.restart_policy.timeout_ms;

    LOG_INFO("[Runtime] Restarting provider: " << provider_id);
    LOG_DEBUG("[Runtime]   Command: " << provider_config.command);

    // Create new provider instance and fully validate it before swapping the
    // registry entry. If any step fails, leave the existing (unavailable)
    // provider entry in place so supervision can continue retrying.
    auto provider = std::make_shared<provider::ProviderHandle>(
        provider_id, provider_config.command, provider_config.args, provider_config.timeout_ms,
        provider_config.hello_timeout_ms, provider_config.ready_timeout_ms, config_.runtime.shutdown_timeout_ms);

    if (!provider->start()) {
        LOG_ERROR("[Runtime] Failed to start provider '" << provider_id << "': " << provider->last_error());
        return false;
    }

    // Check timeout after start()
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restart_start_time)
            .count();
    if (elapsed_ms >= timeout_ms) {
        LOG_ERROR("[Runtime] Restart timeout exceeded for provider '" << provider_id << "' after start() ("
                                                                      << elapsed_ms << "ms >= " << timeout_ms << "ms)");
        return false;
    }

    LOG_INFO("[Runtime] Provider " << provider_id << " process started");

    std::vector<registry::RegisteredDevice> replacement_devices;
    if (!registry_->inspect_provider_devices(provider_id, *provider, replacement_devices)) {
        LOG_ERROR("[Runtime] Discovery failed for provider '" << provider_id << "': " << registry_->last_error());
        return false;
    }

    // Check timeout after device inspection.
    elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restart_start_time)
            .count();
    if (elapsed_ms >= timeout_ms) {
        LOG_ERROR("[Runtime] Restart timeout exceeded for provider '" << provider_id << "' after discovery ("
                                                                      << elapsed_ms << "ms >= " << timeout_ms << "ms)");
        return false;
    }

    std::string ownership_error;
    if (!validate_i2c_ownership_claims_after_provider_replacement(registry_->get_all_devices(), provider_id,
                                                                  replacement_devices, ownership_error)) {
        LOG_ERROR("[Runtime] " << ownership_error);
        return false;
    }

    registry_->commit_provider_devices(provider_id, std::move(replacement_devices), true);

    // Rebuild poll configs for this provider (Sprint 1.3: reconcile changed capabilities)
    state_cache_->rebuild_poll_configs(provider_id);

    // Swap registry entry only after replacement provider startup + discovery succeeds.
    provider_registry_.add_provider(provider_id, provider);

    LOG_INFO("[Runtime] Provider " << provider_id << " restarted and devices rediscovered");
    return true;
}

}  // namespace runtime
}  // namespace anolis
