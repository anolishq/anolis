#pragma once

/**
 * @file provider_handle.hpp
 * @brief High-level synchronous ADPP client wrapper for one provider process.
 */

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "i_provider_handle.hpp"
#include "protocol.pb.h"
#include "provider_process.hpp"

namespace anolis {
namespace provider {

/**
 * @brief Blocking ADPP client facade backed by a spawned provider process.
 *
 * ProviderHandle layers request/response framing, protobuf serialization,
 * request ID correlation, and handshake/ready checks on top of
 * `ProviderProcess`.
 *
 * Threading:
 * Public RPC methods are safe to call from multiple threads, but requests are
 * serialized internally so one provider session has at most one in-flight ADPP
 * exchange at a time.
 *
 * Availability:
 * `is_available()` is stronger than child liveness alone; it also requires the
 * session to remain healthy after framing, timeout, or protocol checks.
 */
class ProviderHandle : public IProviderHandle {
public:
    /** @brief Construct a provider handle with operation-specific timeouts. */
    ProviderHandle(const std::string &provider_id, const std::string &executable_path,
                   const std::vector<std::string> &args = {}, int timeout_ms = 5000, int hello_timeout_ms = 5000,
                   int ready_timeout_ms = 60000, int shutdown_timeout_ms = 2000);
    ~ProviderHandle() override = default;

    ProviderHandle(const ProviderHandle &) = delete;
    ProviderHandle &operator=(const ProviderHandle &) = delete;

    /**
     * @brief Spawn the process, perform `Hello`, and optionally wait for `WaitReady`.
     *
     * @return true when the provider session is ready for runtime use
     */
    bool start() override;

    /** @brief Report whether the process is alive and the ADPP session is healthy. */
    bool is_available() const override {
        return session_healthy_.load(std::memory_order_acquire) && process_.is_running();
    }

    /** @brief Issue the ADPP `Hello` handshake. */
    bool hello(anolis::deviceprovider::v1::HelloResponse &response) override;
    /** @brief Issue `WaitReady` using the configured ready timeout. */
    bool wait_ready(anolis::deviceprovider::v1::WaitReadyResponse &response);
    /** @brief Issue `ListDevices` and return the provider's device roster. */
    bool list_devices(std::vector<anolis::deviceprovider::v1::Device> &devices) override;
    /** @brief Issue `DescribeDevice` for one provider-local device ID. */
    bool describe_device(const std::string &device_id,
                         anolis::deviceprovider::v1::DescribeDeviceResponse &response) override;
    /** @brief Issue `ReadSignals` for one device and requested signal subset. */
    bool read_signals(const std::string &device_id, const std::vector<std::string> &signal_ids,
                      anolis::deviceprovider::v1::ReadSignalsResponse &response) override;
    /** @brief Issue `Call` for one device/function selector and argument map. */
    bool call(const std::string &device_id, uint32_t function_id, const std::string &function_name,
              const std::map<std::string, anolis::deviceprovider::v1::Value> &args,
              anolis::deviceprovider::v1::CallResponse &response) override;

    /** @brief Fetch provider-reported health via ADPP GetHealth. */
    bool get_health(anolis::deviceprovider::v1::GetHealthResponse &response) override;

    /**
     * @brief Return the last client-side or provider-reported error.
     *
     * By value under `mutex_`: `get_health`/`read_signals`/etc. may be called
     * from the health-snapshot task, the poller, and HTTP threads at once, so
     * `error_` is only ever touched while holding the lock. Because `mutex_`
     * also serializes the in-flight request, this can block until a concurrent
     * RPC on another thread finishes (bounded by that op's timeout) — an
     * accepted fault-path latency, since blocking-but-correct beats the prior
     * data race.
     */
    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    /**
     * @brief Return the last ADPP status code observed from the provider.
     *
     * Under `mutex_` for the same reason as last_error(): it is written in
     * send_request under the lock and read here from other threads.
     */
    anolis::deviceprovider::v1::Status_Code last_status_code() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_status_code_;
    }

    /** @brief Return the runtime-local provider identifier. */
    const std::string &provider_id() const override { return process_.provider_id(); }

private:
    ProviderProcess process_;
    std::atomic<bool> session_healthy_{false};
    std::string error_;
    anolis::deviceprovider::v1::Status_Code last_status_code_ = anolis::deviceprovider::v1::Status_Code_CODE_OK;
    std::atomic<uint32_t> next_request_id_;
    // Guards error_ and serializes the one-in-flight request. `mutable` so the
    // const last_error() can copy error_ under the lock.
    mutable std::mutex mutex_;

    int timeout_ms_;        // ADPP operation timeout
    int hello_timeout_ms_;  // Process liveness check timeout
    int ready_timeout_ms_;  // Hardware initialization timeout

    /**
     * @brief Set error_ under mutex_.
     *
     * For the failure paths OUTSIDE send_request (which already holds mutex_
     * while writing error_ directly — calling this there would self-deadlock).
     */
    void set_error(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(message);
    }

    /** @brief Serialize, send, and validate one ADPP request/response exchange. */
    bool send_request(const anolis::deviceprovider::v1::Request &request,
                      anolis::deviceprovider::v1::Response &response, uint64_t request_id);

    /** @brief Wait for one correlated response frame within the given timeout. */
    bool wait_for_response(anolis::deviceprovider::v1::Response &response, uint64_t expected_request_id,
                           int timeout_ms);
};

}  // namespace provider
}  // namespace anolis
