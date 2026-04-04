#pragma once

/**
 * @file device_registry.hpp
 * @brief Live device inventory and immutable capability snapshots.
 */

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

/**
 * @brief Flattened signal metadata used for polling and validation.
 */
struct SignalSpec {
    std::string signal_id;
    std::string label;
    anolis::deviceprovider::v1::ValueType type;
    bool readable;
    bool writable;
    bool is_default;  // Included in the default periodic poll plan
};

/**
 * @brief Flattened function metadata used for selector resolution and arg validation.
 */
struct FunctionSpec {
    uint32_t function_id;
    std::string function_name;
    std::string label;
    std::vector<anolis::deviceprovider::v1::ArgSpec> args;  // Full ArgSpec retained for validation
};

/**
 * @brief Immutable capability snapshot for a discovered device.
 *
 * Stores the original protobuf plus lookup maps keyed by signal ID and
 * function name so validation paths do not need to rescan protobuf fields on
 * every read or call.
 *
 * The distinct name avoids the Windows `DeviceCapabilities` macro collision.
 */
struct DeviceCapabilitySet {
    anolis::deviceprovider::v1::Device proto;
    std::unordered_map<std::string, SignalSpec> signals_by_id;
    std::unordered_map<std::string, FunctionSpec> functions_by_id;
};

/**
 * @brief Registry entry for a single discovered device.
 */
struct RegisteredDevice {
    std::string provider_id;  // Configured name (e.g., "sim0")
    std::string device_id;    // Provider-local ID (e.g., "tempctl0")
    DeviceCapabilitySet capabilities;

    /** @brief Return the canonical `provider_id/device_id` handle. */
    std::string get_handle() const { return provider_id + "/" + device_id; }
};

/**
 * @brief Live inventory of discovered devices and their immutable capabilities.
 *
 * DeviceRegistry separates discovery from publication. Provider I/O happens in
 * `inspect_provider_devices()`, which builds a temporary provider-local
 * inventory without mutating the published registry. The caller can then make a
 * single exclusive update via `commit_provider_devices()`.
 *
 * Threading:
 * Published inventory reads and commits are synchronized. Lookup APIs return
 * copies so callers can safely use results after later registry updates.
 *
 * Invariants:
 * Device handles are globally unique by `provider_id/device_id`.
 * Capability snapshots are treated as immutable after commit.
 */
class DeviceRegistry {
public:
    DeviceRegistry() = default;

    /**
     * @brief Discover a provider's devices without publishing them.
     *
     * Performs `ListDevices` followed by `DescribeDevice` for each device and
     * builds a temporary inventory in `discovered_devices`.
     *
     * Error handling:
     * Devices that fail `DescribeDevice` or capability parsing are skipped.
     * Returns false if discovery fails outright or no devices can be
     * registered.
     *
     * @param provider_id Configured provider identifier
     * @param provider Connected provider handle used for discovery RPCs
     * @param discovered_devices Output vector populated with discovered devices
     * @return true if at least one device was successfully discovered
     */
    bool inspect_provider_devices(const std::string &provider_id, anolis::provider::IProviderHandle &provider,
                                  std::vector<RegisteredDevice> &discovered_devices);

    /**
     * @brief Publish a previously discovered provider inventory.
     *
     * When `replace_existing` is true, all existing devices for the provider
     * are removed before the new inventory is installed and reindexed.
     *
     * @param provider_id Configured provider identifier
     * @param discovered_devices Provider-local inventory to publish
     * @param replace_existing Whether to replace existing devices for the same provider
     */
    void commit_provider_devices(const std::string &provider_id, std::vector<RegisteredDevice> discovered_devices,
                                 bool replace_existing = false);

    /**
     * @brief Discover a provider and immediately publish the resulting inventory.
     *
     * This is a convenience wrapper around `inspect_provider_devices()` plus
     * `commit_provider_devices()`.
     *
     * @param provider_id Configured provider identifier
     * @param provider Connected provider handle used for discovery RPCs
     * @param replace_existing Whether to replace the provider's existing published devices
     * @return true if discovery produced at least one publishable device
     */
    bool discover_provider(const std::string &provider_id, anolis::provider::IProviderHandle &provider,
                           bool replace_existing = false);

    /**
     * @brief Look up a device by provider ID and device ID.
     *
     * Returns a copy so callers remain insulated from later registry mutation.
     */
    std::optional<RegisteredDevice> get_device_copy(const std::string &provider_id, const std::string &device_id) const;

    /**
     * @brief Look up a device by its canonical `provider/device` handle.
     *
     * Returns a copy so callers can safely use the result after later registry
     * mutation or provider restart.
     */
    std::optional<RegisteredDevice> get_device_by_handle_copy(const std::string &handle) const;

    /** @brief Get a snapshot copy of all published devices. */
    std::vector<RegisteredDevice> get_all_devices() const;

    /** @brief Get a snapshot copy of all published devices for one provider. */
    std::vector<RegisteredDevice> get_devices_for_provider(const std::string &provider_id) const;

    /**
     * @brief Remove all published devices belonging to one provider.
     *
     * The handle index is rebuilt before the method returns.
     */
    void clear_provider_devices(const std::string &provider_id);

    /** @brief Get the current published device count. */
    size_t device_count() const;

    /**
     * @brief Get the most recent discovery or capability-build error string.
     *
     * This is primarily intended for diagnostics after a failed discovery
     * attempt.
     */
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
