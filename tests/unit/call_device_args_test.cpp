// CallDevice's args (#806): an args port that is present but cannot be read
// is a failed call, reported and counted, never a call with no arguments; and
// literal args that are not a JSON object refuse the tree when it is built.

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "automation/bt_nodes.hpp"
#include "automation/bt_services.hpp"
#include "control/call_router.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

struct Reported {
    bool success;
    std::string error;
};

// No provider is registered, so a call that reaches the router is refused
// with "Provider not found": that error is how a test sees a dispatch.
class CallDeviceArgsTest : public ::testing::Test {
protected:
    void SetUp() override { factory_.registerNodeType<anolis::automation::CallDeviceNode>("CallDevice"); }

    BT::Tree build(const std::string &args_attribute) {
        blackboard_ = BT::Blackboard::create();
        anolis::automation::BTServiceContext services;
        services.state_cache = &state_cache_;
        services.call_router = &call_router_;
        services.provider_registry = &provider_registry_;
        services.on_call_result = [this](const std::string &, const std::string &, bool success,
                                         const std::string &error) {
            reported_.push_back({success, error});
            return std::string();
        };
        blackboard_->set(anolis::automation::kBTServiceContextKey, services);
        const std::string xml =
            "<root BTCPP_format=\"4\"><BehaviorTree ID=\"Main\">"
            "<CallDevice device_handle=\"ghost0/dev1\" function_name=\"set_opt\" " +
            args_attribute + "/></BehaviorTree></root>";
        return factory_.createTreeFromText(xml, blackboard_);
    }

    BT::BehaviorTreeFactory factory_;
    BT::Blackboard::Ptr blackboard_;
    anolis::registry::DeviceRegistry registry_;
    anolis::state::StateCache state_cache_{registry_, 100};
    anolis::control::CallRouter call_router_{registry_, state_cache_};
    anolis::provider::ProviderRegistry provider_registry_;
    std::vector<Reported> reported_;
};

TEST_F(CallDeviceArgsTest, PrefixedJsonArgsReachTheRouter) {
    auto tree = build(R"(args='json:{"value":25.0}')");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("Provider not found"), std::string::npos) << reported_[0].error;
}

TEST_F(CallDeviceArgsTest, AbsentArgsStillMeanNoArguments) {
    auto tree = build("");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("Provider not found"), std::string::npos) << reported_[0].error;
}

TEST_F(CallDeviceArgsTest, AnEmptyLiteralStillMeansNoArguments) {
    auto tree = build(R"(args="")");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("Provider not found"), std::string::npos) << reported_[0].error;
}

TEST_F(CallDeviceArgsTest, UnprefixedJsonRefusesTheTree) {
    EXPECT_THROW((void)build(R"(args='{"value":25.0}')"), BT::RuntimeError);
}

TEST_F(CallDeviceArgsTest, LiteralArgsThatAreNotAJsonObjectRefuseTheTree) {
    EXPECT_THROW((void)build(R"(args='json:{"value":')"), BT::RuntimeError);
    EXPECT_THROW((void)build(R"(args='json:[1,2]')"), BT::RuntimeError);
}

TEST_F(CallDeviceArgsTest, ANeverWrittenKeyIsACountedFailureNotACallWithNoArguments) {
    auto tree = build(R"(args="{never_written}")");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_FALSE(reported_[0].success);
    EXPECT_NE(reported_[0].error.find("args could not be read"), std::string::npos) << reported_[0].error;
    EXPECT_EQ(reported_[0].error.find("Provider not found"), std::string::npos) << "the call was dispatched";
}

TEST_F(CallDeviceArgsTest, AKeyWrittenUnderAnotherNameIsACountedFailure) {
    auto tree = build(R"(args="{wanted}")");
    blackboard_->set("written", std::string(R"({"value":25.0})"));
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("args could not be read"), std::string::npos) << reported_[0].error;
}

TEST_F(CallDeviceArgsTest, AnEmptyValueFromTheBlackboardIsACountedFailure) {
    // What a refused BuildArgsJson leaves in its `json` output (#941).
    auto tree = build(R"(args="{json}")");
    blackboard_->set("json", std::string());
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("args is empty"), std::string::npos) << reported_[0].error;
    EXPECT_EQ(reported_[0].error.find("Provider not found"), std::string::npos) << "the call was dispatched";
}

TEST_F(CallDeviceArgsTest, APrefixWithNothingAfterItIsACountedFailure) {
    auto tree = build(R"(args="{json}")");
    blackboard_->set("json", std::string("json:"));
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("args is empty"), std::string::npos) << reported_[0].error;
}

TEST_F(CallDeviceArgsTest, BadJsonFromTheBlackboardIsACountedFailure) {
    auto tree = build(R"(args="{args}")");
    blackboard_->set("args", std::string(R"({"value":)"));
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    ASSERT_EQ(1u, reported_.size());
    EXPECT_NE(reported_[0].error.find("JSON parse error"), std::string::npos) << reported_[0].error;
}

}  // namespace

// #805: the allowed precondition does what it says on the call it guards, the
// behaviour the refusal of a misspelt one protects.
TEST_F(CallDeviceArgsTest, ATrueSkipIfMakesNoCall) {
    auto tree = build(R"(_skipIf="true")");
    (void)tree.tickOnce();
    EXPECT_TRUE(reported_.empty());
}

TEST_F(CallDeviceArgsTest, AFalseSkipIfStillMakesTheCall) {
    auto tree = build(R"(_skipIf="false")");
    (void)tree.tickOnce();
    EXPECT_EQ(1U, reported_.size());
}
