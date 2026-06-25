#include <chrono>
#include <iomanip>

#include "../../events/event_emitter.hpp"
#include "../../logging/logger.hpp"
#include "../../registry/device_registry.hpp"
#include "../../state/state_cache.hpp"
#include "../json.hpp"
#include "../server.hpp"
#include "utils.hpp"

namespace anolis {
namespace http {

//=============================================================================
// GET /v0/state
//=============================================================================
void HttpServer::handle_get_state(const httplib::Request &, httplib::Response &res) {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    auto devices = registry_.get_all_devices();  // Returns vector<RegisteredDevice> by value
    nlohmann::json devices_json = nlohmann::json::array();

    for (const auto &device : devices) {
        auto state = state_cache_.get_device_state(device.get_handle());
        if (state) {
            devices_json.push_back(encode_device_state(*state, device.provider_id, device.device_id));
        }
    }

    nlohmann::json response = {
        {"status", make_status(StatusCode::OK)}, {"generated_at_epoch_ms", now_ms}, {"devices", devices_json}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// GET /v0/state/{provider_id}/{device_id}
//=============================================================================
void HttpServer::handle_get_device_state(const httplib::Request &req, httplib::Response &res) {
    std::string provider_id, device_id;
    if (!parse_path_params(req, provider_id, device_id)) {
        send_json(res, StatusCode::INVALID_ARGUMENT,
                  make_error_response(StatusCode::INVALID_ARGUMENT, "Invalid path parameters"));
        return;
    }

    auto device_opt = registry_.get_device_copy(provider_id, device_id);
    if (!device_opt.has_value()) {
        send_json(res, StatusCode::NOT_FOUND,
                  make_error_response(StatusCode::NOT_FOUND, "Device not found: " + provider_id + "/" + device_id));
        return;
    }
    const auto &device = device_opt.value();

    auto state = state_cache_.get_device_state(device.get_handle());
    if (!state) {
        // Device exists but no state yet (shouldn't normally happen)
        send_json(res, StatusCode::UNAVAILABLE,
                  make_error_response(StatusCode::UNAVAILABLE, "Device state not available"));
        return;
    }

    // Handle optional signal_id query params
    std::vector<std::string> filter_signals;
    if (req.has_param("signal_id")) {
        // Collect all signal_id params
        auto it = req.params.find("signal_id");
        if (it != req.params.end()) {
            // Get all values for signal_id
            auto range = req.params.equal_range("signal_id");
            for (auto it2 = range.first; it2 != range.second; ++it2) {
                filter_signals.push_back(it2->second);
            }
        }
    }

    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // Build filtered or full state
    nlohmann::json values = nlohmann::json::array();

    auto worst_quality = anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK;

    for (const auto &[signal_id, cached] : state->signals) {
        // Apply filter if specified
        if (!filter_signals.empty()) {
            bool found = false;
            for (const auto &fs : filter_signals) {
                if (fs == signal_id) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        values.push_back(encode_signal_value(cached, signal_id));
        if (cached.quality > worst_quality) {
            worst_quality = cached.quality;
        }
    }

    std::string device_quality = state->provider_available ? quality_to_string(worst_quality) : "UNAVAILABLE";

    nlohmann::json response = {{"status", make_status(StatusCode::OK)},
                               {"generated_at_epoch_ms", now_ms},
                               {"provider_id", provider_id},
                               {"device_id", device_id},
                               {"quality", device_quality},
                               {"values", values}};

    send_json(res, StatusCode::OK, response);
}

//=============================================================================
// GET /v0/events (SSE - Server-Sent Events)
//=============================================================================
void HttpServer::handle_get_events(const httplib::Request &req, httplib::Response &res) {
    // Check if event emitter is available
    if (!event_emitter_) {
        nlohmann::json error_response = make_error_response(StatusCode::UNAVAILABLE, "Event streaming not enabled");
        send_json(res, StatusCode::UNAVAILABLE, error_response);
        return;
    }

    // Check SSE client limit
    int current_clients = sse_client_count_.load();
    if (current_clients >= MAX_SSE_CLIENTS) {
        LOG_WARN("[SSE] Client rejected: max clients (" << MAX_SSE_CLIENTS << ") reached");
        nlohmann::json error_response = make_error_response(StatusCode::UNAVAILABLE, "Too many SSE clients");
        res.status = 503;  // Service Unavailable
        res.set_content(error_response.dump(), "application/json");
        return;
    }

    // Parse optional filters
    events::EventFilter filter;
    if (req.has_param("provider_id")) {
        filter.provider_id = req.get_param_value("provider_id");
    }
    if (req.has_param("device_id")) {
        filter.device_id = req.get_param_value("device_id");
    }
    if (req.has_param("signal_id")) {
        filter.signal_id = req.get_param_value("signal_id");
    }

    // Subscribe to events (convert to shared_ptr for lambda capture)
    std::string client_name = "sse-" + std::to_string(current_clients + 1);
    std::shared_ptr<events::Subscription> subscription(event_emitter_->subscribe(filter, 100, client_name).release());

    if (!subscription) {
        LOG_ERROR("[SSE] Failed to create subscription");
        nlohmann::json error_response = make_error_response(StatusCode::UNAVAILABLE, "Failed to subscribe to events");
        res.status = 503;
        res.set_content(error_response.dump(), "application/json");
        return;
    }

    sse_client_count_++;

    // Set SSE headers
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");  // Disable nginx buffering

    // Per-client keep-alive counter (shared_ptr for lambda capture)
    auto keepalive_counter = std::make_shared<int>(0);

    // Use chunked content provider for streaming
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, subscription, client_name, keepalive_counter](size_t, httplib::DataSink &sink) {
            // Check if server is still running
            if (!running_.load()) {
                return false;  // Stop streaming
            }

            // Try to get event with timeout (allows periodic keep-alive)
            auto event_opt = subscription->pop(1000);  // 1 second timeout

            if (event_opt) {
                // Format and send event
                std::string sse_data = format_sse_event(*event_opt);
                if (!sink.write(sse_data.c_str(), sse_data.size())) {
                    LOG_WARN("[SSE] Write failed for " << client_name);
                    return false;  // Stop streaming on write error
                }
                *keepalive_counter = 0;  // Reset on successful event
            } else {
                // No event - send keep-alive comment every ~15 seconds
                if (++(*keepalive_counter) >= 15) {
                    std::string keepalive = ": keepalive\n\n";
                    if (!sink.write(keepalive.c_str(), keepalive.size())) {
                        LOG_WARN("[SSE] Keep-alive failed for " << client_name);
                        return false;  // Stop streaming on write error
                    }
                    *keepalive_counter = 0;
                }
            }

            return true;  // Continue streaming
        },
        [this](bool) {
            // Cleanup callback when stream ends
            sse_client_count_--;
        });
}

// Helper: Format event as SSE message
std::string HttpServer::format_sse_event(const events::Event &event) {
    std::string result;

    std::visit(
        [&result](auto &&e) {
            using T = std::decay_t<decltype(e)>;

            nlohmann::json data;

            if constexpr (std::is_same_v<T, events::StateUpdateEvent>) {
                result = "event: state_update\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["provider_id"] = e.provider_id;
                data["device_id"] = e.device_id;
                data["signal_id"] = e.signal_id;
                data["timestamp_ms"] = e.timestamp_ms;
                data["quality"] = events::quality_to_string(e.quality);

                // Encode value in established format
                std::visit(
                    [&data](auto &&val) {
                        using V = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<V, double>) {
                            data["value"] = {{"type", "double"}, {"double", val}};
                        } else if constexpr (std::is_same_v<V, int64_t>) {
                            data["value"] = {{"type", "int64"}, {"int64", val}};
                        } else if constexpr (std::is_same_v<V, uint64_t>) {
                            data["value"] = {{"type", "uint64"}, {"uint64", val}};
                        } else if constexpr (std::is_same_v<V, bool>) {
                            data["value"] = {{"type", "bool"}, {"bool", val}};
                        } else if constexpr (std::is_same_v<V, std::string>) {
                            data["value"] = {{"type", "string"}, {"string", val}};
                        }
                    },
                    e.value);

            } else if constexpr (std::is_same_v<T, events::QualityChangeEvent>) {
                result = "event: quality_change\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["provider_id"] = e.provider_id;
                data["device_id"] = e.device_id;
                data["signal_id"] = e.signal_id;
                data["old_quality"] = events::quality_to_string(e.old_quality);
                data["new_quality"] = events::quality_to_string(e.new_quality);
                data["timestamp_ms"] = e.timestamp_ms;

            } else if constexpr (std::is_same_v<T, events::DeviceAvailabilityEvent>) {
                result = "event: device_availability\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["provider_id"] = e.provider_id;
                data["device_id"] = e.device_id;
                data["available"] = e.available;
                data["timestamp_ms"] = e.timestamp_ms;

            } else if constexpr (std::is_same_v<T, events::ModeChangeEvent>) {
                result = "event: mode_change\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["previous_mode"] = e.previous_mode;
                data["new_mode"] = e.new_mode;
                data["timestamp_ms"] = e.timestamp_ms;

            } else if constexpr (std::is_same_v<T, events::ParameterChangeEvent>) {
                result = "event: parameter_change\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["parameter_name"] = e.parameter_name;
                data["old_value"] = e.old_value_str;
                data["new_value"] = e.new_value_str;
                data["timestamp_ms"] = e.timestamp_ms;

            } else if constexpr (std::is_same_v<T, events::BTErrorEvent>) {
                result = "event: bt_error\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["node"] = e.node;
                data["error"] = e.error;
                data["timestamp_ms"] = e.timestamp_ms;

            } else if constexpr (std::is_same_v<T, events::ProviderHealthChangeEvent>) {
                result = "event: provider_health_change\n";
                result += "id: " + std::to_string(e.event_id) + "\n";

                data["provider_id"] = e.provider_id;
                data["state"] = e.state;
                data["timestamp_ms"] = e.timestamp_ms;
            }

            result += "data: " + data.dump() + "\n\n";
        },
        event);

    return result;
}

}  // namespace http
}  // namespace anolis
