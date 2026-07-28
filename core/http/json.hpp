#pragma once

#include <chrono>
#include <nlohmann/json.hpp>

#include "protocol.pb.h"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace http {

/**
 * @brief JSON encoding utilities for ADPP types
 *
 * All value types use lowercase JSON-idiomatic names:
 * - double, int64, uint64, bool, string, bytes (base64)
 *
 * This mirrors the ADPP Value union safely while avoiding
 * exposure of protobuf enum names.
 */

// Forward declarations
nlohmann::json encode_value(const anolis::deviceprovider::v1::Value& value);
nlohmann::json encode_signal_value(const state::CachedSignalValue& cached, const std::string& signal_id);
nlohmann::json encode_device_state(const state::DeviceState& state, const std::string& provider_id,
                                   const std::string& device_id);
nlohmann::json encode_device_info(const registry::RegisteredDevice& device);
nlohmann::json encode_capabilities(const registry::DeviceCapabilitySet& caps);
nlohmann::json encode_signal_spec(const registry::SignalSpec& spec);
nlohmann::json encode_function_spec(const registry::FunctionSpec& spec);

// Decode functions for incoming requests
bool decode_value(const nlohmann::json& json, anolis::deviceprovider::v1::Value& value, std::string& error);
bool decode_call_request(const nlohmann::json& json, std::string& provider_id, std::string& device_id,
                         uint32_t& function_id, std::map<std::string, anolis::deviceprovider::v1::Value>& args,
                         std::string& error);

// Quality conversions
std::string quality_to_string(anolis::deviceprovider::v1::SignalValue_Quality quality);
std::string value_type_to_string(anolis::deviceprovider::v1::ValueType type);
std::string function_category_to_string(anolis::deviceprovider::v1::FunctionPolicy_Category category);

}  // namespace http
}  // namespace anolis
