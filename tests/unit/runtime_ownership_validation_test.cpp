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

}  // namespace

TEST(RuntimeOwnershipValidationTest, AllowsDevicesWithoutOwnershipTags) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {make_device("sim", "temp0", {})};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, AllowsUniqueOwnershipClaims) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x08"}}),
        make_device("ezo", "ph_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x63"}}),
        make_device("ezo", "ph_1", {{"hw.bus_path", "/dev/i2c-2"}, {"hw.i2c_address", "0x63"}})};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, RejectsDuplicateOwnershipAcrossProviders) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x61"}}),
        make_device("ezo", "do_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0X61"}})};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_NE(error.find("bread/dcmt_0"), std::string::npos);
    EXPECT_NE(error.find("ezo/do_0"), std::string::npos);
    EXPECT_NE(error.find("0x61"), std::string::npos);
}

TEST(RuntimeOwnershipValidationTest, RejectsIncompleteOwnershipTags) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}})};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_NE(error.find("incomplete ownership tags"), std::string::npos);
}

TEST(RuntimeOwnershipValidationTest, RejectsLegacyOnlyOwnershipTags) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("legacy", "device_0", {{"bus_path", "/dev/i2c-1"}, {"i2c_address", "0x61"}})};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_NE(error.find("uses legacy ownership tags"), std::string::npos);
}

TEST(RuntimeOwnershipValidationTest, RejectsInvalidI2cAddressTagValue) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        make_device("ezo", "ph_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "not-a-hex-address"}})};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_i2c_ownership_claims(devices, error));
    EXPECT_NE(error.find("invalid tag 'hw.i2c_address'"), std::string::npos);
}

TEST(RuntimeOwnershipValidationTest, AllowsProviderReplacementWhenOwnershipRemainsUnique) {
    const std::vector<anolis::registry::RegisteredDevice> current_devices = {
        make_device("bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x08"}}),
        make_device("ezo", "ph_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x63"}})};

    const std::vector<anolis::registry::RegisteredDevice> replacement_devices = {
        make_device("ezo", "ph_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x63"}}),
        make_device("ezo", "orp_0", {{"hw.bus_path", "/dev/i2c-2"}, {"hw.i2c_address", "0x62"}})};

    std::string error;
    EXPECT_TRUE(anolis::runtime::validate_i2c_ownership_claims_after_provider_replacement(current_devices, "ezo",
                                                                                          replacement_devices, error));
    EXPECT_TRUE(error.empty());
}

TEST(RuntimeOwnershipValidationTest, RejectsProviderReplacementWithDuplicateOwnership) {
    const std::vector<anolis::registry::RegisteredDevice> current_devices = {
        make_device("bread", "dcmt_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x61"}}),
        make_device("ezo", "ph_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x63"}})};

    const std::vector<anolis::registry::RegisteredDevice> replacement_devices = {
        make_device("ezo", "do_0", {{"hw.bus_path", "/dev/i2c-1"}, {"hw.i2c_address", "0x61"}})};

    std::string error;
    EXPECT_FALSE(anolis::runtime::validate_i2c_ownership_claims_after_provider_replacement(current_devices, "ezo",
                                                                                           replacement_devices, error));
    EXPECT_NE(error.find("Restart-time ownership validation failed for provider 'ezo'"), std::string::npos);
    EXPECT_NE(error.find("bread/dcmt_0"), std::string::npos);
    EXPECT_NE(error.find("ezo/do_0"), std::string::npos);
}
