#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "protocol.pb.h"
#include "provider/i_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace anolis {

// Forward declarations
namespace automation {
class ModeManager;
}

namespace control {

// Call request - High-level API for executing device functions
struct CallRequest {
    std::string device_handle;  // "provider_id/device_id"
    uint32_t function_id = 0;   // Optional selector used by HTTP API
    std::string function_name;  // Optional selector used by automation/internal callers
    std::map<std::string, anolis::deviceprovider::v1::Value> args;
    bool is_automated = false;  // true if called from BT automation, false if manual (HTTP/UI)
};

// Call result - Status and optional return values
struct CallResult {
    bool success;
    std::string error_message;
    std::map<std::string, anolis::deviceprovider::v1::Value> results;
    anolis::deviceprovider::v1::Status_Code status_code = anolis::deviceprovider::v1::Status_Code_CODE_OK;
};

// CallRouter - Unified control path with validation and state management
class CallRouter {
public:
    CallRouter(const registry::DeviceRegistry &registry, state::StateCache &state_cache);

    /**
     * Set mode manager for manual/auto gating
     * Must be called before execute_call if automation is enabled.
     */
    void set_mode_manager(automation::ModeManager *mode_manager, const std::string &gating_policy);

    // Execute a function call
    // This is the ONLY way to execute control actions in Anolis
    CallResult execute_call(const CallRequest &request, provider::ProviderRegistry &provider_registry);

    // Validation only (no execution)
    bool validate_call(const CallRequest &request, std::string &error) const;

private:
    using ProviderLock = std::shared_ptr<std::mutex>;

    const registry::DeviceRegistry &registry_;
    state::StateCache &state_cache_;
    automation::ModeManager *mode_manager_ = nullptr;  // optional
    std::string manual_gating_policy_ = "BLOCK";

    // Per-provider lock table for serialized access (v0: prevent concurrent calls to same provider).
    // Callers hold a shared_ptr copy so lock lifetime is independent of lock-table mutation scope.
    std::map<std::string, ProviderLock> provider_locks_;
    std::mutex provider_locks_mutex_;

    // Validation helpers
    bool validate_device_exists(const std::string &device_handle, std::string &error) const;
    bool validate_function_exists(const registry::RegisteredDevice &device, const std::string &function_name,
                                  const registry::FunctionSpec *&out_spec, std::string &error) const;
    bool resolve_function_spec(const registry::RegisteredDevice &device, const CallRequest &request,
                               const registry::FunctionSpec *&out_spec, std::string &out_function_name,
                               std::string &error) const;
    bool validate_arguments(const registry::FunctionSpec &spec,
                            const std::map<std::string, anolis::deviceprovider::v1::Value> &args,
                            std::string &error) const;
    bool validate_argument_type(const anolis::deviceprovider::v1::ArgSpec &spec,
                                const anolis::deviceprovider::v1::Value &value, std::string &error) const;
    bool validate_argument_range(const anolis::deviceprovider::v1::ArgSpec &spec,
                                 const anolis::deviceprovider::v1::Value &value, std::string &error) const;
    std::string value_type_to_string(anolis::deviceprovider::v1::ValueType type) const;

    // Helper: Parse device_handle into provider_id and device_id
    bool parse_device_handle(const std::string &device_handle, std::string &provider_id, std::string &device_id,
                             std::string &error) const;
    ProviderLock get_or_create_provider_lock(const std::string &provider_id);
};

}  // namespace control
}  // namespace anolis

