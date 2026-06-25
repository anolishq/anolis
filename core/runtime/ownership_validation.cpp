/**
 * @file ownership_validation.cpp
 * @brief Validation of canonical I2C bus/address ownership tags across discovered devices.
 *
 * The validator operates on published inventory snapshots, not on provider
 * config files directly, so it enforces the ownership contract actually
 * surfaced by provider discovery.
 */

#include "ownership_validation.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace anolis {
namespace runtime {
namespace {

constexpr const char *kBusPathTag = "hw.bus_path";
constexpr const char *kI2cAddressTag = "hw.i2c_address";
constexpr const char *kLegacyBusPathTag = "bus_path";
constexpr const char *kLegacyI2cAddressTag = "i2c_address";

struct OwnershipClaim {
    std::string provider_id;
    std::string device_id;
    std::string bus_path;
    std::string i2c_address;
};

std::string trim_copy(const std::string &input) {
    size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])) != 0) {
        ++first;
    }

    size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) {
        --last;
    }

    return input.substr(first, last - first);
}

bool parse_i2c_address(const std::string &raw_input, std::string &canonical) {
    std::string text = trim_copy(raw_input);
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text = text.substr(2);
    }

    if (text.empty()) {
        return false;
    }

    unsigned int value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto parse_result = std::from_chars(begin, end, value, 16);
    if (parse_result.ec != std::errc() || parse_result.ptr != end || value > 0x7FU) {
        return false;
    }

    std::ostringstream out;
    out << "0x" << std::nouppercase << std::hex << std::setw(2) << std::setfill('0') << value;
    canonical = out.str();
    return true;
}

bool ownership_claim_less(const OwnershipClaim &lhs, const OwnershipClaim &rhs) {
    if (lhs.provider_id != rhs.provider_id) {
        return lhs.provider_id < rhs.provider_id;
    }
    return lhs.device_id < rhs.device_id;
}

}  // namespace

bool validate_i2c_ownership_claims(const std::vector<registry::RegisteredDevice> &devices, std::string &error) {
    std::map<std::string, std::vector<OwnershipClaim>> claims_by_key;

    for (const auto &device : devices) {
        const auto &tags = device.capabilities.proto.tags();
        const auto bus_it = tags.find(kBusPathTag);
        const auto address_it = tags.find(kI2cAddressTag);

        const bool has_bus_path = bus_it != tags.end();
        const bool has_i2c_address = address_it != tags.end();
        const bool has_legacy_bus_path = tags.find(kLegacyBusPathTag) != tags.end();
        const bool has_legacy_i2c_address = tags.find(kLegacyI2cAddressTag) != tags.end();

        const std::string handle = device.get_handle();

        // Devices that do not claim shared-bus ownership are ignored; devices
        // that partially claim it are rejected because the runtime cannot make
        // a safe uniqueness decision from incomplete tags.
        if (!has_bus_path && !has_i2c_address) {
            if (has_legacy_bus_path || has_legacy_i2c_address) {
                error = std::format(
                    "Device '{}' uses legacy ownership tags ('{}', '{}') without canonical tags "
                    "('{}', '{}')",
                    handle, kLegacyBusPathTag, kLegacyI2cAddressTag, kBusPathTag, kI2cAddressTag);
                return false;
            }
            continue;
        }

        if (has_bus_path != has_i2c_address) {
            error = std::format(
                "Device '{}' has incomplete ownership tags; both '{}' and '{}' are required when "
                "either is present",
                handle, kBusPathTag, kI2cAddressTag);
            return false;
        }

        const std::string bus_path = trim_copy(bus_it->second);
        if (bus_path.empty()) {
            error = std::format("Device '{}' has empty tag '{}'", handle, kBusPathTag);
            return false;
        }

        std::string canonical_address;
        if (!parse_i2c_address(address_it->second, canonical_address)) {
            error =
                std::format("Device '{}' has invalid tag '{}' value '{}'", handle, kI2cAddressTag, address_it->second);
            return false;
        }

        // Validation keys are normalized to trimmed bus path plus canonical
        // lowercase hex address so providers cannot evade collisions by format.
        const std::string key = std::format("{}|{}", bus_path, canonical_address);
        claims_by_key[key].push_back(OwnershipClaim{device.provider_id, device.device_id, bus_path, canonical_address});
    }

    std::vector<std::string> conflicts;
    for (const auto &entry : claims_by_key) {
        const auto &claims = entry.second;
        if (claims.size() < 2) {
            continue;
        }

        std::vector<OwnershipClaim> sorted_claims = claims;
        std::sort(sorted_claims.begin(), sorted_claims.end(), ownership_claim_less);

        std::ostringstream line;
        line << "duplicate ownership for bus='" << sorted_claims.front().bus_path << "' addr='"
             << sorted_claims.front().i2c_address << "' claimed by ";

        for (size_t i = 0; i < sorted_claims.size(); ++i) {
            if (i > 0) {
                line << ", ";
            }
            line << sorted_claims[i].provider_id << "/" << sorted_claims[i].device_id;
        }

        conflicts.push_back(line.str());
    }

    if (conflicts.empty()) {
        return true;
    }

    std::ostringstream out;
    out << "I2C ownership validation failed";
    for (const auto &conflict : conflicts) {
        out << "; " << conflict;
    }
    out << ". Ensure each provider/device owns a unique (hw.bus_path, hw.i2c_address) pair.";

    error = out.str();
    return false;
}

bool validate_i2c_ownership_claims_after_provider_replacement(
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
    if (validate_i2c_ownership_claims(candidate_devices, ownership_error)) {
        return true;
    }

    error = "Restart-time ownership validation failed for provider '" + provider_id + "': " + ownership_error;
    return false;
}

}  // namespace runtime
}  // namespace anolis
