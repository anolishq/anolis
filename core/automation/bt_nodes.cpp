#include "automation/bt_nodes.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "automation/parameter_manager.hpp"
#include "control/call_router.hpp"
#include "logging/logger.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace automation {

namespace {
std::optional<BTServiceContext> read_service_context(const BT::TreeNode &node, const char *node_name) {
    const auto blackboard = node.config().blackboard;
    if (!blackboard) {
        LOG_ERROR("[" << node_name << "] Blackboard is null");
        return std::nullopt;
    }

    try {
        return blackboard->get<BTServiceContext>(kBTServiceContextKey);
    } catch (const std::exception &e) {
        LOG_ERROR("[" << node_name << "] Missing/invalid BT service context: " << e.what());
        return std::nullopt;
    }
}

int64_t current_time_ms() {
    // Use monotonic time to avoid schedule distortions from wall-clock jumps.
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// Helper: Convert protobuf Value to double (for output port)
static double value_to_double(const anolis::deviceprovider::v1::Value &val) {
    switch (val.kind_case()) {
        case anolis::deviceprovider::v1::Value::kDoubleValue:
            return val.double_value();
        case anolis::deviceprovider::v1::Value::kInt64Value:
            return static_cast<double>(val.int64_value());
        case anolis::deviceprovider::v1::Value::kUint64Value:
            return static_cast<double>(val.uint64_value());
        case anolis::deviceprovider::v1::Value::kBoolValue:
            return val.bool_value() ? 1.0 : 0.0;
        default:
            return 0.0;  // Default for string/bytes/unspecified
    }
}

// Helper: Convert quality enum to string
static std::string quality_to_string(anolis::deviceprovider::v1::SignalValue_Quality q) {
    switch (q) {
        case anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK:
            return "OK";
        case anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE:
            return "STALE";
        case anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_FAULT:
            return "FAULT";
        case anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_UNKNOWN:
            return "UNKNOWN";
        default:
            return "UNSPECIFIED";
    }
}

// Helper: Convert string to quality enum
static anolis::deviceprovider::v1::SignalValue_Quality string_to_quality(const std::string &s) {
    if (s == "OK") {
        return anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK;
    }
    if (s == "STALE") {
        return anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_STALE;
    }
    if (s == "FAULT") {
        return anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_FAULT;
    }
    if (s == "UNKNOWN") {
        return anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_UNKNOWN;
    }
    return anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_UNSPECIFIED;
}

// Helper: Create protobuf Value from double
static anolis::deviceprovider::v1::Value double_to_value(double d) {
    anolis::deviceprovider::v1::Value val;
    val.set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
    val.set_double_value(d);
    return val;
}

// Helper: Create protobuf Value from int64
static anolis::deviceprovider::v1::Value int64_to_value(int64_t i) {
    anolis::deviceprovider::v1::Value val;
    val.set_type(anolis::deviceprovider::v1::VALUE_TYPE_INT64);
    val.set_int64_value(i);
    return val;
}

// Helper: Create protobuf Value from bool
static anolis::deviceprovider::v1::Value bool_to_value(bool b) {
    anolis::deviceprovider::v1::Value val;
    val.set_type(anolis::deviceprovider::v1::VALUE_TYPE_BOOL);
    val.set_bool_value(b);
    return val;
}

// Helper: Create protobuf Value from string
static anolis::deviceprovider::v1::Value string_to_value(const std::string &s) {
    anolis::deviceprovider::v1::Value val;
    val.set_type(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
    val.set_string_value(s);
    return val;
}

//-----------------------------------------------------------------------------
// ReadSignalNode
//-----------------------------------------------------------------------------

ReadSignalNode::ReadSignalNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

BT::PortsList ReadSignalNode::providedPorts() {
    return {BT::InputPort<std::string>("device_handle", "Device handle (provider_id/device_id)"),
            BT::InputPort<std::string>("signal_id", "Signal identifier"),
            BT::OutputPort<double>("value", "Signal value (as double)"),
            BT::OutputPort<std::string>("quality", "Signal quality (OK/STALE/UNAVAILABLE/FAULT)")};
}

BT::NodeStatus ReadSignalNode::tick() {
    const auto services = get_services();
    if (!services || services->state_cache == nullptr) {
        LOG_ERROR("[ReadSignalNode] StateCache not available in BT service context");
        return BT::NodeStatus::FAILURE;
    }

    // Get input ports
    auto device_handle = getInput<std::string>("device_handle");
    auto signal_id = getInput<std::string>("signal_id");

    if (!device_handle || !signal_id) {
        LOG_ERROR("[ReadSignalNode] Missing required input ports");
        return BT::NodeStatus::FAILURE;
    }

    // Read signal from StateCache
    auto signal_value = services->state_cache->get_signal_value(device_handle.value(), signal_id.value());

    if (!signal_value) {
        LOG_ERROR("[ReadSignalNode] Signal not found: " << device_handle.value() << "/" << signal_id.value());
        return BT::NodeStatus::FAILURE;
    }

    // Convert and write output ports
    double value_out = value_to_double(signal_value->value);
    std::string quality_out = quality_to_string(signal_value->quality);

    setOutput("value", value_out);
    setOutput("quality", quality_out);

    return BT::NodeStatus::SUCCESS;
}

std::optional<BTServiceContext> ReadSignalNode::get_services() const {
    return read_service_context(*this, "ReadSignalNode");
}

//-----------------------------------------------------------------------------
// CallDeviceNode
//-----------------------------------------------------------------------------

CallDeviceNode::CallDeviceNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

BT::PortsList CallDeviceNode::providedPorts() {
    // Structured payload interface: device_handle + function_name + args (JSON)
    // Args is a JSON object string, e.g., '{"target":30.0}'
    // Validation handled by CallRouter using ArgSpec metadata
    return {BT::InputPort<std::string>("device_handle", "Device handle (provider_id/device_id)"),
            BT::InputPort<std::string>("function_name", "Function identifier"),
            BT::InputPort<std::string>("args", "{}", "Arguments as JSON object (e.g., '{\"target\":30.0}')"),
            BT::OutputPort<bool>("success", "Call result (true/false)"),
            BT::OutputPort<std::string>("error", "Error message if call failed")};
}

BT::NodeStatus CallDeviceNode::tick() {
    const auto services = get_services();
    if (!services || services->call_router == nullptr) {
        LOG_ERROR("[CallDeviceNode] CallRouter not available in BT service context");
        setOutput("success", false);
        setOutput("error", "CallRouter not available");
        return BT::NodeStatus::FAILURE;
    }

    // Get input ports
    auto device_handle = getInput<std::string>("device_handle");
    auto function_name = getInput<std::string>("function_name");

    if (!device_handle || !function_name) {
        LOG_ERROR("[CallDeviceNode] Missing required input ports");
        setOutput("success", false);
        setOutput("error", "Missing device_handle or function_name");
        return BT::NodeStatus::FAILURE;
    }

    // Build CallRequest
    control::CallRequest request;
    request.device_handle = device_handle.value();
    request.function_name = function_name.value();
    request.is_automated = true;  // Mark as automated (BT) call to bypass manual gating

    // Parse args JSON and convert to protobuf Value map
    auto args_str = getInput<std::string>("args").value_or("{}");

    // Strip "json:" prefix if present (BT.CPP convention for JSON literals)
    if (args_str.size() >= 5 && args_str.substr(0, 5) == "json:") {
        args_str = args_str.substr(5);  // Remove "json:" prefix
    }

    if (!args_str.empty() && args_str != "{}") {
        try {
            auto json_args = nlohmann::json::parse(args_str);

            if (!json_args.is_object()) {
                LOG_ERROR("[CallDeviceNode] args must be a JSON object, got: " << args_str);
                setOutput("success", false);
                setOutput("error", "args must be a JSON object");
                return BT::NodeStatus::FAILURE;
            }
            for (auto &[key, value] : json_args.items()) {
                if (value.is_number_float()) {
                    request.args[key] = double_to_value(value.get<double>());
                } else if (value.is_number_integer()) {
                    // JSON integers could be int or uint - use int64 as safe default
                    request.args[key] = int64_to_value(value.get<int64_t>());
                } else if (value.is_boolean()) {
                    request.args[key] = bool_to_value(value.get<bool>());
                } else if (value.is_string()) {
                    request.args[key] = string_to_value(value.get<std::string>());
                } else {
                    LOG_ERROR("[CallDeviceNode] Unsupported JSON type for arg '" << key << "'");
                    setOutput("success", false);
                    setOutput("error", "Unsupported JSON type for arg '" + key + "'");
                    return BT::NodeStatus::FAILURE;
                }
            }
        } catch (const nlohmann::json::parse_error &e) {
            LOG_ERROR("[CallDeviceNode] JSON parse error: " << e.what());
            setOutput("success", false);
            setOutput("error", std::string("JSON parse error: ") + e.what());
            return BT::NodeStatus::FAILURE;
        }
    }

    // Get provider registry from blackboard
    if (services->provider_registry == nullptr) {
        LOG_ERROR("[CallDeviceNode] ProviderRegistry not available in BT service context");
        setOutput("success", false);
        setOutput("error", "Provider registry not available");
        return BT::NodeStatus::FAILURE;
    }

    // Execute call via CallRouter
    auto result = services->call_router->execute_call(request, *services->provider_registry);

    setOutput("success", result.success);
    setOutput("error", result.error_message);

    if (result.success) {
        LOG_INFO("[CallDeviceNode] Call succeeded: " << device_handle.value() << "/" << function_name.value());
        return BT::NodeStatus::SUCCESS;
    }

    LOG_ERROR("[CallDeviceNode] Call failed: " << result.error_message);
    return BT::NodeStatus::FAILURE;
}

std::optional<BTServiceContext> CallDeviceNode::get_services() const {
    return read_service_context(*this, "CallDeviceNode");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// CheckQualityNode
//-----------------------------------------------------------------------------

CheckQualityNode::CheckQualityNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

BT::PortsList CheckQualityNode::providedPorts() {
    return {BT::InputPort<std::string>("device_handle", "Device handle (provider_id/device_id)"),
            BT::InputPort<std::string>("signal_id", "Signal identifier"),
            BT::InputPort<std::string>("expected_quality", "OK", "Expected quality (default: OK)")};
}

BT::NodeStatus CheckQualityNode::tick() {
    const auto services = get_services();
    if (!services || services->state_cache == nullptr) {
        LOG_ERROR("[CheckQualityNode] StateCache not available in BT service context");
        return BT::NodeStatus::FAILURE;
    }

    // Get input ports
    auto device_handle = getInput<std::string>("device_handle");
    auto signal_id = getInput<std::string>("signal_id");
    auto expected_quality_str = getInput<std::string>("expected_quality").value_or("OK");

    if (!device_handle || !signal_id) {
        LOG_ERROR("[CheckQualityNode] Missing required input ports");
        return BT::NodeStatus::FAILURE;
    }

    // Read signal from StateCache
    auto signal_value = services->state_cache->get_signal_value(device_handle.value(), signal_id.value());

    if (!signal_value) {
        LOG_ERROR("[CheckQualityNode] Signal not found: " << device_handle.value() << "/" << signal_id.value());
        return BT::NodeStatus::FAILURE;
    }

    // Check quality
    auto expected_quality = string_to_quality(expected_quality_str);

    if (signal_value->quality == expected_quality) {
        return BT::NodeStatus::SUCCESS;
    }

    LOG_INFO("[CheckQualityNode] Quality mismatch: expected " << expected_quality_str << ", got "
                                                              << quality_to_string(signal_value->quality));
    return BT::NodeStatus::FAILURE;
}

std::optional<BTServiceContext> CheckQualityNode::get_services() const {
    return read_service_context(*this, "CheckQualityNode");
}

//-----------------------------------------------------------------------------
// GetParameterNode
//-----------------------------------------------------------------------------

GetParameterNode::GetParameterNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList GetParameterNode::providedPorts() {
    return {BT::InputPort<std::string>("param", "Parameter name"),
            BT::OutputPort<double>("value", "Parameter value (numeric only: double or int64)")};
}

BT::NodeStatus GetParameterNode::tick() {
    const auto services = get_services();
    if (!services || services->parameter_manager == nullptr) {
        LOG_ERROR("[GetParameterNode] ParameterManager not available in BT service context");
        return BT::NodeStatus::FAILURE;
    }

    // Get input port
    auto param_name = getInput<std::string>("param");
    if (!param_name) {
        LOG_ERROR("[GetParameterNode] Missing 'param' input port");
        return BT::NodeStatus::FAILURE;
    }

    // Read parameter value
    auto value_opt = services->parameter_manager->get(param_name.value());
    if (!value_opt.has_value()) {
        LOG_ERROR("[GetParameterNode] Parameter not found: " << param_name.value());
        return BT::NodeStatus::FAILURE;
    }

    // Explicit contract: GetParameter exposes numeric values only.
    // String/bool parameters must be handled by dedicated BT nodes in a future extension.
    double output_value = 0.0;
    const auto &param_value = value_opt.value();

    if (std::holds_alternative<double>(param_value)) {
        output_value = std::get<double>(param_value);
    } else if (std::holds_alternative<int64_t>(param_value)) {
        output_value = static_cast<double>(std::get<int64_t>(param_value));
    } else {
        LOG_ERROR("[GetParameterNode] Parameter '" << param_name.value() << "' has non-numeric type '"
                                                   << parameter_value_type_name(param_value) << "'");
        return BT::NodeStatus::FAILURE;
    }

    setOutput("value", output_value);

    LOG_INFO("[GetParameterNode] Read parameter '" << param_name.value() << "' = " << output_value);

    return BT::NodeStatus::SUCCESS;
}

std::optional<BTServiceContext> GetParameterNode::get_services() const {
    return read_service_context(*this, "GetParameterNode");
}

//-----------------------------------------------------------------------------
// GetParameterBoolNode
//-----------------------------------------------------------------------------

GetParameterBoolNode::GetParameterBoolNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList GetParameterBoolNode::providedPorts() {
    return {BT::InputPort<std::string>("param", "Parameter name"),
            BT::OutputPort<bool>("value", "Parameter value (bool)")};
}

BT::NodeStatus GetParameterBoolNode::tick() {
    const auto services = get_services();
    if (!services || services->parameter_manager == nullptr) {
        LOG_ERROR("[GetParameterBoolNode] ParameterManager not available in BT service context");
        return BT::NodeStatus::FAILURE;
    }

    auto param_name = getInput<std::string>("param");
    if (!param_name) {
        LOG_ERROR("[GetParameterBoolNode] Missing 'param' input port");
        return BT::NodeStatus::FAILURE;
    }

    auto value_opt = services->parameter_manager->get(param_name.value());
    if (!value_opt.has_value()) {
        LOG_ERROR("[GetParameterBoolNode] Parameter not found: " << param_name.value());
        return BT::NodeStatus::FAILURE;
    }

    const auto &param_value = value_opt.value();
    if (!std::holds_alternative<bool>(param_value)) {
        LOG_ERROR("[GetParameterBoolNode] Parameter '" << param_name.value() << "' has non-bool type '"
                                                       << parameter_value_type_name(param_value) << "'");
        return BT::NodeStatus::FAILURE;
    }

    setOutput("value", std::get<bool>(param_value));
    return BT::NodeStatus::SUCCESS;
}

std::optional<BTServiceContext> GetParameterBoolNode::get_services() const {
    return read_service_context(*this, "GetParameterBoolNode");
}

//-----------------------------------------------------------------------------
// GetParameterInt64Node
//-----------------------------------------------------------------------------

GetParameterInt64Node::GetParameterInt64Node(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList GetParameterInt64Node::providedPorts() {
    return {BT::InputPort<std::string>("param", "Parameter name"),
            BT::OutputPort<int64_t>("value", "Parameter value (int64)")};
}

BT::NodeStatus GetParameterInt64Node::tick() {
    const auto services = get_services();
    if (!services || services->parameter_manager == nullptr) {
        LOG_ERROR("[GetParameterInt64Node] ParameterManager not available in BT service context");
        return BT::NodeStatus::FAILURE;
    }

    auto param_name = getInput<std::string>("param");
    if (!param_name) {
        LOG_ERROR("[GetParameterInt64Node] Missing 'param' input port");
        return BT::NodeStatus::FAILURE;
    }

    auto value_opt = services->parameter_manager->get(param_name.value());
    if (!value_opt.has_value()) {
        LOG_ERROR("[GetParameterInt64Node] Parameter not found: " << param_name.value());
        return BT::NodeStatus::FAILURE;
    }

    const auto &param_value = value_opt.value();
    if (!std::holds_alternative<int64_t>(param_value)) {
        LOG_ERROR("[GetParameterInt64Node] Parameter '" << param_name.value() << "' has non-int64 type '"
                                                        << parameter_value_type_name(param_value) << "'");
        return BT::NodeStatus::FAILURE;
    }

    setOutput("value", std::get<int64_t>(param_value));
    return BT::NodeStatus::SUCCESS;
}

std::optional<BTServiceContext> GetParameterInt64Node::get_services() const {
    return read_service_context(*this, "GetParameterInt64Node");
}

//-----------------------------------------------------------------------------
// CheckBoolNode
//-----------------------------------------------------------------------------

CheckBoolNode::CheckBoolNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList CheckBoolNode::providedPorts() {
    return {BT::InputPort<bool>("value", "Boolean value to compare"),
            BT::InputPort<bool>("expected", true, "Expected value (default: true)")};
}

BT::NodeStatus CheckBoolNode::tick() {
    const auto value = getInput<bool>("value");
    if (!value) {
        LOG_ERROR("[CheckBoolNode] Missing required input 'value'");
        return BT::NodeStatus::FAILURE;
    }

    const bool expected = getInput<bool>("expected").value_or(true);
    return (value.value() == expected) ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
}

//-----------------------------------------------------------------------------
// PeriodicPulseWindowNode
//-----------------------------------------------------------------------------

PeriodicPulseWindowNode::PeriodicPulseWindowNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList PeriodicPulseWindowNode::providedPorts() {
    return {BT::InputPort<bool>("enabled", true, "Enable periodic pulse scheduling"),
            BT::InputPort<int64_t>("startup_delay_s", 0, "Initial delay before first pulse (seconds)"),
            BT::InputPort<int64_t>("interval_s", 60, "Pulse interval (seconds)"),
            BT::InputPort<int64_t>("pulse_s", 1, "Pulse width (seconds)"),
            BT::InputPort<int64_t>("max_pulses_per_hour", 0, "Optional cap (0=disabled)"),
            BT::InputPort<int64_t>("now_ms", "Optional time override for deterministic testing"),
            BT::OutputPort<bool>("active", "True when inside active pulse window"),
            BT::OutputPort<int64_t>("pulse_index", "Current pulse index after startup delay"),
            BT::OutputPort<int64_t>("elapsed_ms", "Milliseconds elapsed since node enabled")};
}

BT::NodeStatus PeriodicPulseWindowNode::tick() {
    const bool enabled = getInput<bool>("enabled").value_or(true);
    const int64_t startup_delay_s = getInput<int64_t>("startup_delay_s").value_or(0);
    const int64_t interval_s = getInput<int64_t>("interval_s").value_or(60);
    const int64_t pulse_s = getInput<int64_t>("pulse_s").value_or(1);
    const int64_t max_pulses_per_hour = getInput<int64_t>("max_pulses_per_hour").value_or(0);
    const int64_t now_ms = getInput<int64_t>("now_ms").value_or(current_time_ms());

    if (startup_delay_s < 0 || interval_s <= 0 || pulse_s <= 0 || pulse_s > interval_s || max_pulses_per_hour < 0) {
        LOG_ERROR("[PeriodicPulseWindowNode] Invalid scheduling inputs");
        return BT::NodeStatus::FAILURE;
    }

    if (!enabled) {
        initialized_ = false;
        setOutput("active", false);
        setOutput("pulse_index", int64_t{-1});
        setOutput("elapsed_ms", int64_t{0});
        return BT::NodeStatus::SUCCESS;
    }

    if (!initialized_) {
        initialized_ = true;
        enabled_at_ms_ = now_ms;
    }

    int64_t elapsed_ms = now_ms - enabled_at_ms_;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    const int64_t startup_delay_ms = startup_delay_s * 1000;
    if (elapsed_ms < startup_delay_ms) {
        setOutput("active", false);
        setOutput("pulse_index", int64_t{-1});
        setOutput("elapsed_ms", elapsed_ms);
        return BT::NodeStatus::SUCCESS;
    }

    const int64_t pulse_ms = pulse_s * 1000;
    int64_t interval_ms = interval_s * 1000;

    if (max_pulses_per_hour > 0) {
        const int64_t min_interval_by_cap_ms = (3600 * 1000) / max_pulses_per_hour;
        if (interval_ms < min_interval_by_cap_ms) {
            interval_ms = min_interval_by_cap_ms;
        }
    }

    const int64_t effective_ms = elapsed_ms - startup_delay_ms;
    const int64_t pulse_index = effective_ms / interval_ms;
    const int64_t offset_ms = effective_ms % interval_ms;
    const bool active = offset_ms < pulse_ms;

    setOutput("active", active);
    setOutput("pulse_index", pulse_index);
    setOutput("elapsed_ms", elapsed_ms);
    return BT::NodeStatus::SUCCESS;
}

//-----------------------------------------------------------------------------
// EmitOnChangeOrIntervalNode
//-----------------------------------------------------------------------------

EmitOnChangeOrIntervalNode::EmitOnChangeOrIntervalNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList EmitOnChangeOrIntervalNode::providedPorts() {
    return {BT::InputPort<std::string>("key", "Deterministic command key/signature"),
            BT::InputPort<int64_t>("keepalive_s", 30, "Emit at least this often (seconds)"),
            BT::InputPort<int64_t>("min_spacing_ms", 0, "Minimum spacing between emissions"),
            BT::InputPort<bool>("force", false, "Force immediate emission"),
            BT::InputPort<int64_t>("now_ms", "Optional time override for deterministic testing"),
            BT::OutputPort<bool>("emit", "True if command should be emitted this tick"),
            BT::OutputPort<std::string>("reason", "Emission decision reason")};
}

BT::NodeStatus EmitOnChangeOrIntervalNode::tick() {
    const auto key = getInput<std::string>("key");
    if (!key) {
        LOG_ERROR("[EmitOnChangeOrIntervalNode] Missing required input 'key'");
        return BT::NodeStatus::FAILURE;
    }

    const int64_t keepalive_s = getInput<int64_t>("keepalive_s").value_or(30);
    const int64_t min_spacing_ms = getInput<int64_t>("min_spacing_ms").value_or(0);
    const bool force = getInput<bool>("force").value_or(false);
    const int64_t now_ms = getInput<int64_t>("now_ms").value_or(current_time_ms());

    if (keepalive_s < 0 || min_spacing_ms < 0) {
        LOG_ERROR("[EmitOnChangeOrIntervalNode] keepalive_s and min_spacing_ms must be >= 0");
        return BT::NodeStatus::FAILURE;
    }

    bool emit = false;
    std::string reason = "hold";

    if (force) {
        emit = true;
        reason = "force";
    } else if (!has_last_emit_) {
        emit = true;
        reason = "first";
    } else {
        const int64_t since_last_ms = now_ms - last_emit_ms_;
        const bool changed = key.value() != last_key_;
        if (changed && since_last_ms >= min_spacing_ms) {
            emit = true;
            reason = "changed";
        } else if ((keepalive_s > 0) && since_last_ms >= (keepalive_s * 1000)) {
            emit = true;
            reason = "keepalive";
        }
    }

    if (emit) {
        has_last_emit_ = true;
        last_emit_ms_ = now_ms;
        last_key_ = key.value();
    }

    setOutput("emit", emit);
    setOutput("reason", reason);
    return BT::NodeStatus::SUCCESS;
}

//-----------------------------------------------------------------------------
// BuildArgsJsonNode
//-----------------------------------------------------------------------------

BuildArgsJsonNode::BuildArgsJsonNode(const std::string &name, const BT::NodeConfig &config)
    : BT::SyncActionNode(name, config) {}

/*static*/ BT::PortsList BuildArgsJsonNode::providedPorts() {
    BT::PortsList ports = {BT::OutputPort<std::string>("json", "Output JSON object string")};
    for (int i = 1; i <= 6; ++i) {
        const std::string slot = std::to_string(i);
        ports.insert(BT::InputPort<std::string>("arg" + slot + "_name", "", "Argument key name"));
        ports.insert(BT::InputPort<std::string>("arg" + slot + "_type", "",
                                                "Optional type override: int64|double|bool|string"));
        ports.insert(BT::InputPort<double>("arg" + slot + "_num", "Numeric argument value"));
        ports.insert(BT::InputPort<bool>("arg" + slot + "_bool", "Boolean argument value"));
        ports.insert(BT::InputPort<std::string>("arg" + slot + "_str", "String argument value"));
    }
    return ports;
}

BT::NodeStatus BuildArgsJsonNode::tick() {
    nlohmann::json args = nlohmann::json::object();

    for (int i = 1; i <= 6; ++i) {
        const std::string slot = std::to_string(i);
        const auto arg_name = getInput<std::string>("arg" + slot + "_name");
        if (!arg_name || arg_name.value().empty()) {
            continue;
        }

        const auto arg_type = getInput<std::string>("arg" + slot + "_type").value_or("");
        const auto num_value = getInput<double>("arg" + slot + "_num");
        const auto bool_value = getInput<bool>("arg" + slot + "_bool");
        const auto str_value = getInput<std::string>("arg" + slot + "_str");

        int provided_count = 0;
        provided_count += num_value.has_value() ? 1 : 0;
        provided_count += bool_value.has_value() ? 1 : 0;
        provided_count += str_value.has_value() ? 1 : 0;
        if (provided_count == 0) {
            LOG_ERROR("[BuildArgsJsonNode] Missing value for " << arg_name.value());
            return BT::NodeStatus::FAILURE;
        }
        if (provided_count > 1) {
            LOG_ERROR("[BuildArgsJsonNode] Multiple value sources for " << arg_name.value());
            return BT::NodeStatus::FAILURE;
        }

        if (arg_type == "int64") {
            if (!num_value.has_value()) {
                LOG_ERROR("[BuildArgsJsonNode] int64 type requires numeric input for " << arg_name.value());
                return BT::NodeStatus::FAILURE;
            }
            const double raw = num_value.value();
            const auto rounded = std::llround(raw);
            if (std::fabs(raw - static_cast<double>(rounded)) > 1e-9) {
                LOG_ERROR("[BuildArgsJsonNode] Non-integral numeric value for int64 arg " << arg_name.value());
                return BT::NodeStatus::FAILURE;
            }
            args[arg_name.value()] = rounded;
        } else if (arg_type == "double") {
            if (!num_value.has_value()) {
                LOG_ERROR("[BuildArgsJsonNode] double type requires numeric input for " << arg_name.value());
                return BT::NodeStatus::FAILURE;
            }
            args[arg_name.value()] = num_value.value();
        } else if (arg_type == "bool") {
            if (!bool_value.has_value()) {
                LOG_ERROR("[BuildArgsJsonNode] bool type requires bool input for " << arg_name.value());
                return BT::NodeStatus::FAILURE;
            }
            args[arg_name.value()] = bool_value.value();
        } else if (arg_type == "string") {
            if (!str_value.has_value()) {
                LOG_ERROR("[BuildArgsJsonNode] string type requires string input for " << arg_name.value());
                return BT::NodeStatus::FAILURE;
            }
            args[arg_name.value()] = str_value.value();
        } else if (!arg_type.empty()) {
            LOG_ERROR("[BuildArgsJsonNode] Unsupported arg type '" << arg_type << "' for " << arg_name.value());
            return BT::NodeStatus::FAILURE;
        } else if (num_value.has_value()) {
            args[arg_name.value()] = num_value.value();
        } else if (bool_value.has_value()) {
            args[arg_name.value()] = bool_value.value();
        } else {
            args[arg_name.value()] = str_value.value();
        }
    }

    setOutput("json", args.dump());
    return BT::NodeStatus::SUCCESS;
}

}  // namespace automation
}  // namespace anolis
