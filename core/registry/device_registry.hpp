#pragma once

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "protocol.pb.h"
#include "provider/i_provider_handle.hpp"  // Changed to interface

namespace anolis {
namespace registry {

// Signal specification for quick lookup
struct SignalSpec {
    std::string signal_id;
    std::string label;
    anolis::deviceprovider::v1::ValueType type;
    bool readable;
    bool writable;
    bool is_default;  // Polled automatically
};

// Function specification for quick lookup
struct FunctionSpec {
    uint32_t function_id;
    std::string function_name;
    std::string label;
    std::vector<anolis::deviceprovider::v1::ArgSpec> args;  // Full ArgSpec for validation
};

// Immutable device capabilities (populated from DescribeDevice)
// Named DeviceCapabilitySet to avoid Windows macro collision with DeviceCapabilities
struct DeviceCapabilitySet {
    // Raw protobuf (for serialization/inspection)
    anolis::deviceprovider::v1::Device proto;

    // Lookup maps (for fast validation)
    std::unordered_map<std::string, SignalSpec> signals_by_id;
    std::unordered_map<std::string, FunctionSpec> functions_by_id;
};

// Registered device (provider + device metadata)
struct RegisteredDevice {
    std::string provider_id;  // Configured name (e.g., "sim0")
    std::string device_id;    // Provider-local ID (e.g., "tempctl0")
    DeviceCapabilitySet capabilities;

    // Composite key for global device handle
    std::string get_handle() const { return provider_id + "/" + device_id; }
};

// Device Registry - Thread-safe immutable inventory after discovery
/**
 * Thread Safety:
 * - All read methods use shared_lock (concurrent reads safe)
 * - All write methods use unique_lock (exclusive access)
 * - Returns by-value to prevent dangling pointers after clear_provider_devices()
 *
 * Migration from raw pointers:
 * - Old: get_device() returned const RegisteredDevice* (could dangle)
 * - New: get_device_copy() returns std::optional<RegisteredDevice> (safe copy)
 * - get_all_devices() now returns vector<RegisteredDevice> by value
 */
class DeviceRegistry {
public:
    DeviceRegistry() = default;

    // Discovery: Perform Hello -> ListDevices -> DescribeDevice for each device
    // Thread-safe: Uses unique_lock
    bool discover_provider(const std::string &provider_id, anolis::provider::IProviderHandle &provider,
                           bool replace_existing = false);

    // Lookup - Thread-safe by-value returns
    // Returns copy to prevent dangling pointers when registry is mutated
    std::optional<RegisteredDevice> get_device_copy(const std::string &provider_id, const std::string &device_id) const;
    std::optional<RegisteredDevice> get_device_by_handle_copy(const std::string &handle) const;

    // Iteration - Thread-safe by-value returns
    // Returns copies to prevent iterator invalidation during concurrent mutations
    std::vector<RegisteredDevice> get_all_devices() const;
    std::vector<RegisteredDevice> get_devices_for_provider(const std::string &provider_id) const;

    // Management - Thread-safe: Uses unique_lock
    void clear_provider_devices(const std::string &provider_id);

    // Status - Thread-safe
    size_t device_count() const;
    std::string last_error() const;

private:
    // Storage: vector for stable pointers, map for fast lookup
    std::vector<RegisteredDevice> devices_;
    std::unordered_map<std::string, size_t> handle_to_index_;  // "provider/device" -> index

    std::string error_;

    // Thread safety: shared_mutex allows concurrent reads, exclusive writes
    mutable std::shared_mutex mutex_;

    // Helper: Build capability maps from protobuf (not thread-safe, called under lock)
    bool build_capabilities(const anolis::deviceprovider::v1::Device &proto_device,
                            const anolis::deviceprovider::v1::CapabilitySet &proto_caps, DeviceCapabilitySet &caps);
};

}  // namespace registry
}  // namespace anolis

