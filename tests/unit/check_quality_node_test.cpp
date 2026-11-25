// CheckQuality's expected_quality vocabulary (#797): UNAVAILABLE, the name
// HTTP and events give an unknown quality, matches it; a name that is not a
// quality refuses the tree when it is built instead of never matching.

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "automation/bt_nodes.hpp"
#include "automation/bt_services.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

using Quality = anolis::deviceprovider::v1::SignalValue_Quality;

class CheckQualityNodeTest : public ::testing::Test {
protected:
    // A device whose one signal, `temp`, is polled once with `quality`.
    void poll_with(Quality quality) {
        using anolis::deviceprovider::v1::DescribeDeviceResponse;
        using anolis::deviceprovider::v1::Device;
        using anolis::deviceprovider::v1::ReadSignalsResponse;
        using ::testing::_;
        using ::testing::Invoke;
        using ::testing::Return;
        using ::testing::ReturnRef;

        mock_provider_ = std::make_shared<::testing::NiceMock<anolis::tests::MockProviderHandle>>();
        ON_CALL(*mock_provider_, provider_id()).WillByDefault(ReturnRef(mock_provider_->_id));
        ON_CALL(*mock_provider_, is_available()).WillByDefault(Return(true));
        ON_CALL(*mock_provider_, get_health(_)).WillByDefault(Return(false));
        ON_CALL(*mock_provider_, list_devices(_)).WillByDefault(Invoke([](std::vector<Device> &devices) {
            Device dev;
            dev.set_device_id("dev0");
            devices.push_back(dev);
            return true;
        }));
        ON_CALL(*mock_provider_, describe_device(_, _))
            .WillByDefault(Invoke([](const std::string &id, DescribeDeviceResponse &response) {
                response.mutable_device()->set_device_id(id);
                auto *temp = response.mutable_capabilities()->add_signals();
                temp->set_signal_id("temp");
                temp->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                temp->set_poll_hint_hz(1.0);
                return true;
            }));
        ON_CALL(*mock_provider_, read_signals(_, _, _, _))
            .WillByDefault(Invoke([quality](const std::string &, const std::vector<std::string> &,
                                            ReadSignalsResponse &response, anolis::deviceprovider::v1::Status_Code &) {
                auto *temp = response.add_values();
                temp->set_signal_id("temp");
                temp->set_quality(quality);
                temp->mutable_value()->set_double_value(21.0);
                return true;
            }));
        provider_registry_.add_provider("sim0", mock_provider_);
        registry_.discover_provider("sim0", *mock_provider_);
        state_cache_ = std::make_unique<anolis::state::StateCache>(registry_, 500);
        ASSERT_TRUE(state_cache_->initialize());
        state_cache_->poll_once(provider_registry_);
    }

    BT::Tree build(const std::string &expected_quality) {
        factory_.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality");
        auto blackboard = BT::Blackboard::create();
        anolis::automation::BTServiceContext services;
        services.state_cache = state_cache_.get();
        blackboard->set(anolis::automation::kBTServiceContextKey, services);
        blackboard->set("wanted", std::string("Ok"));
        const std::string xml = R"(<root BTCPP_format="4"><BehaviorTree ID="Main">
<CheckQuality device_handle="sim0/dev0" signal_id="temp" expected_quality=")" +
                                expected_quality + R"("/>
</BehaviorTree></root>)";
        return factory_.createTreeFromText(xml, blackboard);
    }

    BT::BehaviorTreeFactory factory_;
    anolis::registry::DeviceRegistry registry_;
    anolis::provider::ProviderRegistry provider_registry_;
    std::shared_ptr<::testing::NiceMock<anolis::tests::MockProviderHandle>> mock_provider_;
    std::unique_ptr<anolis::state::StateCache> state_cache_;
};

struct QualityCase {
    Quality cached;
    const char *expected_quality;
};

class CheckQualityMatchesTest : public CheckQualityNodeTest, public ::testing::WithParamInterface<QualityCase> {};

TEST_P(CheckQualityMatchesTest, MatchesTheCachedQualityByItsName) {
    poll_with(GetParam().cached);
    auto tree = build(GetParam().expected_quality);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
}

INSTANTIATE_TEST_SUITE_P(
    EachQuality, CheckQualityMatchesTest,
    ::testing::Values(QualityCase{anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK, "OK"},
                      QualityCase{anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE, "STALE"},
                      QualityCase{anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_FAULT, "FAULT"},
                      QualityCase{anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_UNKNOWN, "UNAVAILABLE"},
                      QualityCase{anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_UNKNOWN, "UNKNOWN"}),
    [](const ::testing::TestParamInfo<QualityCase> &info) { return std::string(info.param.expected_quality); });

TEST_F(CheckQualityNodeTest, DoesNotMatchAnotherQuality) {
    poll_with(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE);
    auto tree = build("OK");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}

TEST_F(CheckQualityNodeTest, ANameThatIsNotAQualityRefusesTheTree) {
    poll_with(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
    EXPECT_THROW((void)build("Ok"), BT::RuntimeError);
}

TEST_F(CheckQualityNodeTest, ANameFromTheBlackboardIsCheckedAtTick) {
    poll_with(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
    auto tree = build("{wanted}");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}

}  // namespace

// #941: an expected_quality read from a blackboard entry nothing wrote fails
// the node; it was taken as OK.
TEST_F(CheckQualityNodeTest, AnUnwrittenExpectedQualityFailsTheNode) {
    poll_with(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
    auto tree = build("{never_written}");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}
