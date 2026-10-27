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

std::string quality_name(anolis::deviceprovider::v1::SignalValue_Quality quality) {
    const auto &name = anolis::deviceprovider::v1::SignalValue_Quality_Name(quality);
    return name.empty() ? std::to_string(static_cast<int>(quality)) : name;
}

// The status signal and a reports_estop device's estop are judged only when
// their provider asserted QUALITY_OK. Unlike device health, it does not treat UNSPECIFIED (a provider that never
// sets quality) or an unrecognised value as OK (#526).
bool is_ok(const state::CachedSignalValue &value) {
    return value.quality == anolis::deviceprovider::v1::SignalValue_Quality_QUALITY_OK;
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
        return {StopChainState::Unknown, std::format("{} is stale ({} ms old, quality {})", signal,
                                                     value->age().count(), quality_name(value->quality))};
    }
    if (!is_ok(*value)) {
        return {StopChainState::Unknown,
                std::format("{} has quality {}, not QUALITY_OK", signal, quality_name(value->quality))};
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

std::optional<std::string> StopChainMonitor::contradiction(const std::string &device_handle,
                                                           runtime::StopChainExpect expect) const {
    // Judged only once the stop has been in force for the stale bound, so a
    // reading still fresh now was taken after the stop began.
    if (expect == runtime::StopChainExpect::PowerLoss) {
        // Any fresh reading is a device that still answers, whatever quality
        // its provider claims: only STALE, FAULT or UNKNOWN quality, or age,
        // makes it silent. QUALITY_OK is not required here (#526).
        const auto device = cache_.get_device_state(device_handle);
        if (device == nullptr) {
            return std::nullopt;
        }
        for (const auto &[signal_id, value] : device->signals) {
            if (!value.is_stale(stale_after_)) {
                return std::format("{} still answers: {} read {} ms ago, after the stop", device_handle, signal_id,
                                   value.age().count());
            }
        }
        return std::nullopt;
    }
    // reports_estop: only a fresh, OK estop=false contradicts it. No reading, or
    // a stale one, is a device that may have lost power, not the stop failing.
    const auto estop = cache_.get_signal_value(device_handle, "estop");
    if (estop == nullptr || !is_ok(*estop) || estop->is_stale(stale_after_) || !estop->value.has_bool_value() ||
        estop->value.bool_value()) {
        return std::nullopt;
    }
    return std::format("{}/estop reads false", device_handle);
}

std::vector<StopChainMismatchChange> StopChainMonitor::update_mismatches() {
    if (!configured() || config_.expectations.empty()) {
        return {};
    }
    const auto chain = snapshot();
    const int64_t now = now_epoch_ms();

    // Judged once the stop has been in force for the stale bound: until then a
    // device that lost power can still have a fresh reading, and estop can lag
    // a poll.
    std::map<std::string, StopChainMismatch> found;
    if (chain.state == StopChainState::InForce && chain.since_epoch_ms.has_value() &&
        now - *chain.since_epoch_ms >= stale_after_.count()) {
        for (const auto &[handle, expect] : config_.expectations) {
            if (auto reason = contradiction(handle, expect)) {
                found.emplace(handle, StopChainMismatch{handle, expect, std::move(*reason), now});
            }
        }
    }

    std::vector<StopChainMismatchChange> changes;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = mismatches_.begin(); it != mismatches_.end();) {
        if (found.contains(it->first)) {
            ++it;
            continue;
        }
        StopChainMismatch resolved = it->second;
        resolved.reason = chain.state == StopChainState::InForce
                              ? std::format("{} now behaves as expected", resolved.device)
                              : std::string("the stop is no longer in force");
        resolved.since_epoch_ms = now;
        changes.push_back({std::move(resolved), false});
        it = mismatches_.erase(it);
    }
    for (auto &[handle, mismatch] : found) {
        const auto standing = mismatches_.find(handle);
        if (standing != mismatches_.end()) {
            standing->second.reason = std::move(mismatch.reason);  // e.g. a read age that changes
            continue;
        }
        changes.push_back({mismatch, true});
        mismatches_.emplace(handle, std::move(mismatch));
    }
    return changes;
}

std::vector<StopChainMismatch> StopChainMonitor::mismatches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StopChainMismatch> standing;
    standing.reserve(mismatches_.size());
    for (const auto &[handle, mismatch] : mismatches_) {
        (void)handle;
        standing.push_back(mismatch);
    }
    return standing;
}

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
