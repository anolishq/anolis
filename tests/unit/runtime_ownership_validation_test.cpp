#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "runtime/ownership_validation.hpp"

namespace {

anolis::registry::RegisteredDevice make_device(const std::string &provider_id, const std::string &device_id,
                                               const std::vector<std::pair<std::string, std::string>> &tags) {
    anolis::registry::RegisteredDevice device;
    device.provider_id = provider_id;
    device.device_id = device_id;
    device.capabilities.proto.set_device_id(device_id);

    auto *proto_tags = device.capabilities.proto.mutable_tags();
    for (const auto &entry : tags) {
        (*proto_tags)[entry.first] = entry.second;
    }

    return device;
}

anolis::registry::RegisteredDevice claiming(const std::string &provider_id, const std::string &device_id,
                                            const std::string &claim) {
    return make_device(provider_id, device_id, {{"anolis.claim", claim}});
}

}  // namespace

TEST(RuntimeOwnershipValidationTest, AllowsDevicesWithoutClaims) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {make_device("sim", "temp0", {})};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims(devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, AllowsUniqueClaims) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x08"),
                                                                     claiming("ezo", "ph_0", "i2c:/dev/i2c-1:0x63"),
                                                                     claiming("ezo", "ph_1", "i2c:/dev/i2c-2:0x63")};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims(devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, RejectsDuplicateClaimAcrossProviders) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x61"),
                                                                     claiming("ezo", "do_0", "i2c:/dev/i2c-1:0x61")};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_ownership_claims(devices, error));
    EXPECT_NE(error.find("bread/dcmt_0"), std::string::npos);
    EXPECT_NE(error.find("ezo/do_0"), std::string::npos);
    EXPECT_NE(error.find("'i2c:/dev/i2c-1:0x61'"), std::string::npos);
}

TEST(RuntimeOwnershipValidationTest, ComparesKeysAsExactStrings) {
    // The runtime parses nothing: spelling the same address differently is a
    // different key. Providers agree by using the SDK's claim_key.
    const std::vector<anolis::registry::RegisteredDevice> devices = {claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x61"),
                                                                     claiming("ezo", "do_0", "i2c:/dev/i2c-1:0X61")};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims(devices, error));
}

TEST(RuntimeOwnershipValidationTest, ChecksEveryKeyOfAMultiKeyClaim) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x14 gpio:17"), claiming("other", "relay_0", "gpio:17")};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_ownership_claims(devices, error));
    EXPECT_NE(error.find("'gpio:17'"), std::string::npos);
    EXPECT_EQ(error.find("i2c:/dev/i2c-1:0x14"), std::string::npos);  // that key is unique
}

TEST(RuntimeOwnershipValidationTest, AKeyRepeatedWithinOneDeviceIsNotAConflict) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x14  i2c:/dev/i2c-1:0x14")};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims(devices, error));
}

TEST(RuntimeOwnershipValidationTest, IgnoresTheOldOwnershipTags) {
    // No fallback: hw.* tags are not read, so a duplicate expressed only with
    // them is not caught. providers_with_unchecked_legacy_ownership names it.
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("old_bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x61"}}),
        make_device("old_ezo", "do_0", {{"bus_path", "/dev/i2c-1"}, {"i2c_address", "0x61"}}),
        claiming("ezo", "ph_0", "i2c:/dev/i2c-1:0x63"), make_device("sim", "temp0", {})};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims(devices, error));
    EXPECT_EQ(anolis::runtime::providers_with_unchecked_legacy_ownership(devices),
              (std::vector<std::string>{"old_bread", "old_ezo"}));
}

TEST(RuntimeOwnershipValidationTest, AClaimSilencesTheLegacyWarning) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("bread", "dcmt_0", {{"anolis.claim", "i2c:/dev/i2c-1:0x14"}, {"hw.bus_path", "/dev/i2c-1"}})};

    EXPECT_TRUE(anolis::runtime::providers_with_unchecked_legacy_ownership(devices).empty());
}

TEST(RuntimeOwnershipValidationTest, AllowsProviderReplacementWhenClaimsRemainUnique) {
    const std::vector<anolis::registry::RegisteredDevice> current_devices = {
        claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x08"), claiming("ezo", "ph_0", "i2c:/dev/i2c-1:0x63")};

    const std::vector<anolis::registry::RegisteredDevice> replacement_devices = {
        claiming("ezo", "ph_0", "i2c:/dev/i2c-1:0x63"), claiming("ezo", "orp_0", "i2c:/dev/i2c-2:0x62")};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_ownership_claims_after_provider_replacement(current_devices, "ezo",
                                                                                      replacement_devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, RejectsProviderReplacementWithDuplicateClaim) {
    const std::vector<anolis::registry::RegisteredDevice> current_devices = {
        claiming("bread", "dcmt_0", "i2c:/dev/i2c-1:0x61"), claiming("ezo", "ph_0", "i2c:/dev/i2c-1:0x63")};

    const std::vector<anolis::registry::RegisteredDevice> replacement_devices = {
        claiming("ezo", "do_0", "i2c:/dev/i2c-1:0x61")};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_ownership_claims_after_provider_replacement(current_devices, "ezo",
                                                                                       replacement_devices, error));
    EXPECT_NE(error.find("Restart-time ownership validation failed for provider 'ezo'"), std::string::npos);
    EXPECT_NE(error.find("bread/dcmt_0"), std::string::npos);
    EXPECT_NE(error.find("ezo/do_0"), std::string::npos);
}
