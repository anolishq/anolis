#pragma once

/**
 * @file i_provider_handle.hpp
 * @brief Abstract provider-session interface used by the runtime core.
 */

#include <map>
#include <string>
#include <vector>

#include "protocol.pb.h"

namespace anolis {
namespace provider {

/**
 * @brief Abstract handle for a connected provider session.
 *
 * Runtime subsystems depend on this interface rather than `ProviderHandle`
 * directly so tests can substitute mocks and alternate implementations.
 *
 * Threading:
 * Callers may use a handle from multiple runtime threads. Implementations are
 * expected to provide the synchronization needed for their own transport/session
 * state.
 *
 * Error handling:
 * RPC-like methods return `false` on transport failure, protocol failure, or
 * provider-declared non-OK status. Callers inspect `last_error()` and
 * `last_status_code()` for the most recent failure details.
 */
class IProviderHandle {
public:
    virtual ~IProviderHandle() = default;

    /**
     * @brief Start the provider session and perform any required readiness handshake.
     *
     * A successful return means the handle is ready for normal RPC operations.
     */
    virtual bool start() = 0;

    /**
     * @brief Report whether the provider session is currently usable.
     */
    virtual bool is_available() const = 0;

    /**
     * @brief Execute the ADPP `Hello` handshake.
     */
    virtual bool hello(anolis::deviceprovider::v1::HelloResponse &response) = 0;

    /**
     * @brief List the devices currently exposed by the provider.
     */
    virtual bool list_devices(std::vector<anolis::deviceprovider::v1::Device> &devices) = 0;

    /**
     * @brief Fetch full capability metadata for one provider-local device.
     */
    virtual bool describe_device(const std::string &device_id,
                                 anolis::deviceprovider::v1::DescribeDeviceResponse &response) = 0;

    /**
     * @brief Read one or more signals from a device.
     *
     * Implementations are expected to block until a response or timeout.
     */
    virtual bool read_signals(const std::string &device_id, const std::vector<std::string> &signal_ids,
                              anolis::deviceprovider::v1::ReadSignalsResponse &response) = 0;

    /**
     * @brief Execute a device function call.
     *
     * `function_id` and `function_name` are both provided so callers can route
     * by numeric or symbolic selector while preserving the resolved mapping sent
     * over the provider protocol.
     */
    virtual bool call(const std::string &device_id, uint32_t function_id, const std::string &function_name,
                      const std::map<std::string, anolis::deviceprovider::v1::Value> &args,
                      anolis::deviceprovider::v1::CallResponse &response) = 0;

    /**
     * @brief Get the most recent human-readable failure string.
     */
    virtual const std::string &last_error() const = 0;

    /**
     * @brief Get the most recent provider status code associated with a call.
     */
    virtual anolis::deviceprovider::v1::Status_Code last_status_code() const = 0;

    /**
     * @brief Get the configured runtime-facing provider identifier.
     */
    virtual const std::string &provider_id() const = 0;
};

}  // namespace provider
}  // namespace anolis
