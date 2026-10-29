#include "registry/device_registry.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mocks/mock_provider_handle.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

namespace {

// A provider whose ListDevices answers with the given ids, each described with
// one double signal, or fails to describe when `describable` is false.
std::shared_ptr<NiceMock<MockProviderHandle>> provider_listing(const std::string &provider_id,
                                                               std::vector<std::string> ids, bool describable = true) {
    auto provider = std::make_shared<NiceMock<MockProviderHandle>>();
    provider->_id = provider_id;
    ON_CALL(*provider, provider_id()).WillByDefault(ReturnRef(provider->_id));
    ON_CALL(*provider, is_available()).WillByDefault(Return(true));
    ON_CALL(*provider, list_devices(_)).WillByDefault(Invoke([ids](std::vector<Device> &devices) {
        for (const auto &id : ids) {
            Device dev;
            dev.set_device_id(id);
            devices.push_back(dev);
        }
        return true;
    }));
    ON_CALL(*provider, describe_device(_, _))
        .WillByDefault(Invoke([describable](const std::string &id, DescribeDeviceResponse &response) {
            if (!describable) {
                return false;
            }
            response.mutable_device()->set_device_id(id);
            auto *sig = response.mutable_capabilities()->add_signals();
            sig->set_signal_id("temp");
            sig->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
            return true;
        }));
    return provider;
}

}  // namespace

// anolishq/anolis#461: a provider that answers and lists no devices (bread on
// an unpowered bus) is up with an empty inventory, not a failed discovery.
TEST(DeviceRegistryTest, AProviderThatListsNoDevicesDiscoversAnEmptyInventory) {
    registry::DeviceRegistry registry;
    auto sim = provider_listing("sim0", {"dev0"});
    auto bread = provider_listing("bread0", {});
    ASSERT_TRUE(registry.discover_provider("sim0", *sim));

    EXPECT_TRUE(registry.discover_provider("bread0", *bread)) << registry.last_error();
    EXPECT_TRUE(registry.get_devices_for_provider("bread0").empty());
    EXPECT_EQ(registry.get_devices_for_provider("sim0").size(), 1u) << "another provider's devices were touched";
}

// The restart path inspects and then commits with replace: a provider that
// comes back listing nothing replaces its old inventory with none.
TEST(DeviceRegistryTest, ARestartThatListsNoDevicesReplacesTheInventoryWithNone) {
    registry::DeviceRegistry registry;
    auto before = provider_listing("bread0", {"rlht0", "dcmt0"});
    ASSERT_TRUE(registry.discover_provider("bread0", *before));
    ASSERT_EQ(registry.get_devices_for_provider("bread0").size(), 2u);

    auto after = provider_listing("bread0", {});
    std::vector<registry::RegisteredDevice> replacement;
    ASSERT_TRUE(registry.inspect_provider_devices("bread0", *after, replacement)) << registry.last_error();
    EXPECT_TRUE(replacement.empty());
    registry.commit_provider_devices("bread0", std::move(replacement), true);
    EXPECT_TRUE(registry.get_devices_for_provider("bread0").empty());
}

TEST(DeviceRegistryTest, AFailedListDevicesStillFailsDiscovery) {
    registry::DeviceRegistry registry;
    auto provider = provider_listing("bread0", {});
    ON_CALL(*provider, list_devices(_)).WillByDefault(Return(false));
    EXPECT_FALSE(registry.discover_provider("bread0", *provider));
}

// Listed devices none of which describe leave the provider up with none, as
// one surviving device leaves it up with one (anolishq/anolis#461).
TEST(DeviceRegistryTest, ListedDevicesNoneOfWhichDescribeLeaveAnEmptyInventory) {
    registry::DeviceRegistry registry;
    auto provider = provider_listing("bread0", {"rlht0"}, false);
    EXPECT_TRUE(registry.discover_provider("bread0", *provider)) << registry.last_error();
    EXPECT_TRUE(registry.get_devices_for_provider("bread0").empty());
}

// #566: v1 implements no leases. A function that declares requires_lease is
// discovered, and a WARN names it, so it is not silently treated as leased.
TEST(DeviceRegistryTest, AFunctionThatRequiresALeaseIsWarnedAboutAtDiscovery) {
    registry::DeviceRegistry registry;
    auto provider = provider_listing("sim0", {"dev0"});
    ON_CALL(*provider, describe_device(_, _))
        .WillByDefault(Invoke([](const std::string &id, DescribeDeviceResponse &response) {
            response.mutable_device()->set_device_id(id);
            auto *fn = response.mutable_capabilities()->add_functions();
            fn->set_name("take_over");
            fn->set_function_id(1);
            fn->mutable_policy()->set_requires_lease(true);
            return true;
        }));

    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    const bool ok = registry.discover_provider("sim0", *provider);
    std::cerr.rdbuf(old);

    ASSERT_TRUE(ok) << registry.last_error();
    EXPECT_THAT(captured.str(), HasSubstr("sim0/dev0/take_over declares requires_lease"));
    EXPECT_THAT(captured.str(), HasSubstr("[WARN]"));
}
