// Anolis nodes inside a SubTree find their services (#807).
//
// BTRuntime puts the service context on the tree's root blackboard. A SubTree
// has its own blackboard, so a node that read only its own failed every tick
// with "Missing key [anolis.bt_service_context]" unless the SubTree set
// _autoremap. Each node here sits one and two SubTrees deep without it.

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "automation/bt_nodes.hpp"
#include "automation/bt_services.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

class SubTreeServiceContextTest : public ::testing::Test {
protected:
    // A device sim0/dev0 whose one signal, `temp`, is polled once.
    void SetUp() override {
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
            .WillByDefault(Invoke([](const std::string &, const std::vector<std::string> &,
                                     ReadSignalsResponse &response, anolis::deviceprovider::v1::Status_Code &) {
                auto *temp = response.add_values();
                temp->set_signal_id("temp");
                temp->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                temp->mutable_value()->set_double_value(21.0);
                return true;
            }));
        provider_registry_.add_provider("sim0", mock_provider_);
        registry_.discover_provider("sim0", *mock_provider_);
        state_cache_ = std::make_unique<anolis::state::StateCache>(registry_, 500);
        ASSERT_TRUE(state_cache_->initialize());
        state_cache_->poll_once(provider_registry_);
        call_router_ = std::make_unique<anolis::control::CallRouter>(registry_, *state_cache_);
        ASSERT_TRUE(parameter_manager_.define("setpoint", anolis::automation::ParameterType::DOUBLE, 30.0));

        factory_.registerNodeType<anolis::automation::GetParameterNode>("GetParameter");
        factory_.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal");
        factory_.registerNodeType<anolis::automation::CallDeviceNode>("CallDevice");
    }

    // `node` inside `depth` nested SubTrees, none with _autoremap, ticked
    // once with the context on the root blackboard only, as BTRuntime puts it.
    BT::NodeStatus tick_nested(const std::string &node, int depth) {
        std::string xml = R"(<root BTCPP_format="4" main_tree_to_execute="Main">)";
        xml += R"(<BehaviorTree ID="Main"><SubTree ID="Level1"/></BehaviorTree>)";
        for (int level = 1; level <= depth; ++level) {
            const std::string body =
                level == depth ? node : R"(<SubTree ID="Level)" + std::to_string(level + 1) + R"("/>)";
            xml += R"(<BehaviorTree ID="Level)" + std::to_string(level) + R"(">)" + body + "</BehaviorTree>";
        }
        xml += "</root>";

        auto root = BT::Blackboard::create();
        anolis::automation::BTServiceContext services;
        services.state_cache = state_cache_.get();
        services.call_router = call_router_.get();
        services.provider_registry = &provider_registry_;
        services.parameter_manager = &parameter_manager_;
        services.on_call_result = [this](const std::string &, const std::string &, bool, const std::string &) {
            ++calls_reported_;
            return std::string();
        };
        root->set(anolis::automation::kBTServiceContextKey, services);
        auto tree = factory_.createTreeFromText(xml, root);
        return tree.tickOnce();
    }

    BT::BehaviorTreeFactory factory_;
    anolis::registry::DeviceRegistry registry_;
    anolis::provider::ProviderRegistry provider_registry_;
    std::shared_ptr<::testing::NiceMock<anolis::tests::MockProviderHandle>> mock_provider_;
    std::unique_ptr<anolis::state::StateCache> state_cache_;
    std::unique_ptr<anolis::control::CallRouter> call_router_;
    anolis::automation::ParameterManager parameter_manager_;
    int calls_reported_ = 0;
};

class SubTreeDepthTest : public SubTreeServiceContextTest, public ::testing::WithParamInterface<int> {};

TEST_P(SubTreeDepthTest, GetParameterReadsTheParameter) {
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tick_nested(R"(<GetParameter param="setpoint" value="{v}"/>)", GetParam()));
}

TEST_P(SubTreeDepthTest, ReadSignalReadsTheSignal) {
    EXPECT_EQ(BT::NodeStatus::SUCCESS,
              tick_nested(R"(<ReadSignal device_handle="sim0/dev0" signal_id="temp" value="{v}"/>)", GetParam()));
}

// The device declares no function, so the router refuses the call; what
// matters is that CallDevice reached it and reported the result, which it
// cannot do without its services.
TEST_P(SubTreeDepthTest, CallDeviceReachesTheRouter) {
    (void)tick_nested(R"(<CallDevice device_handle="sim0/dev0" function_name="noop"/>)", GetParam());
    EXPECT_EQ(1, calls_reported_);
}

INSTANTIATE_TEST_SUITE_P(OneAndTwoDeep, SubTreeDepthTest, ::testing::Values(1, 2));

}  // namespace
