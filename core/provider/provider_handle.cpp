#include "provider_handle.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include "logging/logger.hpp"

namespace anolis {
namespace provider {

ProviderHandle::ProviderHandle(const std::string &provider_id, const std::string &executable_path,
                               const std::vector<std::string> &args, int timeout_ms, int hello_timeout_ms,
                               int ready_timeout_ms, int shutdown_timeout_ms)
    : process_(provider_id, executable_path, args, shutdown_timeout_ms),
      next_request_id_(1),
      timeout_ms_(timeout_ms),
      hello_timeout_ms_(hello_timeout_ms),
      ready_timeout_ms_(ready_timeout_ms) {}

bool ProviderHandle::start() {
    LOG_INFO("[" << process_.provider_id() << "] Starting provider");
    session_healthy_.store(false, std::memory_order_release);

    // Spawn process
    if (!process_.spawn()) {
        error_ = process_.last_error();
        return false;
    }
    session_healthy_.store(true, std::memory_order_release);

    // Step 1: Process liveness check with Hello handshake
    anolis::deviceprovider::v1::HelloResponse hello_response;
    if (!hello(hello_response)) {
        session_healthy_.store(false, std::memory_order_release);
        LOG_ERROR("[" << process_.provider_id() << "] Hello handshake failed: " << error_);
        return false;
    }

    LOG_INFO("[" << process_.provider_id() << "] Hello succeeded: " << hello_response.provider_name() << " v"
                 << hello_response.provider_version());

    // Step 2: Check if provider supports WaitReady via capability signaling
    bool supports_ready = hello_response.metadata().count("supports_wait_ready") &&
                          hello_response.metadata().at("supports_wait_ready") == "true";

    if (supports_ready) {
        LOG_INFO("[" << process_.provider_id() << "] Provider supports WaitReady, waiting for initialization...");
        anolis::deviceprovider::v1::WaitReadyResponse ready_response;
        if (!wait_ready(ready_response)) {
            session_healthy_.store(false, std::memory_order_release);
            LOG_ERROR("[" << process_.provider_id() << "] WaitReady failed: " << error_);
            return false;
        }

        // Log initialization diagnostics
        if (ready_response.diagnostics().count("init_time_ms")) {
            LOG_INFO("[" << process_.provider_id() << "] Provider initialized in "
                         << ready_response.diagnostics().at("init_time_ms") << "ms");
        }
    } else {
        LOG_INFO("[" << process_.provider_id() << "] Provider does not support WaitReady");
    }

    return true;
}

bool ProviderHandle::hello(anolis::deviceprovider::v1::HelloResponse &response) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    auto *hello_req = request.mutable_hello();
    hello_req->set_protocol_version("v1");
    hello_req->set_client_name("anolis-runtime");
    hello_req->set_client_version("0.1.0");

    anolis::deviceprovider::v1::Response resp;
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_hello()) {
        error_ = "Response missing hello field";
        return false;
    }

    response = resp.hello();
    if (response.protocol_version() != "v1") {
        error_ = "Protocol version mismatch: runtime expects v1, provider reported " + response.protocol_version();
        return false;
    }

    return true;
}

bool ProviderHandle::wait_ready(anolis::deviceprovider::v1::WaitReadyResponse &response) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    auto *ready_req = request.mutable_wait_ready();
    ready_req->set_max_wait_ms_hint(ready_timeout_ms_);

    anolis::deviceprovider::v1::Response resp;
    // Use ready_timeout_ms_ for this RPC (hardware initialization can be slow)
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_wait_ready()) {
        error_ = "Response missing wait_ready field";
        return false;
    }

    response = resp.wait_ready();
    return true;
}

bool ProviderHandle::list_devices(std::vector<anolis::deviceprovider::v1::Device> &devices) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    request.mutable_list_devices();

    anolis::deviceprovider::v1::Response resp;
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_list_devices()) {
        error_ = "Response missing list_devices field";
        return false;
    }

    devices.clear();
    for (const auto &device : resp.list_devices().devices()) {
        devices.push_back(device);
    }
    return true;
}

bool ProviderHandle::describe_device(const std::string &device_id,
                                     anolis::deviceprovider::v1::DescribeDeviceResponse &response) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    request.mutable_describe_device()->set_device_id(device_id);

    anolis::deviceprovider::v1::Response resp;
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_describe_device()) {
        error_ = "Response missing describe_device field";
        return false;
    }

    response = resp.describe_device();
    return true;
}

bool ProviderHandle::read_signals(const std::string &device_id, const std::vector<std::string> &signal_ids,
                                  anolis::deviceprovider::v1::ReadSignalsResponse &response) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    auto *read_req = request.mutable_read_signals();
    read_req->set_device_id(device_id);
    for (const auto &sig_id : signal_ids) {
        read_req->add_signal_ids(sig_id);
    }

    anolis::deviceprovider::v1::Response resp;
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_read_signals()) {
        error_ = "Response missing read_signals field";
        return false;
    }

    response = resp.read_signals();
    return true;
}

bool ProviderHandle::call(const std::string &device_id, uint32_t function_id, const std::string &function_name,
                          const std::map<std::string, anolis::deviceprovider::v1::Value> &args,
                          anolis::deviceprovider::v1::CallResponse &response) {
    anolis::deviceprovider::v1::Request request;
    uint64_t request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    request.set_request_id(request_id);
    auto *call_req = request.mutable_call();
    call_req->set_device_id(device_id);
    call_req->set_function_id(function_id);
    call_req->set_function_name(function_name);
    for (const auto &[key, value] : args) {
        (*call_req->mutable_args())[key] = value;
    }

    anolis::deviceprovider::v1::Response resp;
    if (!send_request(request, resp, request_id)) {
        return false;
    }

    if (!resp.has_call()) {
        error_ = "Response missing call field";
        return false;
    }

    response = resp.call();
    return true;
}

bool ProviderHandle::send_request(const anolis::deviceprovider::v1::Request &request,
                                  anolis::deviceprovider::v1::Response &response, uint64_t request_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!session_healthy_.load(std::memory_order_acquire)) {
        error_ = "Provider session not healthy";
        return false;
    }

    // Check provider is running
    if (!process_.is_running()) {
        session_healthy_.store(false, std::memory_order_release);
        error_ = "Provider process not running";
        return false;
    }

    // Serialize request
    std::string serialized;
    if (!request.SerializeToString(&serialized)) {
        error_ = "Failed to serialize request";
        return false;
    }

    // Determine timeout based on request type
    int timeout_to_use = timeout_ms_;  // Default for normal operations
    if (request.has_hello()) {
        timeout_to_use = hello_timeout_ms_;
    } else if (request.has_wait_ready()) {
        timeout_to_use = ready_timeout_ms_;
    }

    // Send frame
    if (!process_.client().write_frame(reinterpret_cast<const uint8_t *>(serialized.data()), serialized.size(),
                                       timeout_to_use)) {
        session_healthy_.store(false, std::memory_order_release);
        error_ = "Failed to write request: " + process_.client().last_error();
        return false;
    }

    // Wait for response with request_id validation
    if (!wait_for_response(response, request_id, timeout_to_use)) {
        return false;
    }

    // Capture status code
    last_status_code_ = response.status().code();

    // Check status code
    if (last_status_code_ != anolis::deviceprovider::v1::Status_Code_CODE_OK) {
        error_ = "Provider returned error: " + response.status().message();
        return false;
    }

    return true;
}

bool ProviderHandle::wait_for_response(anolis::deviceprovider::v1::Response &response, uint64_t expected_request_id,
                                       int timeout_ms) {
    auto start = std::chrono::steady_clock::now();

    // Simple polling loop with timeout
    while (true) {
        // Check overall timeout
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (elapsed_ms >= timeout_ms) {
            session_healthy_.store(false, std::memory_order_release);
            error_ = "Timeout waiting for response (" + std::to_string(timeout_ms) + "ms)";
            LOG_ERROR("[" << process_.provider_id() << "] " << error_);
            return false;
        }

        int remaining = static_cast<int>(timeout_ms - elapsed_ms);
        // Poll in small chunks to check process status
        int poll_wait = (remaining > 50) ? 50 : remaining;

        if (process_.client().wait_for_data(poll_wait)) {
            // Data available, read full frame with TOTAL remaining time
            elapsed = std::chrono::steady_clock::now() - start;  // Update elapsed
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            int read_timeout = static_cast<int>(timeout_ms - elapsed_ms);
            if (read_timeout < 0) {
                read_timeout = 0;
            }

            std::vector<uint8_t> frame_data;
            if (process_.client().read_frame(frame_data, read_timeout)) {
                // Parse response
                if (!response.ParseFromArray(frame_data.data(), static_cast<int>(frame_data.size()))) {
                    session_healthy_.store(false, std::memory_order_release);
                    error_ = "Failed to parse response protobuf";
                    return false;
                }

                // Validate request_id correlation
                if (response.request_id() != expected_request_id) {
                    session_healthy_.store(false, std::memory_order_release);
                    error_ = "Response request_id mismatch (expected " + std::to_string(expected_request_id) +
                             ", got " + std::to_string(response.request_id()) + ")";
                    LOG_ERROR("[" << process_.provider_id() << "] " << error_);
                    return false;
                }

                return true;
            }

            if (!process_.client().last_error().empty()) {
                session_healthy_.store(false, std::memory_order_release);
                error_ = "Failed to read response: " + process_.client().last_error();
            } else {
                error_ = "Timed out reading response payload";
            }
            return false;
        } else if (!process_.client().last_error().empty()) {
            session_healthy_.store(false, std::memory_order_release);
            error_ = "Failed waiting for response: " + process_.client().last_error();
            return false;
        }

        // Check if process died
        if (!process_.is_running()) {
            session_healthy_.store(false, std::memory_order_release);
            error_ = "Provider process died while waiting for response";
            return false;
        }
    }
}

}  // namespace provider
}  // namespace anolis
