#pragma once

/**
 * @file influx_sink.hpp
 * @brief InfluxDB telemetry sink for Anolis observability layer
 *
 * This sink subscribes to the EventEmitter and writes state changes to
 * InfluxDB using the v2 Line Protocol. Features:
 * - Async batched writes (don't block state cache)
 * - Configurable batch size and flush interval
 * - Graceful degradation on connection failure
 * - Per-type value fields (value_double, value_int, value_bool)
 * - HTTP and HTTPS support (TLS via OpenSSL)
 *
 * InfluxDB Schema:
 * - Measurement: anolis_signal
 * - Tags: runtime_name, provider_id, device_id, signal_id
 * - Fields: value_double|value_int|value_bool, quality
 * - Timestamp: epoch milliseconds (precision=ms)
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../events/event_emitter.hpp"
#include "../events/event_types.hpp"
#include "../logging/logger.hpp"

// Forward declaration for cpp-httplib client
namespace httplib {
class Client;
}

namespace anolis {
namespace telemetry {

/**
 * @brief Configuration for InfluxDB telemetry sink
 */
struct InfluxConfig {
    bool enabled = false;                       // Enable telemetry sink
    std::string url = "http://localhost:8086";  // InfluxDB URL
    std::string org = "anolis";                 // InfluxDB organization
    std::string bucket = "anolis";              // InfluxDB bucket
    std::string token;                          // InfluxDB API token
    std::string runtime_name = "default";       // Runtime instance tag for multi-runtime disambiguation

    // Batching configuration
    size_t batch_size = 100;       // Flush when batch reaches this size
    int flush_interval_ms = 1000;  // Flush every N milliseconds

    // Connection settings
    int timeout_ms = 5000;          // HTTP request timeout
    int retry_interval_ms = 10000;  // Retry interval on connection failure

    // Queue settings (larger than SSE since persistence is important)
    size_t queue_size = 10000;            // Event queue size
    size_t max_retry_buffer_size = 1000;  // Max events to buffer on write failure
};

/**
 * @brief Escapes special characters in InfluxDB line protocol
 *
 * Tag keys/values and field keys need escaping of: comma, equals, space
 * Field string values need escaping of: double quote, backslash
 *
 * Tag values are provider-supplied too — `device_id`, `provider_id` and
 * `signal_id` reach here verbatim, with no charset validation anywhere upstream.
 * Line protocol is newline-delimited, so a `\n` in a device id splits one row
 * into two malformed ones and the whole batch is rejected: silent, repeated
 * telemetry loss. A trailing backslash is as bad, escaping the separator that
 * ends the tag set. Control bytes are therefore neutralised the same way as in
 * field strings, and a literal backslash is escaped so it cannot consume the
 * delimiter that follows it.
 */
inline std::string escape_tag(const std::string &s) {
    std::string result;
    result.reserve(s.size() + 10);  // Reserve extra for escapes
    for (char c : s) {
        switch (c) {
            case ',':
            case '=':
            case ' ':
            case '\\':
                result += '\\';
                result += c;
                continue;
            case '\n':
                result += "\\n";
                continue;
            case '\r':
                result += "\\r";
                continue;
            case '\t':
                result += "\\t";
                continue;
            default:
                break;
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            continue;  // remaining C0/DEL bytes carry no meaning in an identifier
        }
        result += c;
    }
    return result;
}

/**
 * @brief Escape a string for use as a line-protocol field value.
 *
 * Line protocol is newline-delimited, and a field string has no encoding for a
 * literal newline. A raw `\n` in a provider-supplied value therefore does not
 * corrupt one row — it splits the payload, and a value crafted to close the row
 * and open another forges a complete second measurement with attacker-chosen
 * tags. Control characters are neutralised rather than passed through: `\n`,
 * `\r` and `\t` become their two-character escapes (backslash-n and friends are
 * kept verbatim by line protocol, which only unescapes `"` and `\`), and any
 * other C0/DEL byte is dropped.
 */
inline std::string escape_field_string(const std::string &s) {
    std::string result;
    result.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"':
            case '\\':
                result += '\\';
                result += c;
                continue;
            case '\n':
                result += "\\n";
                continue;
            case '\r':
                result += "\\r";
                continue;
            case '\t':
                result += "\\t";
                continue;
            default:
                break;
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            continue;  // remaining C0/DEL bytes carry no meaning here
        }
        result += c;
    }
    return result;
}

/**
 * @brief Format a StateUpdateEvent as InfluxDB line protocol
 *
 * Format:
 *   anolis_signal,runtime_name=R,provider_id=X,device_id=Y,signal_id=Z value_TYPE=V,quality="Q" TIMESTAMP
 *
 * Type-specific fields:
 * - double: value_double=23.5
 * - int64: value_int=1200i
 * - uint64: value_uint=1200u
 * - bool: value_bool=true
 * - string: value_string="hello"
 */
inline std::string format_line_protocol(const events::StateUpdateEvent &event,
                                        const std::string &runtime_name = "default") {
    std::ostringstream line;

    // Measurement and tags
    line << "anolis_signal";
    line << ",runtime_name=" << escape_tag(runtime_name);
    line << ",provider_id=" << escape_tag(event.provider_id);
    line << ",device_id=" << escape_tag(event.device_id);
    line << ",signal_id=" << escape_tag(event.signal_id);

    // Field set (space before fields)
    line << " ";

    // Value field (type-specific)
    std::visit(
        [&line](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, double>) {
                // Handle special float values
                if (std::isnan(arg) || std::isinf(arg)) {
                    // InfluxDB doesn't support NaN/Inf, skip value field
                    // We still write quality
                } else {
                    line << "value_double=" << arg;
                }
            } else if constexpr (std::is_same_v<T, int64_t>) {
                line << "value_int=" << arg << "i";
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                line << "value_uint=" << arg << "u";
            } else if constexpr (std::is_same_v<T, bool>) {
                line << "value_bool=" << (arg ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::string>) {
                line << "value_string=\"" << escape_field_string(arg) << "\"";
            }
        },
        event.value);

    // Quality field (always present)
    // Check if we already wrote a value field
    std::string current = line.str();
    if (current.back() != ' ') {
        line << ",";  // Separator between fields
    }
    line << "quality=\"" << events::quality_to_string(event.quality) << "\"";

    // Timestamp (epoch milliseconds)
    line << " " << event.timestamp_ms;

    return line.str();
}

/**
 * @brief Format ModeChangeEvent as InfluxDB line protocol
 *
 * Measurement: mode_change
 * Tags: none (system-wide event)
 * Fields: previous_mode, new_mode
 * Timestamp: epoch milliseconds
 */
inline std::string format_mode_change_line_protocol(const events::ModeChangeEvent &event) {
    std::ostringstream line;

    // Measurement (no tags for system-wide events)
    line << "mode_change";

    // Field set
    line << " previous_mode=\"" << escape_field_string(event.previous_mode) << "\"";
    line << ",new_mode=\"" << escape_field_string(event.new_mode) << "\"";

    // Timestamp (epoch milliseconds)
    line << " " << event.timestamp_ms;

    return line.str();
}

/**
 * @brief Format ParameterChangeEvent as InfluxDB line protocol
 *
 * Measurement: parameter_change
 * Tags: parameter_name
 * Fields: old_value, new_value
 * Timestamp: epoch milliseconds
 */
inline std::string format_parameter_change_line_protocol(const events::ParameterChangeEvent &event) {
    std::ostringstream line;

    // Measurement with tag
    line << "parameter_change,parameter_name=" << escape_tag(event.parameter_name);

    // Field set
    line << " old_value=\"" << escape_field_string(event.old_value_str) << "\"";
    line << ",new_value=\"" << escape_field_string(event.new_value_str) << "\"";

    // Timestamp (epoch milliseconds)
    line << " " << event.timestamp_ms;

    return line.str();
}

/**
 * @brief InfluxDB telemetry sink
 *
 * Subscribes to EventEmitter and asynchronously writes events to InfluxDB.
 * Runs a background thread that batches writes for efficiency.
 */
class InfluxSink {
public:
    /**
     * @brief Create sink (does not start automatically)
     */
    explicit InfluxSink(const InfluxConfig &config)
        : config_(config), running_(false), connected_(false), total_written_(0), total_failed_(0) {}

    ~InfluxSink() { stop(); }

    // Non-copyable
    InfluxSink(const InfluxSink &) = delete;
    InfluxSink &operator=(const InfluxSink &) = delete;

    /**
     * @brief Start the sink with the given event emitter
     *
     * Subscribes to events and starts background flush thread.
     *
     * @param emitter Event emitter to subscribe to
     * @return true if started successfully
     */
    bool start(const std::shared_ptr<events::EventEmitter> &emitter) {
        if (running_.load()) {
            LOG_WARN("[InfluxSink] Already running");
            return false;
        }

        if (!config_.enabled) {
            LOG_INFO("[InfluxSink] Telemetry disabled in config");
            return false;
        }

        if (config_.token.empty()) {
            LOG_ERROR("[InfluxSink] No API token configured");
            return false;
        }

        emitter_ = emitter;

        // Subscribe with telemetry-sized queue
        subscription_ = emitter_->subscribe(events::EventFilter::all(), config_.queue_size, "telemetry-sink");

        if (!subscription_) {
            LOG_ERROR("[InfluxSink] Failed to subscribe to events");
            return false;
        }

        running_.store(true);
        flush_thread_ = std::thread(&InfluxSink::flush_loop, this);

        LOG_INFO("[InfluxSink] Started, writing to " << config_.url << "/" << config_.bucket);
        return true;
    }

    /**
     * @brief Stop the sink
     *
     * Flushes remaining events and stops background thread.
     */
    void stop() {
        if (!running_.load()) return;

        running_.store(false);

        // Unsubscribe to unblock the pop() call
        if (subscription_) {
            subscription_->unsubscribe();
        }

        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }

        subscription_.reset();
        emitter_.reset();

        LOG_INFO("[InfluxSink] Stopped. Written: " << total_written_ << ", Failed: " << total_failed_);
    }

    /**
     * @brief Check if sink is running
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief Check if connected to InfluxDB
     */
    bool is_connected() const { return connected_.load(); }

    /**
     * @brief Get total events written
     */
    size_t total_written() const { return total_written_.load(); }

    /**
     * @brief Get total events failed
     */
    size_t total_failed() const { return total_failed_.load(); }

    /**
     * @brief Get current batch size
     */
    size_t current_batch_size() const {
        std::lock_guard<std::mutex> lock(batch_mutex_);
        return batch_.size();
    }

    /**
     * @brief Whether telemetry is enabled in config (the sink was configured to run)
     */
    bool enabled() const { return config_.enabled; }

    /**
     * @brief Enqueue pre-formatted line-protocol rows onto the current batch.
     *
     * Used by the health snapshot task (#203); rows share the sink's batching,
     * flush cadence, and retry buffer with event-driven telemetry.
     */
    void enqueue_lines(const std::vector<std::string> &lines) {
        if (lines.empty()) return;
        std::lock_guard<std::mutex> lock(batch_mutex_);
        batch_.insert(batch_.end(), lines.begin(), lines.end());
    }

    /**
     * @brief Backend coordinates (never includes the API token)
     */
    const std::string &url() const { return config_.url; }
    const std::string &org() const { return config_.org; }
    const std::string &bucket() const { return config_.bucket; }

    /**
     * @brief Current subscriber-queue depth (events awaiting batching)
     *
     * Safe to call from another thread: SubscriberQueue is internally locked, and
     * the subscription handle is only reset in stop() after the HTTP server (and
     * thus any in-flight reader) has already stopped.
     */
    size_t queue_depth() const { return subscription_ ? subscription_->queue_size() : 0; }

    /**
     * @brief Events dropped because the subscriber queue overflowed (flush fell behind)
     */
    size_t dropped_events() const { return subscription_ ? subscription_->dropped_count() : 0; }

private:
    /**
     * @brief Background thread that collects events and flushes batches
     */
    void flush_loop() {
        auto last_flush = std::chrono::steady_clock::now();

        while (running_.load()) {
            // Pop events from subscription queue
            auto event_opt = subscription_->pop(100);  // 100ms timeout

            if (event_opt) {
                // Process StateUpdateEvents, ModeChangeEvents, and ParameterChangeEvents
                if (std::holds_alternative<events::StateUpdateEvent>(*event_opt)) {
                    const auto &update = std::get<events::StateUpdateEvent>(*event_opt);

                    std::lock_guard<std::mutex> lock(batch_mutex_);
                    batch_.push_back(format_line_protocol(update, config_.runtime_name));
                } else if (std::holds_alternative<events::ModeChangeEvent>(*event_opt)) {
                    const auto &mode_change = std::get<events::ModeChangeEvent>(*event_opt);

                    std::lock_guard<std::mutex> lock(batch_mutex_);
                    batch_.push_back(format_mode_change_line_protocol(mode_change));
                } else if (std::holds_alternative<events::ParameterChangeEvent>(*event_opt)) {
                    const auto &param_change = std::get<events::ParameterChangeEvent>(*event_opt);

                    std::lock_guard<std::mutex> lock(batch_mutex_);
                    batch_.push_back(format_parameter_change_line_protocol(param_change));
                }
            }

            // Check if we should flush
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count();

            bool should_flush = false;
            {
                std::lock_guard<std::mutex> lock(batch_mutex_);
                should_flush =
                    !batch_.empty() && (batch_.size() >= config_.batch_size || elapsed_ms >= config_.flush_interval_ms);
            }

            if (should_flush) {
                flush_batch();
                last_flush = now;
            }
        }

        // Final flush on shutdown
        flush_batch();
    }

    /**
     * @brief Flush current batch to InfluxDB
     */
    void flush_batch();  // Implemented in .cpp to avoid httplib include

    InfluxConfig config_;
    std::shared_ptr<events::EventEmitter> emitter_;
    std::unique_ptr<events::Subscription> subscription_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread flush_thread_;

    mutable std::mutex batch_mutex_;
    std::vector<std::string> batch_;         // Line protocol strings
    std::vector<std::string> retry_buffer_;  // Failed batches awaiting retry

    std::atomic<size_t> total_written_;
    std::atomic<size_t> total_failed_;

    // Last connection error time for rate-limited logging
    std::chrono::steady_clock::time_point last_error_log_;
};

}  // namespace telemetry
}  // namespace anolis
