/**
 * @file influx_sink.cpp
 * @brief Implementation of InfluxDB telemetry sink
 */

#include "influx_sink.hpp"

#include "logging/logger.hpp"

// cpp-httplib with OpenSSL support (enabled via vcpkg feature)
#include <httplib.h>

#include <chrono>
#include <iterator>

namespace anolis {
namespace telemetry {

void InfluxSink::flush_batch() {
    // Move batch out under lock and prepend retry buffer (minimize lock time)
    std::vector<std::string> lines_to_write;
    {
        std::lock_guard<std::mutex> lock(batch_mutex_);

        // Prepend any failed events from retry buffer
        if (!retry_buffer_.empty()) {
            lines_to_write = std::move(retry_buffer_);
            retry_buffer_.clear();
        }

        // Append current batch
        if (!batch_.empty()) {
            lines_to_write.insert(lines_to_write.end(), std::make_move_iterator(batch_.begin()),
                                  std::make_move_iterator(batch_.end()));
            batch_.clear();
        }

        if (lines_to_write.empty()) {
            return;
        }
    }

    // Build line protocol body (newline separated)
    std::string body;
    size_t total_size = 0;
    for (const auto &line : lines_to_write) {
        total_size += line.size() + 1;  // +1 for newline
    }
    body.reserve(total_size);

    for (const auto &line : lines_to_write) {
        body += line;
        body += '\n';
    }

    // Create HTTP/HTTPS client from URL
    // httplib::Client auto-detects scheme and handles SSL when built with OpenSSL
    auto client = std::make_unique<httplib::Client>(config_.url);

    client->set_connection_timeout(std::chrono::milliseconds(config_.timeout_ms));
    client->set_read_timeout(std::chrono::milliseconds(config_.timeout_ms));
    client->set_write_timeout(std::chrono::milliseconds(config_.timeout_ms));

    // Build URL path with query params
    std::string path = "/api/v2/write?org=" + config_.org + "&bucket=" + config_.bucket + "&precision=ms";

    // Set authorization header
    httplib::Headers headers = {{"Authorization", "Token " + config_.token},
                                {"Content-Type", "text/plain; charset=utf-8"}};

    // Send request
    auto result = client->Post(path.c_str(), headers, body, "text/plain");

    if (result) {
        if (result->status >= 200 && result->status < 300) {
            // Success - retry buffer was already cleared above
            connected_.store(true);
            total_written_.fetch_add(lines_to_write.size());

            // Log periodically (every 1000 writes)
            if (total_written_.load() % 1000 == 0) {
                LOG_INFO("[InfluxSink] Written " << total_written_.load() << " events to InfluxDB");
            }
        } else if (result->status != 429 && result->status >= 400 && result->status < 500) {
            // A permanent rejection (#572): 400 malformed line protocol, 401/403
            // a token without write access to the bucket, 404 no such org or
            // bucket, 413 too large, 422 unprocessable. The same request would
            // be rejected again, and re-queued it would sit ahead of every new
            // batch until the retry buffer filled and new rows were dropped.
            // So it is dropped and counted, not retried. 429 and 5xx are
            // transient and take the retry path below.
            connected_.store(true);  // the server answered
            total_rejected_.fetch_add(lines_to_write.size());
            total_failed_.fetch_add(lines_to_write.size());

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_error_log_).count();
            if (elapsed >= 10) {
                const bool auth = result->status == 401 || result->status == 403;
                LOG_ERROR("[InfluxSink] Write rejected (HTTP "
                          << result->status << "): " << result->body << "; dropped " << lines_to_write.size()
                          << " rows, not retried" << (auth ? " (check the InfluxDB token and bucket)" : "") << " ("
                          << total_rejected_.load() << " rejected in all)");
                last_error_log_ = now;
            }
        } else {
            // HTTP error - save to retry buffer up to max size
            connected_.store(false);
            size_t failed_count = lines_to_write.size();

            {
                std::lock_guard<std::mutex> lock(batch_mutex_);
                // Add failed events to retry buffer, respecting max size
                size_t space_available = (config_.max_retry_buffer_size > retry_buffer_.size())
                                             ? (config_.max_retry_buffer_size - retry_buffer_.size())
                                             : 0;

                if (space_available > 0) {
                    size_t to_keep = std::min(space_available, lines_to_write.size());
                    using IteratorDiff = std::vector<std::string>::difference_type;
                    const auto keep_end = std::next(lines_to_write.begin(), static_cast<IteratorDiff>(to_keep));
                    retry_buffer_.insert(retry_buffer_.end(), std::make_move_iterator(lines_to_write.begin()),
                                         std::make_move_iterator(keep_end));

                    // Count dropped events
                    size_t dropped = lines_to_write.size() - to_keep;
                    if (dropped > 0) {
                        total_failed_.fetch_add(dropped);
                    }
                } else {
                    // Retry buffer full, drop all events
                    total_failed_.fetch_add(failed_count);
                }
            }

            // Rate-limit error logging
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_error_log_).count();

            if (elapsed >= 10) {  // Log at most every 10 seconds
                LOG_WARN("[InfluxSink] HTTP error " << result->status << ": " << result->body << " ("
                                                    << retry_buffer_.size() << " events in retry buffer)");
                last_error_log_ = now;
            }
        }
    } else {
        // Connection error - save to retry buffer up to max size
        connected_.store(false);
        size_t failed_count = lines_to_write.size();

        {
            std::lock_guard<std::mutex> lock(batch_mutex_);
            // Add failed events to retry buffer, respecting max size
            size_t space_available = (config_.max_retry_buffer_size > retry_buffer_.size())
                                         ? (config_.max_retry_buffer_size - retry_buffer_.size())
                                         : 0;

            if (space_available > 0) {
                size_t to_keep = std::min(space_available, lines_to_write.size());
                using IteratorDiff = std::vector<std::string>::difference_type;
                const auto keep_end = std::next(lines_to_write.begin(), static_cast<IteratorDiff>(to_keep));
                retry_buffer_.insert(retry_buffer_.end(), std::make_move_iterator(lines_to_write.begin()),
                                     std::make_move_iterator(keep_end));

                // Count dropped events
                size_t dropped = lines_to_write.size() - to_keep;
                if (dropped > 0) {
                    total_failed_.fetch_add(dropped);
                }
            } else {
                // Retry buffer full, drop all events
                total_failed_.fetch_add(failed_count);
            }
        }

        // Rate-limit error logging
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_error_log_).count();

        if (elapsed >= 10) {
            auto err = result.error();
            LOG_ERROR("[InfluxSink] Connection error: " << httplib::to_string(err) << " (" << retry_buffer_.size()
                                                        << " events in retry buffer)");
            last_error_log_ = now;
        }
    }
}

}  // namespace telemetry
}  // namespace anolis
