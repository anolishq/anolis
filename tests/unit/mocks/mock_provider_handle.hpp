#pragma once
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "protocol.pb.h"
#include "provider/i_provider_handle.hpp"

namespace anolis::tests {

using namespace anolis;
using namespace testing;
using ProtoValue = anolis::deviceprovider::v1::Value;
using Device = anolis::deviceprovider::v1::Device;
using DescribeDeviceResponse = anolis::deviceprovider::v1::DescribeDeviceResponse;
using ReadSignalsResponse = anolis::deviceprovider::v1::ReadSignalsResponse;
using CallResponse = anolis::deviceprovider::v1::CallResponse;
using GetHealthResponse = anolis::deviceprovider::v1::GetHealthResponse;
using ValueMap = std::map<std::string, ProtoValue>;

class MockProviderHandle : public provider::IProviderHandle {
public:
    MOCK_METHOD(bool, start, (), (override));
    MOCK_METHOD(bool, is_available, (), (const, override));

    MOCK_METHOD(bool, hello, (anolis::deviceprovider::v1::HelloResponse &), (override));
    MOCK_METHOD(bool, list_devices, (std::vector<Device> &), (override));
    MOCK_METHOD(bool, describe_device, (const std::string &, DescribeDeviceResponse &), (override));
    MOCK_METHOD(bool, read_signals, (const std::string &, const std::vector<std::string> &, ReadSignalsResponse &),
                (override));
    MOCK_METHOD(bool, call, (const std::string &, uint32_t, const std::string &, const ValueMap &, CallResponse &),
                (override));
    MOCK_METHOD(bool, get_health, (GetHealthResponse &), (override));

    MOCK_METHOD(std::string, last_error, (), (const, override));
    MOCK_METHOD(anolis::deviceprovider::v1::Status_Code, last_status_code, (), (const, override));
    MOCK_METHOD(const std::string &, provider_id, (), (const, override));

    // Helper to store/return ID reference
    std::string _id = "sim0";
    std::string _err = "";
};

}  // namespace anolis::tests
