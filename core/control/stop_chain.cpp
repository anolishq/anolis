#include "control/stop_chain.hpp"

#include <algorithm>
#include <format>

namespace anolis {
namespace control {

namespace {

int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char *stop_chain_state_to_string(StopChainState state) {
    switch (state) {
        case StopChainState::InForce:
            return "in_force";
        case StopChainState::Clear:
            return "clear";
        case StopChainState::Unknown:
            return "unknown";
        case StopChainState::NotConfigured:
        default:
            return "not_configured";
    }
}

const char *stop_chain_expect_to_string(runtime::StopChainExpect expect) {
    switch (expect) {
        case runtime::StopChainExpect::PowerLoss:
            return "power_loss";
        case runtime::StopChainExpect::ReportsEstop:
        default:
            return "reports_estop";
    }
}

StopChainMonitor::StopChainMonitor(const runtime::StopChainConfig &config, const state::StateCache &cache,
                                   std::chrono::milliseconds stale_after)
    : config_(config), cache_(cache), stale_after_(stale_after) {
    if (config_.status.has_value()) {
        // The loader checked the provider/device/signal shape.
        const auto &signal = config_.status->signal;
        const auto split = signal.rfind('/');
        device_handle_ = signal.substr(0, split);
        signal_id_ = signal.substr(split + 1);
    }
}

std::pair<StopChainState, std::string> StopChainMonitor::classify() const {
    if (!config_.status.has_value()) {
        return {StopChainState::NotConfigured, ""};
    }
    const auto &signal = config_.status->signal;
    const auto value = cache_.get_signal_value(device_handle_, signal_id_);
    if (value == nullptr) {
        return {StopChainState::Unknown, std::format("{} has no reading (not registered, or not polled yet)", signal)};
    }
    if (value->is_stale(stale_after_)) {
        return {StopChainState::Unknown,
                std::format("{} is stale ({} ms old, quality {})", signal, value->age().count(),
                            anolis::deviceprovider::v1::SignalValue_Quality_Name(value->quality))};
    }
    if (!value->value.has_bool_value()) {
        return {StopChainState::Unknown, std::format("{} is not a bool", signal)};
    }
    if (value->value.bool_value() == config_.status->active) {
        return {StopChainState::InForce, std::format("{} reads {}", signal, value->value.bool_value())};
    }
    return {StopChainState::Clear, std::format("{} reads {}", signal, value->value.bool_value())};
}

std::optional<std::pair<StopChainSnapshot, StopChainSnapshot>> StopChainMonitor::update() {
    if (!configured()) {
        return std::nullopt;
    }
    auto [state, reason] = classify();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state == current_.state && current_.since_epoch_ms.has_value()) {
        current_.reason = std::move(reason);  // e.g. a stale age that grows
        return std::nullopt;
    }
    const StopChainSnapshot before = current_;
    current_ = {state, std::move(reason), now_epoch_ms()};
    return std::make_pair(before, current_);
}

StopChainSnapshot StopChainMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

std::vector<StopChainMismatchChange> StopChainMonitor::update_mismatches() { return {}; }

std::vector<StopChainMismatch> StopChainMonitor::mismatches() const { return {}; }

bool StopChainMonitor::holds() const {
    const auto state = snapshot().state;
    return state == StopChainState::InForce || state == StopChainState::Unknown;
}

std::optional<std::string> StopChainMonitor::clear_refusal() const {
    const auto current = snapshot();
    if (current.state == StopChainState::InForce) {
        return std::format("the emergency stop is in force ({}); release it first", current.reason);
    }
    if (current.state == StopChainState::Unknown) {
        return std::format("the emergency stop's status is unavailable ({}): it may be in force, or power may be lost",
                           current.reason);
    }
    return std::nullopt;
}

bool StopChainMonitor::in_scope(const std::string &device_handle) const {
    return std::find(config_.devices.begin(), config_.devices.end(), device_handle) != config_.devices.end();
}

}  // namespace control
}  // namespace anolis
