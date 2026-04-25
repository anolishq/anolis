#include "json.hpp"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>

namespace anolis {
namespace http {

// Base64 encoding for bytes
static const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static std::string base64_encode(const std::string &bytes) {
    std::string encoded;
    int val = 0;
    int bits = -6;

    for (unsigned char c : bytes) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back(base64_chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        encoded.push_back(base64_chars[((val << 8) >> (bits + 8)) & 0x3F]);
    }
    while ((encoded.size() % 4) != 0) {
        encoded.push_back('=');
    }
    return encoded;
}

static std::optional<std::string> base64_decode(const std::string &encoded) {
    std::string decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) {
        T[base64_chars[i]] = i;
    }

    int val = 0;
    int bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;                  // valid padding — stop
        if (T[c] == -1) return std::nullopt;  // illegal character — fail
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

std::string quality_to_string(anolis::deviceprovider::v1::SignalValue_Quality quality) {
    using Q = anolis::deviceprovider::v1::SignalValue_Quality;
    switch (quality) {
        case Q::SignalValue_Quality_QUALITY_OK:
            return "OK";
        case Q::SignalValue_Quality_QUALITY_STALE:
            return "STALE";
        case Q::SignalValue_Quality_QUALITY_UNKNOWN:
            return "UNAVAILABLE";
        case Q::SignalValue_Quality_QUALITY_FAULT:
            return "FAULT";
        default:
            return "UNAVAILABLE";
    }
}

std::string value_type_to_string(anolis::deviceprovider::v1::ValueType type) {
    using VT = anolis::deviceprovider::v1::ValueType;
    switch (type) {
        case VT::VALUE_TYPE_DOUBLE:
            return "double";
        case VT::VALUE_TYPE_INT64:
            return "int64";
        case VT::VALUE_TYPE_UINT64:
            return "uint64";
        case VT::VALUE_TYPE_BOOL:
            return "bool";
        case VT::VALUE_TYPE_STRING:
            return "string";
        case VT::VALUE_TYPE_BYTES:
            return "bytes";
        default:
            return "unknown";
    }
}

nlohmann::json encode_value(const anolis::deviceprovider::v1::Value &value) {
    nlohmann::json result;

    using VT = anolis::deviceprovider::v1::ValueType;
    switch (value.type()) {
        case VT::VALUE_TYPE_DOUBLE:
            result["type"] = "double";
            result["double"] = value.double_value();
            break;
        case VT::VALUE_TYPE_INT64:
            result["type"] = "int64";
            result["int64"] = value.int64_value();
            break;
        case VT::VALUE_TYPE_UINT64:
            result["type"] = "uint64";
            result["uint64"] = value.uint64_value();
            break;
        case VT::VALUE_TYPE_BOOL:
            result["type"] = "bool";
            result["bool"] = value.bool_value();
            break;
        case VT::VALUE_TYPE_STRING:
            result["type"] = "string";
            result["string"] = value.string_value();
            break;
        case VT::VALUE_TYPE_BYTES:
            result["type"] = "bytes";
            result["base64"] = base64_encode(value.bytes_value());
            break;
        default:
            result["type"] = "unknown";
            break;
    }
    return result;
}

nlohmann::json encode_signal_value(const state::CachedSignalValue &cached, const std::string &signal_id) {
    auto now = std::chrono::system_clock::now();
    auto timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(cached.timestamp.time_since_epoch()).count();
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - cached.timestamp).count();

    return {{"signal_id", signal_id},
            {"value", encode_value(cached.value)},
            {"timestamp_epoch_ms", timestamp_ms},
            {"quality", quality_to_string(cached.quality)},
            {"age_ms", age_ms}};
}

nlohmann::json encode_device_state(const state::DeviceState &state, const std::string &provider_id,
                                   const std::string &device_id) {
    nlohmann::json values = nlohmann::json::array();

    // Determine worst-case quality for device-level quality
    auto worst_quality = anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK;

    for (const auto &[signal_id, cached] : state.signals) {
        values.push_back(encode_signal_value(cached, signal_id));
        // Track worst quality
        if (cached.quality > worst_quality) {
            worst_quality = cached.quality;
        }
    }

    // If provider is unavailable, device quality is UNAVAILABLE
    std::string device_quality = state.provider_available ? quality_to_string(worst_quality) : "UNAVAILABLE";

    return {{"provider_id", provider_id}, {"device_id", device_id}, {"quality", device_quality}, {"values", values}};
}

nlohmann::json encode_device_info(const registry::RegisteredDevice &device) {
    // Extract display_name and type from proto if available
    std::string display_name = device.capabilities.proto.label();
    if (display_name.empty()) {
        display_name = device.device_id;
    }

    std::string type = device.capabilities.proto.type_id();
    if (type.empty()) {
        type = "unknown";
    }

    return {{"provider_id", device.provider_id},
            {"device_id", device.device_id},
            {"display_name", display_name},
            {"type", type}};
}

nlohmann::json encode_signal_spec(const registry::SignalSpec &spec) {
    // Note: unit is not currently in SignalSpec, would need to extend if needed
    return {{"signal_id", spec.signal_id}, {"value_type", value_type_to_string(spec.type)}, {"label", spec.label}};
}

nlohmann::json encode_function_spec(const registry::FunctionSpec &spec) {
    // Build args structure with full ArgSpec metadata
    nlohmann::json args = nlohmann::json::object();
    for (const auto &arg : spec.args) {
        nlohmann::json arg_info = {{"type", value_type_to_string(arg.type())}, {"required", arg.required()}};

        // Add description if present
        if (!arg.description().empty()) {
            arg_info["description"] = arg.description();
        }

        // Add unit if present
        if (!arg.unit().empty()) {
            arg_info["unit"] = arg.unit();
        }

        // Add range constraints for numeric types - emit each bound independently.
        if (arg.type() == anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE) {
            if (arg.has_min_double()) arg_info["min"] = arg.min_double();
            if (arg.has_max_double()) arg_info["max"] = arg.max_double();
        } else if (arg.type() == anolis::deviceprovider::v1::VALUE_TYPE_INT64) {
            if (arg.has_min_int64()) arg_info["min"] = arg.min_int64();
            if (arg.has_max_int64()) arg_info["max"] = arg.max_int64();
        } else if (arg.type() == anolis::deviceprovider::v1::VALUE_TYPE_UINT64) {
            if (arg.has_min_uint64()) arg_info["min"] = arg.min_uint64();
            if (arg.has_max_uint64()) arg_info["max"] = arg.max_uint64();
        }

        args[arg.name()] = arg_info;
    }

    return {{"function_id", spec.function_id}, {"name", spec.function_name}, {"label", spec.label}, {"args", args}};
}

nlohmann::json encode_capabilities(const registry::DeviceCapabilitySet &caps) {
    nlohmann::json signals = nlohmann::json::array();
    std::vector<std::reference_wrapper<const registry::SignalSpec>> sorted_signals;
    sorted_signals.reserve(caps.signals_by_id.size());
    for (const auto &[id, spec] : caps.signals_by_id) {
        (void)id;
        sorted_signals.emplace_back(std::cref(spec));
    }
    std::sort(sorted_signals.begin(), sorted_signals.end(),
              [](const auto &a, const auto &b) { return a.get().signal_id < b.get().signal_id; });
    for (const auto &spec : sorted_signals) {
        signals.push_back(encode_signal_spec(spec.get()));
    }

    nlohmann::json functions = nlohmann::json::array();
    std::vector<std::reference_wrapper<const registry::FunctionSpec>> sorted_functions;
    sorted_functions.reserve(caps.functions_by_id.size());
    for (const auto &[id, spec] : caps.functions_by_id) {
        (void)id;
        sorted_functions.emplace_back(std::cref(spec));
    }
    std::sort(sorted_functions.begin(), sorted_functions.end(), [](const auto &a, const auto &b) {
        if (a.get().function_id != b.get().function_id) {
            return a.get().function_id < b.get().function_id;
        }
        return a.get().function_name < b.get().function_name;
    });
    for (const auto &spec : sorted_functions) {
        functions.push_back(encode_function_spec(spec.get()));
    }

    return {{"signals", signals}, {"functions", functions}};
}

bool decode_value(const nlohmann::json &json, anolis::deviceprovider::v1::Value &value, std::string &error) {
    try {
        if (!json.contains("type")) {
            error = "Value missing 'type' field";
            return false;
        }

        std::string type = json.at("type").get<std::string>();

        using VT = anolis::deviceprovider::v1::ValueType;

        if (type == "double") {
            if (!json.contains("double")) {
                error = "Value type 'double' missing 'double' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_DOUBLE);
            value.set_double_value(json.at("double").get<double>());
        } else if (type == "int64") {
            if (!json.contains("int64")) {
                error = "Value type 'int64' missing 'int64' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_INT64);
            value.set_int64_value(json.at("int64").get<int64_t>());
        } else if (type == "uint64") {
            if (!json.contains("uint64")) {
                error = "Value type 'uint64' missing 'uint64' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_UINT64);
            value.set_uint64_value(json.at("uint64").get<uint64_t>());
        } else if (type == "bool") {
            if (!json.contains("bool")) {
                error = "Value type 'bool' missing 'bool' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_BOOL);
            value.set_bool_value(json.at("bool").get<bool>());
        } else if (type == "string") {
            if (!json.contains("string")) {
                error = "Value type 'string' missing 'string' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_STRING);
            value.set_string_value(json.at("string").get<std::string>());
        } else if (type == "bytes") {
            if (!json.contains("base64")) {
                error = "Value type 'bytes' missing 'base64' field";
                return false;
            }
            auto decoded = base64_decode(json.at("base64").get<std::string>());
            if (!decoded.has_value()) {
                error = "Invalid Base64 encoding in 'base64' field";
                return false;
            }
            value.set_type(VT::VALUE_TYPE_BYTES);
            value.set_bytes_value(decoded.value());
        } else {
            error = "Unknown value type: " + type;
            return false;
        }

        return true;
    } catch (const std::exception &e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

bool decode_call_request(const nlohmann::json &json, std::string &provider_id, std::string &device_id,
                         uint32_t &function_id, std::map<std::string, anolis::deviceprovider::v1::Value> &args,
                         std::string &error) {
    try {
        if (!json.contains("provider_id")) {
            error = "Missing 'provider_id'";
            return false;
        }
        if (!json.contains("device_id")) {
            error = "Missing 'device_id'";
            return false;
        }
        if (!json.contains("function_id")) {
            error = "Missing 'function_id'";
            return false;
        }

        provider_id = json.at("provider_id").get<std::string>();
        device_id = json.at("device_id").get<std::string>();
        function_id = json.at("function_id").get<uint32_t>();

        // Args is optional
        if (json.contains("args")) {
            const auto &args_json = json.at("args");
            if (!args_json.is_object()) {
                error = "'args' must be an object";
                return false;
            }

            for (const auto &[key, val] : args_json.items()) {
                anolis::deviceprovider::v1::Value value;
                if (!decode_value(val, value, error)) {
                    std::string message;
                    message.reserve(key.size() + error.size() + 23);
                    message.append("Invalid value for arg '");
                    message.append(key);
                    message.append("': ");
                    message.append(error);
                    error = std::move(message);
                    return false;
                }
                args[key] = value;
            }
        }

        return true;
    } catch (const std::exception &e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

}  // namespace http
}  // namespace anolis
