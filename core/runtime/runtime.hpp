#pragma once

/**
 * @file runtime.hpp
 * @brief Top-level runtime orchestrator for provider startup, polling, HTTP, and supervision.
 */

#include <atomic>
#include <memory>
#include <unordered_map>

#include "anolis_build_config.hpp"
#include "config.hpp"
#include "control/call_router.hpp"
#include "events/event_emitter.hpp"
#include "http/server.hpp"
#include "provider/i_provider_handle.hpp"  // Changed to interface
#include "provider/provider_registry.hpp"
#include "provider/provider_supervisor.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"
#include "telemetry/influx_sink.hpp"

namespace anolis {
namespace automation {
class ModeManager;
class ParameterManager;
#if ANOLIS_ENABLE_AUTOMATION
class BTRuntime;
#endif
}  // namespace automation
namespace runtime {

/**
 * @brief The top-level Anolis runtime kernel instance.
 *
 * Runtime owns the core service graph: provider registry, device registry,
 * state cache, call router, optional HTTP/automation/telemetry services, and
 * provider supervision.
 *
 * Lifecycle:
 * `initialize()` constructs services, starts providers, performs discovery,
 * validates shared-bus ownership, and primes the initial state snapshot.
 * `run()` then starts background activity and enters the main supervision loop
 * until `stop()` or an external shutdown signal requests exit.
 */
class Runtime {
public:
    /**
     * @brief Construct a runtime from an already-loaded configuration.
     */
    Runtime(const RuntimeConfig &config);

    /**
     * @brief Stop owned services on destruction.
     */
    ~Runtime();

    /**
     * @brief Initialize the runtime and all configured services.
     *
     * Error handling:
     * Returns false on startup timeout, provider launch failure, discovery
     * failure, ownership-validation failure, automation initialization failure,
     * or HTTP startup failure.
     */
    bool initialize(std::string &error);

    /**
     * @brief Enter the blocking main loop.
     *
     * Starts background polling, optionally starts automation ticking, and then
     * supervises provider health/restart policy until shutdown is requested.
     */
    void run();

    /** @brief Request that the main loop exit at its next shutdown check. */
    void stop() { running_ = false; }

    /**
     * @brief Stop owned runtime services and clear registered providers.
     *
     * Safe to call more than once.
     */
    void shutdown();

    // Access to core components (for HTTP and automation integration points)
    registry::DeviceRegistry &get_registry() { return *registry_; }
    state::StateCache &get_state_cache() { return *state_cache_; }
    control::CallRouter &get_call_router() { return *call_router_; }
    events::EventEmitter &get_event_emitter() { return *event_emitter_; }

    provider::ProviderRegistry &get_provider_registry() { return provider_registry_; }

private:
    // Staged initialization helpers
    bool init_providers(std::string &error);
    bool init_core_services(std::string &error);
    bool init_automation(std::string &error);
    bool init_http(std::string &error);
    bool init_telemetry(std::string &error);

    /**
     * @brief Restart one provider and atomically publish its replacement inventory.
     *
     * The existing registry entry is only replaced after the new provider
     * process starts, discovery succeeds, ownership validation passes, and the
     * state cache poll plan is rebuilt.
     */
    bool restart_provider(const std::string &provider_id, const provider::ProviderConfig &provider_config);

    RuntimeConfig config_;

    provider::ProviderRegistry provider_registry_;
    std::unique_ptr<registry::DeviceRegistry> registry_;
    std::shared_ptr<events::EventEmitter> event_emitter_;  // Shared with StateCache + HTTP
    std::unique_ptr<state::StateCache> state_cache_;
    std::unique_ptr<control::CallRouter> call_router_;
    std::unique_ptr<http::HttpServer> http_server_;
    std::unique_ptr<telemetry::InfluxSink> telemetry_sink_;
#if ANOLIS_ENABLE_AUTOMATION
    std::unique_ptr<automation::ModeManager> mode_manager_;
    std::unique_ptr<automation::ParameterManager> parameter_manager_;
    std::unique_ptr<automation::BTRuntime> bt_runtime_;
#endif
    std::unique_ptr<provider::ProviderSupervisor> supervisor_;

    std::atomic<bool> running_{false};
};

}  // namespace runtime
}  // namespace anolis
