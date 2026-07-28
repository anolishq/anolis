#pragma once

/**
 * @file provider_value_bridge.hpp
 * @brief Convert a control-plane ParameterValue into a provider protobuf Value.
 *
 * Single source of truth shared by mode-transition hooks and the estop
 * safe-state ladder, so both build provider call arguments identically.
 */

#include <variant>

#include "automation/parameter_types.hpp"
#include "protocol.pb.h"

namespace anolis {
namespace control {

inline anolis::deviceprovider::v1::Value to_provider_value(const automation::ParameterValue &input) {
    anolis::deviceprovider::v1::Value value;
    if (std::holds_alternative<double>(input)) {
        value.set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
        value.set_double_value(std::get<double>(input));
    } else if (std::holds_alternative<int64_t>(input)) {
        value.set_type(anolis::deviceprovider::v1::VALUE_TYPE_INT64);
        value.set_int64_value(std::get<int64_t>(input));
    } else if (std::holds_alternative<bool>(input)) {
        value.set_type(anolis::deviceprovider::v1::VALUE_TYPE_BOOL);
        value.set_bool_value(std::get<bool>(input));
    } else if (std::holds_alternative<std::string>(input)) {
        value.set_type(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
        value.set_string_value(std::get<std::string>(input));
    }
    return value;
}

}  // namespace control
}  // namespace anolis
