#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/bt_factory.h>

#include <iostream>

#include "automation/bt_nodes.hpp"

static bool has_port(const BT::PortsList &ports, const std::string &key) { return ports.find(key) != ports.end(); }

int main() {
    try {
        BT::BehaviorTreeFactory factory;
        factory.registerNodeType<anolis::automation::ReadSignalNode>("ReadSignal");
        factory.registerNodeType<anolis::automation::CallDeviceNode>("CallDevice");
        factory.registerNodeType<anolis::automation::CheckQualityNode>("CheckQuality");
        factory.registerNodeType<anolis::automation::GetParameterNode>("GetParameter");
        factory.registerNodeType<anolis::automation::GetParameterBoolNode>("GetParameterBool");
        factory.registerNodeType<anolis::automation::GetParameterInt64Node>("GetParameterInt64");
        factory.registerNodeType<anolis::automation::CheckBoolNode>("CheckBool");
        factory.registerNodeType<anolis::automation::PeriodicPulseWindowNode>("PeriodicPulseWindow");
        factory.registerNodeType<anolis::automation::EmitOnChangeOrIntervalNode>("EmitOnChangeOrInterval");
        factory.registerNodeType<anolis::automation::BuildArgsJsonNode>("BuildArgsJson");

        const auto read_ports = anolis::automation::ReadSignalNode::providedPorts();
        const auto call_ports = anolis::automation::CallDeviceNode::providedPorts();
        const auto qual_ports = anolis::automation::CheckQualityNode::providedPorts();
        const auto param_ports = anolis::automation::GetParameterNode::providedPorts();
        const auto bool_param_ports = anolis::automation::GetParameterBoolNode::providedPorts();
        const auto int_param_ports = anolis::automation::GetParameterInt64Node::providedPorts();
        const auto check_bool_ports = anolis::automation::CheckBoolNode::providedPorts();
        const auto pulse_ports = anolis::automation::PeriodicPulseWindowNode::providedPorts();
        const auto emit_ports = anolis::automation::EmitOnChangeOrIntervalNode::providedPorts();
        const auto args_ports = anolis::automation::BuildArgsJsonNode::providedPorts();

        if (!has_port(read_ports, "device_handle") || !has_port(read_ports, "signal_id")) {
            std::cerr << "ReadSignalNode ports missing\n";
            return 1;
        }
        if (!has_port(call_ports, "device_handle") || !has_port(call_ports, "function_name") ||
            !has_port(call_ports, "args")) {
            std::cerr << "CallDeviceNode ports missing\n";
            return 1;
        }
        if (!has_port(qual_ports, "device_handle") || !has_port(qual_ports, "signal_id")) {
            std::cerr << "CheckQualityNode ports missing\n";
            return 1;
        }
        if (!has_port(param_ports, "param") || !has_port(param_ports, "value")) {
            std::cerr << "GetParameterNode ports missing\n";
            return 1;
        }
        if (!has_port(bool_param_ports, "param") || !has_port(bool_param_ports, "value")) {
            std::cerr << "GetParameterBoolNode ports missing\n";
            return 1;
        }
        if (!has_port(int_param_ports, "param") || !has_port(int_param_ports, "value")) {
            std::cerr << "GetParameterInt64Node ports missing\n";
            return 1;
        }
        if (!has_port(check_bool_ports, "value") || !has_port(check_bool_ports, "expected")) {
            std::cerr << "CheckBoolNode ports missing\n";
            return 1;
        }
        if (!has_port(pulse_ports, "enabled") || !has_port(pulse_ports, "active")) {
            std::cerr << "PeriodicPulseWindowNode ports missing\n";
            return 1;
        }
        if (!has_port(emit_ports, "key") || !has_port(emit_ports, "emit")) {
            std::cerr << "EmitOnChangeOrIntervalNode ports missing\n";
            return 1;
        }
        if (!has_port(args_ports, "arg1_name") || !has_port(args_ports, "json")) {
            std::cerr << "BuildArgsJsonNode ports missing\n";
            return 1;
        }

        std::cout << "bt_nodes_sanity: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "bt_nodes_sanity: EXCEPTION: " << e.what() << "\n";
        return 2;
    }
}
