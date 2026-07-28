#pragma once

// Prevent Windows macro pollution (must be before httplib.h)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include <httplib.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "anolis_build_config.hpp"
#include "events/event_types.hpp"
#include "provider/i_provider_handle.hpp"  // Updated include
#include "provider/provider_registry.hpp"
#include "runtime/config.hpp"

// Forward declarations
namespace anolis {
namespace runtime {
class Runtime;
}
namespace registry {
class DeviceRegistry;
}
namespace state {
class StateCache;
}
namespace control {
class CallRouter;
class SafeStateController;
}  // namespace control
namespace events {
class EventEmitter;
}
namespace automation {
class ModeManager;
class ParameterManager;
#if ANOLIS_ENABLE_AUTOMATION
class BTRuntime;
#endif
}  // namespace automation
namespace provider {
class ProviderSupervisor;
}  // namespace provider
namespace telemetry {
class InfluxSink;
}  // namespace telemetry
namespace runs {
class RunJournal;
}  // namespace runs
}  // namespace anolis

namespace anolis {
namespace http {

struct HttpServerDependencies {
    registry::DeviceRegistry &registry;
    state::StateCache &state_cache;
    control::CallRouter &call_router;
    provider::ProviderRegistry &provider_registry;

    HttpServerDependencies(registry::DeviceRegistry &registry_ref, state::StateCache &state_cache_ref,
                           control::CallRouter &call_router_ref, provider::ProviderRegistry &provider_registry_ref)
        : registry(registry_ref),
          state_cache(state_cache_ref),
          call_router(call_router_ref),
          provider_registry(provider_registry_ref) {}

    provider::ProviderSupervisor *supervisor = nullptr;
    std::shared_ptr<events::EventEmitter> event_emitter = nullptr;
    automation::ModeManager *mode_manager = nullptr;
    automation::ParameterManager *parameter_manager = nullptr;
    telemetry::InfluxSink *telemetry_sink = nullptr;     // optional: telemetry health surface
    runs::RunJournal *run_journal = nullptr;             // optional: run registry
    control::SafeStateController *safe_state = nullptr;  // optional: e-stop safe-state + latch
#if ANOLIS_ENABLE_AUTOMATION
    automation::BTRuntime *bt_runtime = nullptr;
#endif
};

/**
 * @brief HTTP server wrapper for Anolis runtime
 *
 * The HTTP server is an external adapter layer that exposes the
 * runtime's capabilities over REST endpoints. It runs in a separate
 * thread and delegates all operations to the kernel components
 * (Registry, StateCache, CallRouter).
 *
 * Thread model:
 * - Server runs in its own thread (via httplib::Server::listen_after_bind)
 * - Request handlers execute in httplib's thread pool
 * - All kernel operations are thread-safe (polling uses atomics/mutexes)
 *
 * Lifecycle:
 * - start() binds to configured port and spawns server thread
 * - stop() signals shutdown and joins server thread
 */
class HttpServer {
public:
    /**
     * @brief Construct HTTP server with references to kernel components
     *
     * @param config HTTP
     * configuration (bind address, port)
     * @param polling_interval_ms Polling interval from runtime config
     *
     * @param dependencies Aggregated kernel/service dependencies (required + optional pointers)
     */
    HttpServer(const runtime::HttpConfig &config, int polling_interval_ms, HttpServerDependencies dependencies);

    ~HttpServer();

    /**
     * @brief Start HTTP server
     *
     * Binds to configured address/port and starts server thread.
     * Returns true if server started successfully.
     *
     * @param error Populated with error message on failure
     * @return true if server started
     */
    bool start(std::string &error);

    /**
     * @brief Stop HTTP server
     *
     * Signals shutdown and waits for server thread to exit.
     * Safe to call multiple times.
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief Get the port server is listening on
     */
    int get_port() const { return port_; }

private:
    // Configuration
    runtime::HttpConfig config_;
    int port_ = 0;
    int polling_interval_ms_;

    // Kernel component references
    registry::DeviceRegistry &registry_;
    state::StateCache &state_cache_;
    control::CallRouter &call_router_;
    provider::ProviderRegistry &provider_registry_;
    provider::ProviderSupervisor *supervisor_;         // optional: nullptr when supervision disabled
    automation::ParameterManager *parameter_manager_;  // optional
    std::shared_ptr<events::EventEmitter> event_emitter_;
    automation::ModeManager *mode_manager_;     // optional
    telemetry::InfluxSink *telemetry_sink_;     // optional: nullptr when telemetry disabled
    runs::RunJournal *run_journal_;             // optional: nullptr when run registry unavailable
    control::SafeStateController *safe_state_;  // optional: e-stop safe-state + latch
#if ANOLIS_ENABLE_AUTOMATION
    automation::BTRuntime *bt_runtime_;  // optional
#endif

    // SSE client tracking
    std::atomic<int> sse_client_count_{0};
    static constexpr int MAX_SSE_CLIENTS = 32;

    // Server state
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point start_time_;

    // Route setup
    void setup_routes();

    // Route handlers (implemented in handlers.cpp)
    void handle_get_devices(const httplib::Request &req, httplib::Response &res);
    void handle_get_device_capabilities(const httplib::Request &req, httplib::Response &res);
    void handle_get_state(const httplib::Request &req, httplib::Response &res);
    void handle_get_device_state(const httplib::Request &req, httplib::Response &res);
    void handle_post_call(const httplib::Request &req, httplib::Response &res);
    void handle_get_runtime_status(const httplib::Request &req, httplib::Response &res);
    void handle_get_mode(const httplib::Request &req, httplib::Response &res);
    void handle_post_mode(const httplib::Request &req, httplib::Response &res);
    void handle_post_estop(const httplib::Request &req, httplib::Response &res);
    void handle_post_estop_clear(const httplib::Request &req, httplib::Response &res);
    void handle_get_parameters(const httplib::Request &req, httplib::Response &res);
    void handle_post_parameters(const httplib::Request &req, httplib::Response &res);
    void handle_get_automation_tree(const httplib::Request &req, httplib::Response &res);
    void handle_get_automation_status(const httplib::Request &req, httplib::Response &res);
    void handle_get_providers_health(const httplib::Request &req, httplib::Response &res);
    void handle_get_telemetry_status(const httplib::Request &req, httplib::Response &res);

    // Run registry (#7)
    void handle_post_runs(const httplib::Request &req, httplib::Response &res);
    void handle_get_runs(const httplib::Request &req, httplib::Response &res);
    void handle_get_run(const httplib::Request &req, httplib::Response &res);
    void handle_post_run_close(const httplib::Request &req, httplib::Response &res);
    void handle_post_run_events(const httplib::Request &req, httplib::Response &res);
    void handle_get_run_events(const httplib::Request &req, httplib::Response &res);

    // SSE handler
    void handle_get_events(const httplib::Request &req, httplib::Response &res);
    std::string format_sse_event(const events::Event &event);
};

}  // namespace http
}  // namespace anolis
