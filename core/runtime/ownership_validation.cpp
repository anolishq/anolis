/**
 * @file ownership_validation.cpp
 * @brief Uniqueness of opaque `anolis.claim` keys across discovered devices.
 *
 * The validator operates on published inventory snapshots, not on provider
 * config files directly, so it enforces the claims providers actually surface.
 */

#include "ownership_validation.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace anolis {
namespace runtime {
namespace {

constexpr const char *kClaimTag = "anolis.claim";
constexpr std::array<const char *, 4> kLegacyOwnershipTags = {"hw.bus_path", "hw.i2c_address", "bus_path",
                                                              "i2c_address"};

// The distinct keys of a device's claim tag, split on whitespace.
std::set<std::string> claim_keys(const registry::RegisteredDevice &device) {
    std::set<std::string> keys;
    const auto &tags = device.capabilities.proto.tags();
    const auto it = tags.find(kClaimTag);
    if (it == tags.end()) {
        return keys;
    }
    std::istringstream in(it->second);
    std::string key;
    while (in >> key) {
        keys.insert(key);
    }
    return keys;
}

}  // namespace

bool validate_ownership_claims(const std::vector<registry::RegisteredDevice> &devices, std::string &error) {
    // key -> sorted "provider/device" owners
    std::map<std::string, std::set<std::string>> owners_by_key;
    for (const auto &device : devices) {
        for (const auto &key : claim_keys(device)) {
            owners_by_key[key].insert(device.provider_id + "/" + device.device_id);
        }
    }

    std::vector<std::string> conflicts;
    for (const auto &[key, owners] : owners_by_key) {
        if (owners.size() < 2) {
            continue;
        }
        std::string line = std::format("claim '{}' held by ", key);
        bool first = true;
        for (const auto &owner : owners) {
            line += first ? owner : ", " + owner;
            first = false;
        }
        conflicts.push_back(std::move(line));
    }

    if (conflicts.empty()) {
        return true;
    }

    std::string out = "Ownership validation failed";
    for (const auto &conflict : conflicts) {
        out += "; " + conflict;
    }
    out += ". Each anolis.claim key must belong to exactly one device.";
    error = std::move(out);
    return false;
}

bool validate_ownership_claims_after_provider_replacement(
    const std::vector<registry::RegisteredDevice> &current_devices, const std::string &provider_id,
    const std::vector<registry::RegisteredDevice> &replacement_devices, std::string &error) {
    std::vector<registry::RegisteredDevice> candidate_devices;
    candidate_devices.reserve(current_devices.size() + replacement_devices.size());

    // Build the hypothetical post-restart inventory first, then validate it as
    // a whole. This prevents partial replacement state from leaking into the
    // live registry before ownership checks pass.
    for (const auto &device : current_devices) {
        if (device.provider_id != provider_id) {
            candidate_devices.push_back(device);
        }
    }

    candidate_devices.insert(candidate_devices.end(), replacement_devices.begin(), replacement_devices.end());

    std::string ownership_error;
    if (validate_ownership_claims(candidate_devices, ownership_error)) {
        return true;
    }

    error = "Restart-time ownership validation failed for provider '" + provider_id + "': " + ownership_error;
    return false;
}

std::vector<std::string> providers_with_unchecked_legacy_ownership(
    const std::vector<registry::RegisteredDevice> &devices) {
    std::set<std::string> providers;
    for (const auto &device : devices) {
        const auto &tags = device.capabilities.proto.tags();
        if (tags.count(kClaimTag) != 0U) {
            continue;
        }
        const bool legacy = std::any_of(kLegacyOwnershipTags.begin(), kLegacyOwnershipTags.end(),
                                        [&tags](const char *tag) { return tags.count(tag) != 0U; });
        if (legacy) {
            providers.insert(device.provider_id);
        }
    }
    return {providers.begin(), providers.end()};
}

}  // namespace runtime
}  // namespace anolis
