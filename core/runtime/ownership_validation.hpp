#pragma once

#include <string>
#include <vector>

#include "registry/device_registry.hpp"

namespace anolis {
namespace runtime {

bool validate_i2c_ownership_claims(const std::vector<registry::RegisteredDevice> &devices, std::string &error);
bool validate_i2c_ownership_claims_after_provider_replacement(
    const std::vector<registry::RegisteredDevice> &current_devices, const std::string &provider_id,
    const std::vector<registry::RegisteredDevice> &replacement_devices, std::string &error);

}  // namespace runtime
}  // namespace anolis
