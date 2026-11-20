#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace anolis {
namespace automation {

/**
 * @brief Parameter types supported by automation layer.
 *
 * This is the canonical parameter type model shared by config parsing,
 * parameter manager runtime state, and HTTP control APIs.
 */
enum class ParameterType { DOUBLE, INT64, BOOL, STRING };

/**
 * @brief Canonical value type for automation parameters.
 */
using ParameterValue = std::variant<double, int64_t, bool, std::string>;

/**
 * @brief Convert ParameterType to configuration/API string.
 */
inline const char *parameter_type_to_string(ParameterType type) {
    switch (type) {
        case ParameterType::DOUBLE:
            return "double";
        case ParameterType::INT64:
            return "int64";
        case ParameterType::BOOL:
            return "bool";
        case ParameterType::STRING:
            return "string";
        default:
            return "unknown";
    }
}

/**
 * @brief Parse configuration/API string into ParameterType.
 */
inline std::optional<ParameterType> parameter_type_from_string(const std::string_view type_name) {
    if (type_name == "double") {
        return ParameterType::DOUBLE;
    }
    if (type_name == "int64") {
        return ParameterType::INT64;
    }
    if (type_name == "bool") {
        return ParameterType::BOOL;
    }
    if (type_name == "string") {
        return ParameterType::STRING;
    }
    return std::nullopt;
}

/**
 * @brief Get runtime type name for a ParameterValue instance.
 */
inline const char *parameter_value_type_name(const ParameterValue &value) {
    if (std::holds_alternative<double>(value)) return "double";
    if (std::holds_alternative<int64_t>(value)) return "int64";
    if (std::holds_alternative<bool>(value)) return "bool";
    if (std::holds_alternative<std::string>(value)) return "string";
    return "unknown";
}

/**
 * @brief Check whether a ParameterValue matches an expected ParameterType.
 */
inline bool parameter_value_matches_type(ParameterType type, const ParameterValue &value) {
    switch (type) {
        case ParameterType::DOUBLE:
            return std::holds_alternative<double>(value);
        case ParameterType::INT64:
            return std::holds_alternative<int64_t>(value);
        case ParameterType::BOOL:
            return std::holds_alternative<bool>(value);
        case ParameterType::STRING:
            return std::holds_alternative<std::string>(value);
        default:
            return false;
    }
}

/**
 * @brief Convert a ParameterValue to a deterministic string representation.
 */
inline std::string parameter_value_to_string(const ParameterValue &value) {
    if (std::holds_alternative<double>(value)) {
        // Shortest round-trip form, as validation errors use, so different values
        // never print the same (std::to_string's %f gave 0.000000 for 1e-8; #756).
        return std::format("{}", std::get<double>(value));
    }
    if (std::holds_alternative<int64_t>(value)) {
        return std::to_string(std::get<int64_t>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    }
    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    }
    return {};
}

}  // namespace automation
}  // namespace anolis
