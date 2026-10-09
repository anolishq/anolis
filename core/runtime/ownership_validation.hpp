#pragma once

/**
 * @file ownership_validation.hpp
 * @brief Resource ownership validation over opaque device claims.
 *
 * A device lists the resources it needs exclusively in its `anolis.claim`
 * descriptor tag, as space-separated keys. The runtime requires every key to
 * have one owner across the whole inventory. It compares exact strings and
 * parses nothing, so it knows no transport: a provider that shares a resource
 * with another (an I2C address on one bus) spells the key the way its SDK
 * helper does, and that is what makes the collision visible here.
 */

#include <string>
#include <vector>

#include "registry/device_registry.hpp"

namespace anolis {
namespace runtime {

/**
 * @brief Validate that no two published devices hold the same claim key.
 *
 * Devices without an `anolis.claim` tag claim nothing and are not checked.
 */
bool validate_ownership_claims(const std::vector<registry::RegisteredDevice> &devices, std::string &error);

/**
 * @brief Re-run ownership validation using a provider replacement candidate.
 *
 * This is used during provider restart to validate the replacement inventory
 * against the currently published devices from every other provider before the
 * swap is committed.
 */
bool validate_ownership_claims_after_provider_replacement(
    const std::vector<registry::RegisteredDevice> &current_devices, const std::string &provider_id,
    const std::vector<registry::RegisteredDevice> &replacement_devices, std::string &error);

/**
 * @brief Providers whose devices still carry the pre-claim ownership tags
 * (`hw.bus_path`, `hw.i2c_address`, `bus_path`, `i2c_address`) and no claim.
 *
 * The runtime does not read those tags, so these devices' ownership is not
 * checked; the caller warns, once per provider, so the gap is visible.
 */
std::vector<std::string> providers_with_unchecked_legacy_ownership(
    const std::vector<registry::RegisteredDevice> &devices);

}  // namespace runtime
}  // namespace anolis
