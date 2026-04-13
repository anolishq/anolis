#include "automation/bt_nodes.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <sstream>

#include "automation/bt_services.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

std::string make_get_parameter_tree_xml(const std::string& param_name) {
    return std::string(R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameter param=")") +
           param_name +
           R"(" value="{out}"/>
  </BehaviorTree>
</root>
)";
}

std::string make_get_parameter_bool_tree_xml(const std::string& param_name) {
    return std::string(R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameterBool param=")") +
           param_name +
           R"(" value="{out}"/>
  </BehaviorTree>
</root>
)";
}

std::string make_get_parameter_int64_tree_xml(const std::string& param_name) {
    return std::string(R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <GetParameterInt64 param=")") +
           param_name +
           R"(" value="{out}"/>
  </BehaviorTree>
</root>
)";
}

std::string make_check_bool_tree_xml(bool value, bool expected) {
    std::stringstream xml;
    xml << R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <CheckBool value=")" << (value ? "true" : "false") << R"(" expected=")" << (expected ? "true" : "false")
        << R"("/>
  </BehaviorTree>
</root>
)";
    return xml.str();
}

std::string make_pulse_tree_xml(int64_t startup_delay_s, int64_t interval_s, int64_t pulse_s) {
    std::stringstream xml;
    xml << R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <PeriodicPulseWindow enabled="true" startup_delay_s=")"
        << startup_delay_s << R"(" interval_s=")" << interval_s << R"(" pulse_s=")" << pulse_s << R"("
        now_ms="{now}" active="{active}" pulse_index="{idx}" elapsed_ms="{elapsed}"/>
  </BehaviorTree>
</root>
)";
    return xml.str();
}

std::string make_parameter_driven_pulse_tree_xml() {
    return R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <Sequence>
      <GetParameterBool param="feed_enable" value="{feed_enable}"/>
      <GetParameterInt64 param="feed_startup_delay_s" value="{feed_startup_delay_s}"/>
      <GetParameterInt64 param="feed_interval_s" value="{feed_interval_s}"/>
      <GetParameterInt64 param="feed_pulse_s" value="{feed_pulse_s}"/>
      <PeriodicPulseWindow
        enabled="{feed_enable}"
        startup_delay_s="{feed_startup_delay_s}"
        interval_s="{feed_interval_s}"
        pulse_s="{feed_pulse_s}"
        now_ms="{now}"
        active="{active}"
        pulse_index="{idx}"
        elapsed_ms="{elapsed}"/>
    </Sequence>
  </BehaviorTree>
</root>
)";
}

std::string make_emit_gate_tree_xml(int64_t keepalive_s) {
    std::stringstream xml;
    xml << R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <EmitOnChangeOrInterval key="{key}" keepalive_s=")" << keepalive_s
        << R"(" now_ms="{now}" emit="{emit}" reason="{reason}"/>
  </BehaviorTree>
</root>
)";
    return xml.str();
}

std::string make_build_args_tree_xml(double motor1, double motor2) {
    std::stringstream xml;
    xml << R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <BuildArgsJson
      arg1_name="motor1_pwm"
      arg1_type="int64"
      arg1_num=")"
        << motor1 << R"("
      arg2_name="motor2_pwm"
      arg2_type="int64"
      arg2_num=")" << motor2 << R"("
      json="{args}"/>
  </BehaviorTree>
</root>
)";
    return xml.str();
}

class BTNodesTest : public ::testing::Test {
protected:
    anolis::registry::DeviceRegistry registry_;
    anolis::state::StateCache state_cache_{registry_, 100};
    anolis::control::CallRouter call_router_{registry_, state_cache_};
    anolis::provider::ProviderRegistry provider_registry_;
    anolis::automation::ParameterManager parameter_manager_;

    static void register_nodes(BT::BehaviorTreeFactory& factory) {
        factory.registerNodeType<anolis::automation::GetParameterNode>("GetParameter");
        factory.registerNodeType<anolis::automation::GetParameterBoolNode>("GetParameterBool");
        factory.registerNodeType<anolis::automation::GetParameterInt64Node>("GetParameterInt64");
        factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
        factory.registerNodeType<anolis::automation::PeriodicPulseWindowNode>("PeriodicPulseWindow");
        factory.registerNodeType<anolis::automation::EmitOnChangeOrIntervalNode>("EmitOnChangeOrInterval");
        factory.registerNodeType<anolis::automation::BuildArgsJsonNode>("BuildArgsJson");
    }

    BT::Blackboard::Ptr make_blackboard(bool with_context) {
        auto blackboard = BT::Blackboard::create();
        if (with_context) {
            anolis::automation::BTServiceContext services;
            services.state_cache = &state_cache_;
            services.call_router = &call_router_;
            services.provider_registry = &provider_registry_;
            services.parameter_manager = &parameter_manager_;
            blackboard->set(anolis::automation::kBTServiceContextKey, services);
        }
        return blackboard;
    }
};

}  // namespace

TEST_F(BTNodesTest, GetParameterNodeSucceedsForDouble) {
    ASSERT_TRUE(parameter_manager_.define("temp_setpoint", anolis::automation::ParameterType::DOUBLE, 33.5));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_get_parameter_tree_xml("temp_setpoint"), blackboard);

    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_DOUBLE_EQ(33.5, blackboard->get<double>("out"));
}

TEST_F(BTNodesTest, GetParameterNodeSucceedsForInt64) {
    ASSERT_TRUE(parameter_manager_.define("retry_limit", anolis::automation::ParameterType::INT64, int64_t{7}));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_get_parameter_tree_xml("retry_limit"), blackboard);

    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_DOUBLE_EQ(7.0, blackboard->get<double>("out"));
}

TEST_F(BTNodesTest, GetParameterNodeFailsForNonNumericTypes) {
    ASSERT_TRUE(parameter_manager_.define("enabled", anolis::automation::ParameterType::BOOL, true));
    ASSERT_TRUE(parameter_manager_.define("mode_name", anolis::automation::ParameterType::STRING, std::string("AUTO")));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);

    auto bool_blackboard = make_blackboard(true);
    auto bool_tree = factory.createTreeFromText(make_get_parameter_tree_xml("enabled"), bool_blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, bool_tree.tickOnce());

    auto string_blackboard = make_blackboard(true);
    auto string_tree = factory.createTreeFromText(make_get_parameter_tree_xml("mode_name"), string_blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, string_tree.tickOnce());
}

TEST_F(BTNodesTest, GetParameterNodeFailsWhenServiceContextMissing) {
    ASSERT_TRUE(parameter_manager_.define("temp_setpoint", anolis::automation::ParameterType::DOUBLE, 21.0));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(false);
    auto tree = factory.createTreeFromText(make_get_parameter_tree_xml("temp_setpoint"), blackboard);

    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}

TEST_F(BTNodesTest, GetParameterBoolNodeSucceedsForBool) {
    ASSERT_TRUE(parameter_manager_.define("feed_enable", anolis::automation::ParameterType::BOOL, true));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_get_parameter_bool_tree_xml("feed_enable"), blackboard);

    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("out"));
}

TEST_F(BTNodesTest, GetParameterInt64NodeSucceedsForInt64) {
    ASSERT_TRUE(parameter_manager_.define("feed_interval_s", anolis::automation::ParameterType::INT64, int64_t{900}));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_get_parameter_int64_tree_xml("feed_interval_s"), blackboard);

    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_EQ(900, blackboard->get<int64_t>("out"));
}

TEST_F(BTNodesTest, CheckBoolNodeMatchesExpectedValue) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);

    auto tree_true = factory.createTreeFromText(make_check_bool_tree_xml(true, true), blackboard);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree_true.tickOnce());

    auto tree_false = factory.createTreeFromText(make_check_bool_tree_xml(false, true), blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree_false.tickOnce());
}

TEST_F(BTNodesTest, PeriodicPulseWindowNodeComputesExpectedWindow) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);

    auto tree = factory.createTreeFromText(make_pulse_tree_xml(10, 5, 2), blackboard);
    blackboard->set<int64_t>("now", 0);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("active"));

    // +11s with delay=10s, interval=5s, pulse=2s => inside pulse 0
    blackboard->set<int64_t>("now", 11 * 1000);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("active"));
    EXPECT_EQ(0, blackboard->get<int64_t>("idx"));
}

TEST_F(BTNodesTest, PeriodicPulseWindowNodeAcceptsNumericParametersFromGetParameter) {
    ASSERT_TRUE(parameter_manager_.define("feed_enable", anolis::automation::ParameterType::BOOL, true));
    ASSERT_TRUE(parameter_manager_.define("feed_startup_delay_s", anolis::automation::ParameterType::INT64, int64_t{10}));
    ASSERT_TRUE(parameter_manager_.define("feed_interval_s", anolis::automation::ParameterType::INT64, int64_t{5}));
    ASSERT_TRUE(parameter_manager_.define("feed_pulse_s", anolis::automation::ParameterType::INT64, int64_t{2}));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_parameter_driven_pulse_tree_xml(), blackboard);

    blackboard->set<int64_t>("now", 0);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("active"));

    blackboard->set<int64_t>("now", 11 * 1000);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("active"));
    EXPECT_EQ(0, blackboard->get<int64_t>("idx"));
}

TEST_F(BTNodesTest, EmitOnChangeOrIntervalNodeEmitsOnFirstAndChange) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);

    auto tree = factory.createTreeFromText(make_emit_gate_tree_xml(30), blackboard);

    blackboard->set<std::string>("key", "cmdA");
    blackboard->set<int64_t>("now", 1000);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("emit"));

    blackboard->set<std::string>("key", "cmdB");
    blackboard->set<int64_t>("now", 2000);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("emit"));
}

TEST_F(BTNodesTest, BuildArgsJsonNodeBuildsInt64Args) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_build_args_tree_xml(35.0, 0.0), blackboard);

    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    const auto args_json = blackboard->get<std::string>("args");
    EXPECT_NE(args_json.find("\"motor1_pwm\":35"), std::string::npos);
    EXPECT_NE(args_json.find("\"motor2_pwm\":0"), std::string::npos);
}
