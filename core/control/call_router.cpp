#include "call_router.hpp"

#include <sstream>
#include <unordered_map>

#include "automation/mode_manager.hpp"
#include "logging/logger.hpp"

namespace anolis {
namespace control {

CallRouter::CallRouter(const registry::DeviceRegistry& registry, state::StateCache& state_cache)
    : registry_(registry), state_cache_(state_cache) {}

void CallRouter::set_mode_manager(automation::ModeManager* mode_manager, const std::string& gating_policy) {
    mode_manager_ = mode_manager;
    manual_gating_policy_ = gating_policy;
}

CallRouter::ProviderLock CallRouter::get_or_create_provider_lock(const std::string& provider_id) {
    std::lock_guard<std::mutex> map_lock(provider_locks_mutex_);
    return provider_locks_.try_emplace(provider_id, std::make_shared<std::mutex>()).first->second;
}

CallResult CallRouter::execute_call(const CallRequest& request, provider::ProviderRegistry& provider_registry) {
    CallResult result;
    result.success = false;

    // Block control operations in IDLE mode
    if (mode_manager_ != nullptr && mode_manager_->is_idle()) {
        result.error_message = "Control operations blocked in IDLE mode";
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION;
        LOG_WARN("[CallRouter] " << result.error_message);
        return result;
    }

    // Check manual/auto contention (only for manual calls)
    if (!request.is_automated && mode_manager_ != nullptr &&
        mode_manager_->current_mode() == automation::RuntimeMode::AUTO) {
        if (manual_gating_policy_ == "BLOCK") {
            result.error_message = "Manual call blocked in AUTO mode (policy: BLOCK)";
            result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_FAILED_PRECONDITION;
            LOG_WARN("[CallRouter] " << result.error_message);
            return result;
        } else if (manual_gating_policy_ == "OVERRIDE") {
            // Allow call to proceed
        }
    }

    // Parse device handle
    std::string provider_id, device_id;
    if (!parse_device_handle(request.device_handle, provider_id, device_id, result.error_message)) {
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT;
        return result;
    }

    // Get provider handle
    auto provider = provider_registry.get_provider(provider_id);
    if (!provider) {
        result.error_message = "Provider not found: " + provider_id;
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND;
        LOG_ERROR("[CallRouter] " << result.error_message);
        return result;
    }

    if (!provider->is_available()) {
        result.error_message = "Provider not available: " + provider_id;
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE;
        LOG_ERROR("[CallRouter] " << result.error_message);
        return result;
    }

    // Get device and function spec for function_id
    auto device_opt = registry_.get_device_copy(provider_id, device_id);
    if (!device_opt.has_value()) {
        result.error_message = "Device not found in registry";
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND;
        return result;
    }
    const auto& device = device_opt.value();

    const registry::FunctionSpec* func_spec = nullptr;
    std::string resolved_function_name;
    if (!resolve_function_spec(device, request, func_spec, resolved_function_name, result.error_message)) {
        if (result.error_message.find("not found") != std::string::npos) {
            result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_NOT_FOUND;
        } else {
            result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT;
        }
        return result;
    }

    // Validate function arguments after selector resolution.
    if (!validate_arguments(*func_spec, request.args, result.error_message)) {
        result.status_code = anolis::deviceprovider::v1::Status_Code_CODE_INVALID_ARGUMENT;
        LOG_WARN("[CallRouter] Validation failed: " << result.error_message);
        return result;
    }

    // Per-provider serialization: lock-table lookup is synchronized and lock lifetime is explicit.
    ProviderLock provider_lock_handle = get_or_create_provider_lock(provider_id);
    std::lock_guard<std::mutex> provider_lock(*provider_lock_handle);

    // Forward call to provider
    anolis::deviceprovider::v1::CallResponse call_response;
    if (!provider->call(device_id, func_spec->function_id, resolved_function_name, request.args, call_response)) {
        result.error_message = "Provider call failed: " + provider->last_error();
        result.status_code = provider->last_status_code();
        LOG_ERROR("[CallRouter] Call failed: " << result.error_message << " (Code: " << result.status_code << ")");
        return result;
    }

    // Extract results
    for (const auto& [key, value] : call_response.results()) {
        result.results[key] = value;
    }

    // Post-call state update: immediate poll of affected device
    state_cache_.poll_device_now(request.device_handle, provider_registry);

    result.success = true;
    return result;
}

bool CallRouter::validate_call(const CallRequest& request, std::string& error) const {
    // Check device exists
    if (!validate_device_exists(request.device_handle, error)) {
        return false;
    }

    // Parse device handle
    std::string provider_id, device_id;
    if (!parse_device_handle(request.device_handle, provider_id, device_id, error)) {
        return false;
    }

    // Get device
    auto device_opt = registry_.get_device_copy(provider_id, device_id);
    if (!device_opt.has_value()) {
        error = "Device not found in registry: " + request.device_handle;
        return false;
    }
    const auto& device = device_opt.value();

    // Resolve function selector (ID or name)
    const registry::FunctionSpec* func_spec = nullptr;
    std::string resolved_function_name;
    if (!resolve_function_spec(device, request, func_spec, resolved_function_name, error)) {
        return false;
    }

    // Validate arguments
    if (!validate_arguments(*func_spec, request.args, error)) {
        return false;
    }

    return true;
}

bool CallRouter::validate_device_exists(const std::string& device_handle, std::string& error) const {
    auto device_opt = registry_.get_device_by_handle_copy(device_handle);
    if (!device_opt.has_value()) {
        error = "Device not found: " + device_handle;
        return false;
    }
    return true;
}

bool CallRouter::validate_function_exists(const registry::RegisteredDevice& device, const std::string& function_name,
                                          const registry::FunctionSpec*& out_spec, std::string& error) const {
    auto it = device.capabilities.functions_by_id.find(function_name);
    if (it == device.capabilities.functions_by_id.end()) {
        error = "Function not found: " + function_name + " on device " + device.get_handle();
        return false;
    }
    out_spec = &it->second;
    return true;
}

bool CallRouter::resolve_function_spec(const registry::RegisteredDevice& device, const CallRequest& request,
                                       const registry::FunctionSpec*& out_spec, std::string& out_function_name,
                                       std::string& error) const {
    out_spec = nullptr;
    out_function_name.clear();

    if (request.function_id != 0) {
        for (const auto& [name, spec] : device.capabilities.functions_by_id) {
            if (spec.function_id == request.function_id) {
                out_spec = &spec;
                out_function_name = name;
                break;
            }
        }

        if (out_spec == nullptr) {
            error = "Function ID not found: " + std::to_string(request.function_id);
            return false;
        }

        if (!request.function_name.empty() && request.function_name != out_function_name) {
            error = "Function selector mismatch: function_id " + std::to_string(request.function_id) + " maps to '" +
                    out_function_name + "', not '" + request.function_name + "'";
            return false;
        }

        return true;
    }

    if (request.function_name.empty()) {
        error = "Missing function selector (function_id or function_name)";
        return false;
    }

    if (!validate_function_exists(device, request.function_name, out_spec, error)) {
        return false;
    }

    out_function_name = request.function_name;
    return true;
}

bool CallRouter::validate_arguments(const registry::FunctionSpec& spec,
                                    const std::map<std::string, anolis::deviceprovider::v1::Value>& args,
                                    std::string& error) const {
    // Build arg name to ArgSpec map for validation
    std::unordered_map<std::string, const anolis::deviceprovider::v1::ArgSpec*> spec_map;
    for (const auto& arg_spec : spec.args) {
        spec_map[arg_spec.name()] = &arg_spec;
    }

    // Check all required arguments are present
    for (const auto& arg_spec : spec.args) {
        if (arg_spec.required() && args.find(arg_spec.name()) == args.end()) {
            error = "Missing required argument: " + arg_spec.name();
            return false;
        }
    }

    // Validate provided arguments
    for (const auto& [arg_name, arg_value] : args) {
        // Check that argument is expected
        auto spec_it = spec_map.find(arg_name);
        if (spec_it == spec_map.end()) {
            error = "Unknown argument: " + arg_name;
            return false;
        }

        const auto& arg_spec = *spec_it->second;

        // Type validation
        if (!validate_argument_type(arg_spec, arg_value, error)) {
            std::string message;
            message.reserve(arg_name.size() + error.size() + 14);
            message.append("Argument '");
            message.append(arg_name);
            message.append("': ");
            message.append(error);
            error = std::move(message);
            return false;
        }

        // Range validation for numeric types
        if (!validate_argument_range(arg_spec, arg_value, error)) {
            std::string message;
            message.reserve(arg_name.size() + error.size() + 14);
            message.append("Argument '");
            message.append(arg_name);
            message.append("': ");
            message.append(error);
            error = std::move(message);
            return false;
        }
    }

    return true;
}

bool CallRouter::validate_argument_type(const anolis::deviceprovider::v1::ArgSpec& spec,
                                        const anolis::deviceprovider::v1::Value& value, std::string& error) const {
    using ValueType = anolis::deviceprovider::v1::ValueType;

    // Check that value type matches spec type
    bool type_match = false;
    switch (spec.type()) {
        case ValueType::VALUE_TYPE_BOOL:
            type_match = value.has_bool_value();
            break;
        case ValueType::VALUE_TYPE_INT64:
            type_match = value.has_int64_value();
            break;
        case ValueType::VALUE_TYPE_UINT64:
            type_match = value.has_uint64_value();
            break;
        case ValueType::VALUE_TYPE_DOUBLE:
            type_match = value.has_double_value();
            break;
        case ValueType::VALUE_TYPE_STRING:
            type_match = value.has_string_value();
            break;
        case ValueType::VALUE_TYPE_BYTES:
            type_match = value.has_bytes_value();
            break;
        default:
            error = "Unknown type in ArgSpec";
            return false;
    }

    if (!type_match) {
        error = "Type mismatch: expected " + value_type_to_string(spec.type()) + ", got " +
                value_type_to_string(value.type());
        return false;
    }

    return true;
}

bool CallRouter::validate_argument_range(const anolis::deviceprovider::v1::ArgSpec& spec,
                                         const anolis::deviceprovider::v1::Value& value, std::string& error) const {
    using ValueType = anolis::deviceprovider::v1::ValueType;

    switch (spec.type()) {
        case ValueType::VALUE_TYPE_DOUBLE: {
            double val = value.double_value();
            // Check if bounds are set (protobuf defaults to 0.0)
            // We use -inf/+inf to indicate unbounded
            bool has_min = spec.min_double() != 0.0 || spec.max_double() != 0.0;
            bool has_max = spec.min_double() != 0.0 || spec.max_double() != 0.0;

            if (has_min && val < spec.min_double()) {
                std::ostringstream oss;
                oss << "Value " << val << " below minimum " << spec.min_double();
                error = oss.str();
                return false;
            }
            if (has_max && val > spec.max_double()) {
                std::ostringstream oss;
                oss << "Value " << val << " above maximum " << spec.max_double();
                error = oss.str();
                return false;
            }
            break;
        }
        case ValueType::VALUE_TYPE_INT64: {
            int64_t val = value.int64_value();
            bool has_min = spec.min_int64() != 0 || spec.max_int64() != 0;
            bool has_max = spec.min_int64() != 0 || spec.max_int64() != 0;

            if (has_min && val < spec.min_int64()) {
                std::ostringstream oss;
                oss << "Value " << val << " below minimum " << spec.min_int64();
                error = oss.str();
                return false;
            }
            if (has_max && val > spec.max_int64()) {
                std::ostringstream oss;
                oss << "Value " << val << " above maximum " << spec.max_int64();
                error = oss.str();
                return false;
            }
            break;
        }
        case ValueType::VALUE_TYPE_UINT64: {
            uint64_t val = value.uint64_value();
            bool has_min = spec.min_uint64() != 0 || spec.max_uint64() != 0;
            bool has_max = spec.min_uint64() != 0 || spec.max_uint64() != 0;

            if (has_min && val < spec.min_uint64()) {
                std::ostringstream oss;
                oss << "Value " << val << " below minimum " << spec.min_uint64();
                error = oss.str();
                return false;
            }
            if (has_max && val > spec.max_uint64()) {
                std::ostringstream oss;
                oss << "Value " << val << " above maximum " << spec.max_uint64();
                error = oss.str();
                return false;
            }
            break;
        }
        default:
            // No range validation for bool, string, bytes
            break;
    }

    return true;
}

std::string CallRouter::value_type_to_string(anolis::deviceprovider::v1::ValueType type) const {
    using ValueType = anolis::deviceprovider::v1::ValueType;
    switch (type) {
        case ValueType::VALUE_TYPE_BOOL:
            return "bool";
        case ValueType::VALUE_TYPE_INT64:
            return "int64";
        case ValueType::VALUE_TYPE_UINT64:
            return "uint64";
        case ValueType::VALUE_TYPE_DOUBLE:
            return "double";
        case ValueType::VALUE_TYPE_STRING:
            return "string";
        case ValueType::VALUE_TYPE_BYTES:
            return "bytes";
        default:
            return "unknown";
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool CallRouter::parse_device_handle(const std::string& device_handle, std::string& provider_id, std::string& device_id,
                                     std::string& error) const {
    // Device handle format: "provider_id/device_id"
    auto slash_pos = device_handle.find('/');
    if (slash_pos == std::string::npos) {
        error = "Invalid device handle format (expected 'provider/device'): " + device_handle;
        return false;
    }

    provider_id = device_handle.substr(0, slash_pos);
    device_id = device_handle.substr(slash_pos + 1);

    if (provider_id.empty() || device_id.empty()) {
        error = "Invalid device handle (empty provider or device): " + device_handle;
        return false;
    }

    return true;
}

}  // namespace control
}  // namespace anolis
