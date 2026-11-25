#include "automation/bt_nodes.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "automation/bt_services.hpp"
#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "logging/logger.hpp"
#include "mocks/mock_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"
#include "state/state_cache.hpp"

namespace {

std::string make_get_parameter_tree_xml(const std::string &param_name) {
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

std::string make_get_parameter_bool_tree_xml(const std::string &param_name) {
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

std::string make_get_parameter_int64_tree_xml(const std::string &param_name) {
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
    <CheckBool value=")"
        << (value ? "true" : "false") << R"(" expected=")" << (expected ? "true" : "false") << R"("/>
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

std::string make_requested_pulse_tree_xml(int64_t delay_s, int64_t pulse_s, int64_t max_queued = 1) {
    std::stringstream xml;
    xml << R"(<?xml version="1.0"?>
<root BTCPP_format="4">
  <BehaviorTree ID="MainTree">
    <RequestedPulseWindow request="{request}" delay_s=")"
        << delay_s << R"(" pulse_s=")" << pulse_s << R"(" max_queued=")" << max_queued
        << R"(" now_ms="{now}" active="{active}" queued="{queued}"/>
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
    <EmitOnChangeOrInterval key="{key}" keepalive_s=")"
        << keepalive_s << R"(" now_ms="{now}" emit="{emit}" reason="{reason}"/>
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
      arg2_num=")"
        << motor2 << R"("
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

    static void register_nodes(BT::BehaviorTreeFactory &factory) {
        factory.registerNodeType<anolis::automation::GetParameterNode>("GetParameter");
        factory.registerNodeType<anolis::automation::GetParameterBoolNode>("GetParameterBool");
        factory.registerNodeType<anolis::automation::GetParameterInt64Node>("GetParameterInt64");
        factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
        factory.registerNodeType<anolis::automation::PeriodicPulseWindowNode>("PeriodicPulseWindow");
        factory.registerNodeType<anolis::automation::RequestedPulseWindowNode>("RequestedPulseWindow");
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

// A tree re-ticks GetParameter every engine tick, so a successful read logs
// nothing at INFO; a failed one still logs at ERROR (#612).
TEST_F(BTNodesTest, GetParameterNodeLogsNoSuccessfulReadAtInfo) {
    ASSERT_TRUE(parameter_manager_.define("impeller_pwm", anolis::automation::ParameterType::INT64, int64_t{100}));

    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(make_get_parameter_tree_xml("impeller_pwm"), blackboard);
    auto missing = factory.createTreeFromText(make_get_parameter_tree_xml("no_such_param"), blackboard);

    anolis::logging::Logger::set_level(anolis::logging::Level::LVL_INFO);
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    for (int tick = 0; tick < 3; ++tick) {
        EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    }
    EXPECT_EQ(BT::NodeStatus::FAILURE, missing.tickOnce());
    std::cerr.rdbuf(old);

    const std::string log = captured.str();
    EXPECT_EQ(log.find("Read parameter"), std::string::npos) << log;
    EXPECT_NE(log.find("[ERROR] [GetParameterNode] Parameter not found: no_such_param"), std::string::npos) << log;
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

// #810: a schedule refused mid-pulse left `active` true. The node now writes
// the outputs it writes when disabled before it fails, and keeps its
// schedule for when the inputs become valid again.
TEST_F(BTNodesTest, PeriodicPulseWindowClearsItsOutputsWhenItRefusesASchedule) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = factory.createTreeFromText(
        R"(<root BTCPP_format="4"><BehaviorTree ID="Main">
<PeriodicPulseWindow interval_s="{interval}" pulse_s="{pulse}" now_ms="{now}"
                     active="{active}" pulse_index="{index}" elapsed_ms="{elapsed}"/>
</BehaviorTree></root>)",
        blackboard);
    const auto tick = [&](int64_t interval, int64_t pulse, int64_t now) {
        blackboard->set("interval", interval);
        blackboard->set("pulse", pulse);
        blackboard->set("now", now);
        return tree.tickOnce();
    };

    ASSERT_EQ(BT::NodeStatus::SUCCESS, tick(5, 2, 0));
    ASSERT_EQ(BT::NodeStatus::SUCCESS, tick(5, 2, 1000));
    ASSERT_TRUE(blackboard->get<bool>("active"));
    ASSERT_EQ(1000, blackboard->get<int64_t>("elapsed"));

    EXPECT_EQ(BT::NodeStatus::FAILURE, tick(0, 2, 4000)) << "interval_s 0";
    EXPECT_FALSE(blackboard->get<bool>("active"));
    EXPECT_EQ(-1, blackboard->get<int64_t>("index"));
    EXPECT_EQ(0, blackboard->get<int64_t>("elapsed"));

    // Valid again: the schedule carries on from t=0. Restarting it would
    // pulse at once on every refused-then-valid edge, a dose the interval
    // does not allow; at t=63000 the schedule is between pulses.
    ASSERT_EQ(BT::NodeStatus::SUCCESS, tick(5, 2, 63000));
    EXPECT_FALSE(blackboard->get<bool>("active"));
    EXPECT_EQ(12, blackboard->get<int64_t>("index"));
    EXPECT_EQ(63000, blackboard->get<int64_t>("elapsed"));

    ASSERT_EQ(BT::NodeStatus::SUCCESS, tick(5, 2, 65000));
    ASSERT_TRUE(blackboard->get<bool>("active")) << "in a pulse again";
    EXPECT_EQ(BT::NodeStatus::FAILURE, tick(1, 2, 65500)) << "pulse longer than the interval";
    EXPECT_FALSE(blackboard->get<bool>("active"));
}

TEST_F(BTNodesTest, PeriodicPulseWindowNodeAcceptsNumericParametersFromGetParameter) {
    ASSERT_TRUE(parameter_manager_.define("feed_enable", anolis::automation::ParameterType::BOOL, true));
    ASSERT_TRUE(
        parameter_manager_.define("feed_startup_delay_s", anolis::automation::ParameterType::INT64, int64_t{10}));
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

// #697: one pulse after a delay each time a counter parameter increases. The
// counter's first value is the baseline: a runtime start runs nothing.
namespace {

struct RequestedPulse {
    BT::Tree tree;
    BT::Blackboard::Ptr blackboard;

    bool at(int64_t now_ms, int64_t request) {
        blackboard->set<int64_t>("now", now_ms);
        blackboard->set<int64_t>("request", request);
        EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
        return blackboard->get<bool>("active");
    }
    int64_t queued() { return blackboard->get<int64_t>("queued"); }
};

}  // namespace

TEST_F(BTNodesTest, RequestedPulseWindowRunsOncePerIncreaseAfterItsDelay) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    RequestedPulse p{factory.createTreeFromText(make_requested_pulse_tree_xml(10, 3), blackboard), blackboard};

    EXPECT_FALSE(p.at(0, 4));     // the baseline: no pulse at start
    EXPECT_FALSE(p.at(5000, 4));  // nothing requested
    EXPECT_FALSE(p.at(6000, 5));  // requested: waits delay_s
    EXPECT_EQ(0, p.queued());     // the one request is running, not queued
    EXPECT_FALSE(p.at(15999, 5));
    EXPECT_TRUE(p.at(16000, 5));  // delay over: on for pulse_s
    EXPECT_TRUE(p.at(18999, 5));
    EXPECT_FALSE(p.at(19000, 5));  // and off
    EXPECT_FALSE(p.at(60000, 5));  // once: no repeat without a new request
}

TEST_F(BTNodesTest, RequestedPulseWindowRunsQueuedRequestsOneAfterAnother) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    RequestedPulse p{factory.createTreeFromText(make_requested_pulse_tree_xml(0, 2, 2), blackboard), blackboard};

    EXPECT_FALSE(p.at(0, 0));
    EXPECT_TRUE(p.at(1000, 2));  // two requests at once: the first runs now
    EXPECT_EQ(1, p.queued());
    EXPECT_TRUE(p.at(2999, 2));
    EXPECT_TRUE(p.at(3000, 2));  // the second follows when the first ends
    EXPECT_EQ(0, p.queued());
    EXPECT_FALSE(p.at(5000, 2));
}

// max_queued caps the requests outstanding, the running one included; presses
// beyond it add nothing, and the baseline still moves.
namespace {

int count_pulses(RequestedPulse &p, int presses) {
    int pulses = 0;
    bool was_active = false;
    int64_t request = 0;
    EXPECT_FALSE(p.at(0, request));
    for (int64_t t = 100; t <= 60000; t += 100) {
        if (presses > 0 && t % 300 == 0) {  // five presses, 0.3 s apart
            ++request;
            --presses;
        }
        const bool active = p.at(t, request);
        if (active && !was_active) ++pulses;
        was_active = active;
    }
    return pulses;
}

}  // namespace

TEST_F(BTNodesTest, RequestedPulseWindowCapsTheRequestsOutstanding) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    for (const auto &[cap, pulses] : std::vector<std::pair<int64_t, int>>{{1, 1}, {3, 3}}) {
        auto blackboard = make_blackboard(true);
        RequestedPulse p{factory.createTreeFromText(make_requested_pulse_tree_xml(2, 3, cap), blackboard), blackboard};
        EXPECT_EQ(pulses, count_pulses(p, 5)) << "max_queued " << cap;
    }
}

TEST_F(BTNodesTest, RequestedPulseWindowTakesAPressOnTheTickAPulseEnds) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    RequestedPulse p{factory.createTreeFromText(make_requested_pulse_tree_xml(0, 2, 1), blackboard), blackboard};

    EXPECT_FALSE(p.at(0, 0));
    EXPECT_TRUE(p.at(1000, 1));  // runs 1000-3000
    EXPECT_TRUE(p.at(3000, 2));  // pressed as it ends: its place is free, so it runs
    EXPECT_FALSE(p.at(5000, 2));
}

TEST_F(BTNodesTest, RequestedPulseWindowTakesADroppedCounterAsTheNewBaseline) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    RequestedPulse p{factory.createTreeFromText(make_requested_pulse_tree_xml(0, 2), blackboard), blackboard};

    EXPECT_FALSE(p.at(0, 7));
    EXPECT_FALSE(p.at(1000, 0));  // reset (a runtime restart's default): nothing runs
    EXPECT_TRUE(p.at(2000, 1));   // the next increase from the new baseline runs
}

TEST_F(BTNodesTest, RequestedPulseWindowFailsOnInvalidInputs) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    blackboard->set<int64_t>("now", 0);
    blackboard->set<int64_t>("request", 0);
    auto negative_delay = factory.createTreeFromText(make_requested_pulse_tree_xml(-1, 2), blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, negative_delay.tickOnce());
    auto no_pulse = factory.createTreeFromText(make_requested_pulse_tree_xml(0, 0), blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, no_pulse.tickOnce());
    auto no_queue = factory.createTreeFromText(make_requested_pulse_tree_xml(0, 2, 0), blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, no_queue.tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("active"));
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

TEST_F(BTNodesTest, NodesRegisterAndDeclareTheirPorts) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    EXPECT_NO_THROW(factory.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal"));
    EXPECT_NO_THROW(factory.registerNodeType<anolis::automation::CallDeviceNode>("CallDevice"));
    EXPECT_NO_THROW(factory.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality"));

    const auto expect_ports = [](const BT::PortsList &ports, std::initializer_list<const char *> keys) {
        for (const char *key : keys) {
            EXPECT_TRUE(ports.find(key) != ports.end()) << "missing port: " << key;
        }
    };
    expect_ports(anolis::automation::ReadSignalNode::providedPorts(), {"device_handle", "signal_id"});
    expect_ports(anolis::automation::CallDeviceNode::providedPorts(),
                 {"device_handle", "function_name", "args", "reason"});
    expect_ports(anolis::automation::CheckQualityNode::providedPorts(), {"device_handle", "signal_id"});
    expect_ports(anolis::automation::GetParameterNode::providedPorts(), {"param", "value"});
    expect_ports(anolis::automation::GetParameterBoolNode::providedPorts(), {"param", "value"});
    expect_ports(anolis::automation::GetParameterInt64Node::providedPorts(), {"param", "value"});
    expect_ports(anolis::automation::CheckBoolNode::providedPorts(), {"value", "expected"});
    expect_ports(anolis::automation::PeriodicPulseWindowNode::providedPorts(), {"enabled", "active"});
    expect_ports(anolis::automation::RequestedPulseWindowNode::providedPorts(),
                 {"request", "delay_s", "pulse_s", "max_queued", "active", "queued"});
    expect_ports(anolis::automation::EmitOnChangeOrIntervalNode::providedPorts(), {"key", "emit"});
    expect_ports(anolis::automation::BuildArgsJsonNode::providedPorts(), {"arg1_name", "json"});
}

// #425: a tree node reads parameters and never sets them. ParameterManager::set
// journals synchronously, which the tick thread must not do, so the context
// gives nodes a read-only view and a node that calls set does not compile.
TEST(BTServiceContextTest, NodesGetAReadOnlyParameterManager) {
    EXPECT_TRUE((std::is_same_v<decltype(anolis::automation::BTServiceContext::parameter_manager),
                                const anolis::automation::ParameterManager *>));
}

namespace {

// ReadSignal against a state cache filled by one poll of a mock provider that
// reports a double `temp` and a string `control_mode`, as the simulator does.
class ReadSignalNodeTest : public ::testing::Test {
protected:
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
                auto *caps = response.mutable_capabilities();
                auto *temp = caps->add_signals();
                temp->set_signal_id("temp");
                temp->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                temp->set_poll_hint_hz(1.0);
                auto *mode = caps->add_signals();
                mode->set_signal_id("control_mode");
                mode->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
                mode->set_poll_hint_hz(1.0);
                auto *level = caps->add_signals();
                level->set_signal_id("level");
                level->set_value_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
                level->set_poll_hint_hz(1.0);
                return true;
            }));
        ON_CALL(*mock_provider_, read_signals(_, _, _, _))
            .WillByDefault(Invoke([](const std::string &, const std::vector<std::string> &,
                                     ReadSignalsResponse &response, anolis::deviceprovider::v1::Status_Code &) {
                auto *temp = response.add_values();
                temp->set_signal_id("temp");
                temp->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK);
                temp->mutable_value()->set_double_value(37.5);
                auto *mode = response.add_values();
                mode->set_signal_id("control_mode");
                mode->set_quality(anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE);
                mode->mutable_value()->set_string_value("closed");
                auto *level = response.add_values();  // no quality set: UNSPECIFIED
                level->set_signal_id("level");
                level->mutable_value()->set_double_value(4.0);
                return true;
            }));
        provider_registry_.add_provider("sim0", mock_provider_);
        registry_.discover_provider("sim0", *mock_provider_);
        state_cache_ = std::make_unique<anolis::state::StateCache>(registry_, 500);
        ASSERT_TRUE(state_cache_->initialize());
        state_cache_->poll_once(provider_registry_);
    }

    BT::NodeStatus tick_read(const std::string &signal_id, BT::Blackboard::Ptr &blackboard) {
        BT::BehaviorTreeFactory factory;
        factory.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal");
        blackboard = BT::Blackboard::create();
        anolis::automation::BTServiceContext services;
        services.state_cache = state_cache_.get();
        blackboard->set(anolis::automation::kBTServiceContextKey, services);
        const std::string xml = R"(<root BTCPP_format="4"><BehaviorTree ID="Main">
<ReadSignal device_handle="sim0/dev0" signal_id=")" +
                                signal_id +
                                R"(" value="{value}" quality="{quality}"/>
</BehaviorTree></root>)";
        auto tree = factory.createTreeFromText(xml, blackboard);
        return tree.tickOnce();
    }

    anolis::registry::DeviceRegistry registry_;
    anolis::provider::ProviderRegistry provider_registry_;
    std::shared_ptr<::testing::NiceMock<anolis::tests::MockProviderHandle>> mock_provider_;
    std::unique_ptr<anolis::state::StateCache> state_cache_;
};

}  // namespace

TEST_F(ReadSignalNodeTest, ReadsANumericSignal) {
    BT::Blackboard::Ptr blackboard;
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tick_read("temp", blackboard));
    EXPECT_DOUBLE_EQ(37.5, blackboard->get<double>("value"));
    EXPECT_EQ("OK", blackboard->get<std::string>("quality"));
}

// A text signal has no number to give; reporting 0.0 with SUCCESS handed the
// tree a plausible value and no error (#816).
TEST_F(ReadSignalNodeTest, FailsOnAStringSignalInsteadOfReadingZero) {
    BT::Blackboard::Ptr blackboard;
    EXPECT_EQ(BT::NodeStatus::FAILURE, tick_read("control_mode", blackboard));
    // The port's entry exists from tree creation; nothing was written to it.
    const auto entry = blackboard->getEntry("value");
    EXPECT_TRUE(entry == nullptr || entry->value.empty());
}

// Review of #887: the reading's quality is written even when its value has
// no number to give, so `quality` never keeps an earlier tick's.
TEST_F(ReadSignalNodeTest, WritesTheQualityOfASignalItCannotRead) {
    BT::Blackboard::Ptr blackboard;
    EXPECT_EQ(BT::NodeStatus::FAILURE, tick_read("control_mode", blackboard));
    EXPECT_EQ("STALE", blackboard->get<std::string>("quality"));
}

// Review of #902: a signal the cache no longer holds (its device dropped
// out) left `quality` at the last tick's value. Every failure writes one.
TEST_F(ReadSignalNodeTest, ASignalNotInTheCacheWritesUnavailableOverTheLastQuality) {
    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal");
    auto blackboard = BT::Blackboard::create();
    anolis::automation::BTServiceContext services;
    services.state_cache = state_cache_.get();
    blackboard->set(anolis::automation::kBTServiceContextKey, services);
    blackboard->set<std::string>("quality", "OK");  // an earlier tick's
    auto tree = factory.createTreeFromText(R"(<root BTCPP_format="4"><BehaviorTree ID="Main">
<ReadSignal device_handle="sim0/gone0" signal_id="temp" value="{value}" quality="{quality}"/>
</BehaviorTree></root>)",
                                           blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    EXPECT_EQ("UNAVAILABLE", blackboard->get<std::string>("quality"));
}

// Review of #888: a quality the provider did not set is UNAVAILABLE, the name
// HTTP and events give it, not UNSPECIFIED.
TEST_F(ReadSignalNodeTest, AnUnsetQualityReadsAsUnavailable) {
    BT::Blackboard::Ptr blackboard;
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tick_read("level", blackboard));
    EXPECT_EQ("UNAVAILABLE", blackboard->get<std::string>("quality"));
}

// Review of #888: CheckQuality expected_quality="UNAVAILABLE" matches a
// reading HTTP shows as UNAVAILABLE, including one with no quality set.
TEST_F(ReadSignalNodeTest, CheckQualityUnavailableMatchesAnUnsetQuality) {
    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality");
    auto blackboard = BT::Blackboard::create();
    anolis::automation::BTServiceContext services;
    services.state_cache = state_cache_.get();
    blackboard->set(anolis::automation::kBTServiceContextKey, services);
    auto tree = factory.createTreeFromText(
        R"(<root BTCPP_format="4"><BehaviorTree ID="Main">
<CheckQuality device_handle="sim0/dev0" signal_id="level" expected_quality="UNAVAILABLE"/>
</BehaviorTree></root>)",
        blackboard);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
}

// #822: CheckBool and CheckQuality only test something, so they are
// Conditions, which editors such as Groot2 list and draw as such.
TEST(BtNodeKinds, CheckBoolAndCheckQualityAreConditions) {
    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
    factory.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality");
    EXPECT_EQ(factory.manifests().at("CheckBool").type, BT::NodeType::CONDITION);
    EXPECT_EQ(factory.manifests().at("CheckQuality").type, BT::NodeType::CONDITION);
}

// What the changelog says: every form a tree may use still loads, the explicit
// Action form included, since BehaviorTree.CPP does not check the kind an
// explicit tag names against the registered one.
TEST(BtNodeKinds, EveryFormOfATreeStillLoads) {
    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
    const auto tree_with = [](const std::string &node) {
        return std::string(R"(<root BTCPP_format="4"><BehaviorTree ID="Main">)") + node + "</BehaviorTree></root>";
    };
    auto blackboard = BT::Blackboard::create();
    blackboard->set("flag", true);
    EXPECT_NO_THROW((void)factory.createTreeFromText(tree_with(R"(<CheckBool value="{flag}"/>)"), blackboard));
    EXPECT_NO_THROW(
        (void)factory.createTreeFromText(tree_with(R"(<Condition ID="CheckBool" value="{flag}"/>)"), blackboard));
    EXPECT_NO_THROW(
        (void)factory.createTreeFromText(tree_with(R"(<Action ID="CheckBool" value="{flag}"/>)"), blackboard));
}

// #941: an input whose port reads a blackboard entry that is missing, or
// holds another type, fails the node instead of taking the port's default;
// and a node that fails writes defined outputs.
namespace {

BT::Tree one_node(BT::BehaviorTreeFactory &factory, const std::string &node, const BT::Blackboard::Ptr &blackboard) {
    return factory.createTreeFromText(
        R"(<root BTCPP_format="4"><BehaviorTree ID="Main">)" + node + "</BehaviorTree></root>", blackboard);
}

}  // namespace

TEST_F(BTNodesTest, AnUnwrittenEnableFailsThePulseWindowInsteadOfRunningIt) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = one_node(factory,
                         R"(<PeriodicPulseWindow enabled="{en}" interval_s="5" pulse_s="2" now_ms="0" )"
                         R"(active="{active}" pulse_index="{index}" elapsed_ms="{elapsed}"/>)",
                         blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("active"));

    blackboard->set("en", true);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce()) << "written, it runs";
    EXPECT_TRUE(blackboard->get<bool>("active"));
}

TEST_F(BTNodesTest, ABlackboardInputOfAnotherTypeFailsTheNode) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    blackboard->set("interval", std::string("soon"));
    auto tree =
        one_node(factory, R"(<PeriodicPulseWindow interval_s="{interval}" pulse_s="2" now_ms="0" active="{active}"/>)",
                 blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}

TEST_F(BTNodesTest, ALiteralOrAbsentInputKeepsItsDefault) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree = one_node(factory, R"(<PeriodicPulseWindow pulse_s="2" now_ms="0" active="{active}"/>)", blackboard);
    EXPECT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    EXPECT_TRUE(blackboard->get<bool>("active"));
}

TEST_F(BTNodesTest, AnUnwrittenInputFailsTheOtherWindowAndCheckNodes) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    blackboard->set("requests", int64_t{0});
    blackboard->set("flag", true);
    EXPECT_EQ(BT::NodeStatus::FAILURE,
              one_node(factory,
                       R"(<RequestedPulseWindow request="{requests}" delay_s="{delay}" pulse_s="2" now_ms="0" )"
                       R"(active="{active}" queued="{queued}"/>)",
                       blackboard)
                  .tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("active"));
    EXPECT_EQ(BT::NodeStatus::FAILURE,
              one_node(factory, R"(<CheckBool value="{flag}" expected="{wanted}"/>)", blackboard).tickOnce())
        << "an unwritten expected is not taken as true";
}

TEST_F(BTNodesTest, EmitOnChangeOrIntervalClearsEmitWhenItFails) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    blackboard->set("k", std::string("a"));
    blackboard->set("keepalive", int64_t{30});
    auto tree = one_node(factory,
                         R"(<EmitOnChangeOrInterval key="{k}" keepalive_s="{keepalive}" now_ms="0" )"
                         R"(emit="{emit}" reason="{reason}"/>)",
                         blackboard);
    ASSERT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    ASSERT_TRUE(blackboard->get<bool>("emit"));

    blackboard->set("keepalive", int64_t{-1});
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    EXPECT_FALSE(blackboard->get<bool>("emit")) << "a refused tick does not leave the last emit standing";
}

TEST_F(BTNodesTest, BuildArgsJsonClearsJsonWhenItFails) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    blackboard->set("n", 5.0);
    auto tree = one_node(factory, R"(<BuildArgsJson arg1_name="p" arg1_num="{n}" json="{json}"/>)", blackboard);
    ASSERT_EQ(BT::NodeStatus::SUCCESS, tree.tickOnce());
    ASSERT_EQ(R"({"p":5.0})", blackboard->get<std::string>("json"));

    blackboard->unset("n");
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
    EXPECT_EQ("", blackboard->get<std::string>("json")) << "not the last tick's arguments";
}

// An unwritten source is not passed over for another: arg1_num reads a key
// nothing wrote, so the literal arg1_str must not be sent in its place.
TEST_F(BTNodesTest, BuildArgsJsonFailsOnAnUnwrittenSourceInsteadOfUsingAnother) {
    BT::BehaviorTreeFactory factory;
    register_nodes(factory);
    auto blackboard = make_blackboard(true);
    auto tree =
        one_node(factory, R"(<BuildArgsJson arg1_name="p" arg1_num="{n}" arg1_str="x" json="{json}"/>)", blackboard);
    EXPECT_EQ(BT::NodeStatus::FAILURE, tree.tickOnce());
}
