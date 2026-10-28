#include "server.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "errors.hpp"
#include "events/event_emitter.hpp"
#include "http/auth.hpp"
#include "logging/logger.hpp"
#include "provider/provider_supervisor.hpp"

namespace anolis {
namespace http {

namespace {
constexpr int kDefaultTimeoutSeconds = 5;
constexpr int kDefaultTimeoutMilliseconds = 0;
constexpr int kStatusNoContent = 204;
constexpr int kStatusBadRequest = 400;
constexpr int kStatusNotFound = 404;
constexpr int kStatusInternal = 500;
constexpr int kStatusForbidden = 403;
constexpr int kStatusUnsupportedMediaType = 415;

// The first CORS allowlist entry that admits @p origin, or null. An entry is an
// exact origin, "*", or a pattern with one '*' standing for any run of
// characters.
const std::string *allowed_origin_entry(const std::vector<std::string> &allowed_origins, const std::string &origin) {
    const auto matched =
        std::find_if(allowed_origins.begin(), allowed_origins.end(), [&origin](const std::string &allowed) {
            if (allowed == "*") {
                return true;
            }
            const auto wildcard_pos = allowed.find('*');
            if (wildcard_pos == std::string::npos) {
                return allowed == origin;
            }
            const std::string prefix = allowed.substr(0, wildcard_pos);
            const std::string suffix = allowed.substr(wildcard_pos + 1);
            if (origin.size() < prefix.size() + suffix.size()) {
                return false;
            }
            return origin.compare(0, prefix.size(), prefix) == 0 &&
                   origin.compare(origin.size() - suffix.size(), suffix.size(), suffix) == 0;
        });
    return matched == allowed_origins.end() ? nullptr : &*matched;
}

// The host part of a Host header: "[::1]:8080" -> "::1", "Localhost:8080" ->
// "localhost".
std::string host_name(const std::string &host_header) {
    std::string name = host_header;
    if (!name.empty() && name.front() == '[') {
        const auto close = name.find(']');
        name = close == std::string::npos ? name.substr(1) : name.substr(1, close - 1);
    } else if (const auto colon = name.find(':');
               colon != std::string::npos && name.find(':', colon + 1) == std::string::npos) {
        name = name.substr(0, colon);
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name;
}

// With auth off, a request must name this host: loopback, or the address the
// server is bound to. A page whose own name has been re-pointed at 127.0.0.1
// (DNS rebinding) arrives with that name in Host (#540).
bool host_allowed(const std::string &host_header, const std::string &bind) {
    if (host_header.empty()) {
        return true;  // HTTP/1.0, no Host to rebind; browsers always send one
    }
    const std::string name = host_name(host_header);
    return name == "localhost" || name == "127.0.0.1" || name == "::1" || name == host_name(bind);
}

// The media type of a Content-Type value, lower-cased, without parameters.
std::string media_type(const std::string &content_type) {
    std::string type = content_type.substr(0, content_type.find(';'));
    type.erase(type.find_last_not_of(" \t") + 1);
    type.erase(0, type.find_first_not_of(" \t"));
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
    return type;
}
}  // namespace

HttpServer::HttpServer(const runtime::HttpConfig &config, int polling_interval_ms, HttpServerDependencies dependencies)
    : config_(config),
      polling_interval_ms_(polling_interval_ms),
      registry_(dependencies.registry),
      state_cache_(dependencies.state_cache),
      call_router_(dependencies.call_router),
      provider_registry_(dependencies.provider_registry),
      supervisor_(dependencies.supervisor),
      parameter_manager_(dependencies.parameter_manager),
      event_emitter_(std::move(dependencies.event_emitter)),
      mode_manager_(dependencies.mode_manager),
      telemetry_sink_(dependencies.telemetry_sink),
      run_journal_(dependencies.run_journal),
      safe_state_(dependencies.safe_state),
      device_loss_latch_(dependencies.device_loss_latch),
      stop_chain_(dependencies.stop_chain),
      staleness_policy_(dependencies.staleness_policy)
#if ANOLIS_ENABLE_AUTOMATION
      ,
      bt_runtime_(dependencies.bt_runtime)
#endif
{
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::string &error) {
    if (running_.load()) {
        error = "Server already running";
        return false;
    }

    const bool tls_enabled = !config_.tls_cert_path.empty();
    LOG_INFO("[HTTP] Starting server on " << (tls_enabled ? "https://" : "http://") << config_.bind << ":"
                                          << config_.port);

    // Create server (TLS when a cert/key pair is configured).
    if (tls_enabled) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        auto ssl_server =
            std::make_unique<httplib::SSLServer>(config_.tls_cert_path.c_str(), config_.tls_key_path.c_str());
        if (!ssl_server->is_valid()) {
            error = "Failed to load TLS certificate/key (cert='" + config_.tls_cert_path + "', key='" +
                    config_.tls_key_path + "')";
            return false;
        }
        server_ = std::move(ssl_server);
        LOG_INFO("[HTTP] TLS enabled");
#else
        error = "TLS configured (http.tls_cert_path) but this build lacks OpenSSL support";
        return false;
#endif
    } else {
        server_ = std::make_unique<httplib::Server>();
    }

    // Configure server
    server_->set_read_timeout(kDefaultTimeoutSeconds, kDefaultTimeoutMilliseconds);
    server_->set_write_timeout(kDefaultTimeoutSeconds, kDefaultTimeoutMilliseconds);

    // Set thread pool size to handle SSE + REST concurrently
    // Rule: thread_pool_size >= max_sse_clients + headroom for REST
    // Default is 40 threads (32 SSE clients + 8 headroom)
    int pool_size = config_.thread_pool_size;
    server_->new_task_queue = [pool_size] { return new httplib::ThreadPool(pool_size); };

    // Add CORS headers to all responses (allowlist with wildcard support)
    const bool allow_credentials = config_.cors_allow_credentials;
    server_->set_post_routing_handler([allow_credentials, origins = config_.cors_allowed_origins](
                                          const httplib::Request &req, httplib::Response &res) {
        const auto origin_it = req.headers.find("Origin");
        if (origin_it == req.headers.end()) {
            return;
        }

        const std::string origin = origin_it->second;
        const std::string *allowed = allowed_origin_entry(origins, origin);
        if (allowed == nullptr) {
            return;
        }
        const std::string response_origin = *allowed == "*" ? "*" : origin;

        res.set_header("Access-Control-Allow-Origin", response_origin.c_str());
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        if (allow_credentials) {
            res.set_header("Access-Control-Allow-Credentials", "true");
        }
    });

    // Browser guard, then the authentication gate. Both run before routing, so
    // a refused request never reaches a handler (#540). A browser sends a
    // text/plain or bodyless POST with no preflight, and the CORS headers above
    // only decide whether the page may read the answer:
    // - with auth off, the Host must name this machine (DNS rebinding), unless
    //   http.allow_insecure_bind exposes it to names this check cannot know;
    // - a POST to /v0/ that carries an Origin must come from an allowed origin
    //   (a browser sends one on every cross-origin POST; curl, scripts and the
    //   workbench's proxy send none);
    // - a POST with a body must say application/json.
    // Authentication covers every endpoint including the SSE stream. Loopback
    // is exempt only when http.auth_exempt_loopback is set; see config.
    const bool check_host = !config_.auth_enabled && !config_.allow_insecure_bind;
    server_->set_pre_routing_handler([check_host, bind = config_.bind, origins = config_.cors_allowed_origins,
                                      auth_enabled = config_.auth_enabled,
                                      exempt_loopback = config_.auth_exempt_loopback,
                                      token = config_.auth_token](const httplib::Request &req, httplib::Response &res) {
        auto refuse = [&res](int status, const char *message) {
            res.status = status;
            res.set_content(std::string(R"({"error":")") + message + R"("})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        };
        if (check_host && !host_allowed(req.get_header_value("Host"), bind)) {
            return refuse(kStatusForbidden, "host not allowed");
        }
        if (req.method == "POST" && req.path.rfind("/v0/", 0) == 0) {
            if (req.has_header("Origin") && allowed_origin_entry(origins, req.get_header_value("Origin")) == nullptr) {
                return refuse(kStatusForbidden, "origin not allowed");
            }
            // The body is not read yet: go by what the headers declare.
            const std::string length = req.get_header_value("Content-Length");
            const bool has_body = (!length.empty() && length != "0") || req.has_header("Transfer-Encoding");
            if (has_body && media_type(req.get_header_value("Content-Type")) != "application/json") {
                return refuse(kStatusUnsupportedMediaType, "a request body must be application/json");
            }
        }
        if (authorize(auth_enabled, exempt_loopback, token, req.remote_addr, req.get_header_value("Authorization"))) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        res.status = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.set_content(R"({"error":"unauthorized"})", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
    if (config_.auth_enabled) {
        LOG_INFO("[HTTP] Bearer-token authentication enabled"
                 << (config_.auth_exempt_loopback ? " (loopback exempt)" : ""));
    }

    // Set up routes
    setup_routes();

    // Set error handler for JSON error responses (called for HTTP errors like 404)
    // Only override content if no content has been set
    server_->set_error_handler([](const httplib::Request &req, httplib::Response &res) {
        // If content was already set by the handler, don't override it
        if (!res.body.empty()) {
            return;
        }

        StatusCode code = StatusCode::INTERNAL;
        std::string message = "Internal server error";

        if (res.status == kStatusNotFound) {
            code = StatusCode::NOT_FOUND;
            message = "Route not found: " + req.method + " " + req.path;
        } else if (res.status == kStatusBadRequest) {
            code = StatusCode::INVALID_ARGUMENT;
            message = "Bad request";
        } else if (res.status == kStatusInternal) {
            code = StatusCode::INTERNAL;
            message = "Internal server error";
        }

        nlohmann::json response = make_error_response(code, message);
        res.set_content(response.dump(), "application/json");
    });

    // Set exception handler
    server_->set_exception_handler([](const httplib::Request &, httplib::Response &res, std::exception_ptr ep) {
        std::string msg = "Unknown error";
        try {
            std::rethrow_exception(std::move(ep));
        } catch (const std::exception &e) {
            msg = e.what();
            LOG_ERROR("[HTTP] Exception: " << e.what());
        } catch (...) {
            msg = "Unknown exception";
            LOG_ERROR("[HTTP] Unknown exception");
        }

        nlohmann::json response = make_error_response(StatusCode::INTERNAL, msg);
        res.status = kStatusInternal;
        res.set_content(response.dump(), "application/json");
    });

    // Record server start time for uptime reporting
    start_time_ = std::chrono::steady_clock::now();

    // Bind first. Port 0 asks the OS for a free port; record the one it chose.
    const int bound_port = config_.port == 0 ? server_->bind_to_any_port(config_.bind)
                                             : (server_->bind_to_port(config_.bind, config_.port) ? config_.port : -1);
    if (bound_port < 0) {
        error = "Failed to bind to " + config_.bind + ":" + std::to_string(config_.port);
        return false;
    }
    port_ = bound_port;

    // Start server thread
    running_.store(true);
    server_thread_ = std::make_unique<std::thread>([this]() {
        LOG_INFO("[HTTP] Server thread started");
        server_->listen_after_bind();
        LOG_INFO("[HTTP] Server thread exiting");
    });

    LOG_INFO("[HTTP] Server listening on " << config_.bind << ":" << port_);
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) {
        return;
    }

    LOG_INFO("[HTTP] Stopping server");
    running_.store(false);

    if (server_) {
        server_->stop();
    }

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }

    server_thread_.reset();
    server_.reset();
    LOG_INFO("[HTTP] Server stopped");
}

void HttpServer::setup_routes() {
    // GET /v0/devices - List all devices
    server_->Get("/v0/devices",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_devices(req, res); });

    // GET /v0/devices/:provider_id/:device_id/capabilities - Get device capabilities
    server_->Get(
        R"(/v0/devices/([^/]+)/([^/]+)/capabilities)",
        [this](const httplib::Request &req, httplib::Response &res) { handle_get_device_capabilities(req, res); });

    // GET /v0/state - Get all device state
    server_->Get("/v0/state",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_state(req, res); });

    // GET /v0/state/:provider_id/:device_id - Get specific device state
    server_->Get(R"(/v0/state/([^/]+)/([^/]+))",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_device_state(req, res); });

    // POST /v0/call - Execute device function
    server_->Post("/v0/call",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_call(req, res); });

    // OPTIONS /v0/call - CORS preflight for POST
    server_->Options("/v0/call", [](const httplib::Request &, httplib::Response &res) {
        res.status = kStatusNoContent;
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

    // OPTIONS catch-all for CORS preflight on all routes
    server_->Options(R"(/v0/.*)", [](const httplib::Request &, httplib::Response &res) {
        res.status = kStatusNoContent;
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

    // GET /v0/runtime/status - Get runtime status
    server_->Get("/v0/runtime/status",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_runtime_status(req, res); });

    // GET /v0/mode - Get current automation mode
    server_->Get("/v0/mode",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_mode(req, res); });

    // POST /v0/mode - Set automation mode
    server_->Post("/v0/mode",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_mode(req, res); });

    // POST /v0/estop - Engage the software safe-state (latching)
    server_->Post("/v0/estop",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_estop(req, res); });

    // POST /v0/estop/clear - Release the e-stop latch
    server_->Post("/v0/estop/clear",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_estop_clear(req, res); });

    // GET /v0/parameters - Get all parameters
    server_->Get("/v0/parameters",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_parameters(req, res); });

    // POST /v0/parameters - Update parameter value
    server_->Post("/v0/parameters",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_parameters(req, res); });

    // GET /v0/automation/tree - Get loaded behavior tree
    server_->Get("/v0/automation/tree",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_automation_tree(req, res); });

    // GET /v0/automation/status - Get automation health status
    server_->Get("/v0/automation/status", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_automation_status(req, res);
    });

    // GET /v0/providers/health - Get provider health status
    server_->Get("/v0/providers/health", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_providers_health(req, res);
    });

    // GET /v0/telemetry/status - Get telemetry (InfluxDB sink) health
    server_->Get("/v0/telemetry/status", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_telemetry_status(req, res);
    });

    // Run registry (#7)
    server_->Post("/v0/runs",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_runs(req, res); });
    server_->Get("/v0/runs",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_runs(req, res); });
    server_->Post(R"(/v0/runs/([^/]+)/close)",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_run_close(req, res); });
    // Run event/marker stream (#116). Register the more specific /events routes
    // before the generic by-id GET.
    server_->Post(R"(/v0/runs/([^/]+)/events)",
                  [this](const httplib::Request &req, httplib::Response &res) { handle_post_run_events(req, res); });
    server_->Get(R"(/v0/runs/([^/]+)/events)",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_run_events(req, res); });
    server_->Get(R"(/v0/runs/([^/]+))",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_run(req, res); });

    // GET /v0/events - SSE event stream
    server_->Get("/v0/events",
                 [this](const httplib::Request &req, httplib::Response &res) { handle_get_events(req, res); });

    LOG_INFO("[HTTP] Routes configured:");
    LOG_INFO("[HTTP]   GET  /v0/devices");
    LOG_INFO("[HTTP]   GET  /v0/devices/{provider_id}/{device_id}/capabilities");
    LOG_INFO("[HTTP]   GET  /v0/state");
    LOG_INFO("[HTTP]   GET  /v0/state/{provider_id}/{device_id}");
    LOG_INFO("[HTTP]   POST /v0/call");
    LOG_INFO("[HTTP]   GET  /v0/runtime/status");
    LOG_INFO("[HTTP]   GET  /v0/mode");
    LOG_INFO("[HTTP]   POST /v0/mode");
    LOG_INFO("[HTTP]   POST /v0/estop");
    LOG_INFO("[HTTP]   POST /v0/estop/clear");
    LOG_INFO("[HTTP]   GET  /v0/parameters");
    LOG_INFO("[HTTP]   POST /v0/parameters");
    LOG_INFO("[HTTP]   GET  /v0/automation/tree");
    LOG_INFO("[HTTP]   GET  /v0/automation/status");
    LOG_INFO("[HTTP]   GET  /v0/providers/health");
    LOG_INFO("[HTTP]   GET  /v0/telemetry/status");
    LOG_INFO("[HTTP]   GET  /v0/events (SSE)");
}

}  // namespace http
}  // namespace anolis
