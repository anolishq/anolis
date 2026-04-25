#include "provider_supervisor.hpp"

#include "../logging/logger.hpp"

namespace anolis {
namespace provider {

void ProviderSupervisor::register_provider(const std::string &provider_id, const RestartPolicyConfig &policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policies_[provider_id] = policy;
    states_[provider_id] = RestartState{};

    if (policy.enabled) {
        LOG_INFO("[Supervisor] Registered provider '"
                 << provider_id << "' with restart policy (max_attempts=" << policy.max_attempts << ")");
    } else {
        LOG_DEBUG("[Supervisor] Registered provider '" << provider_id << "' without restart policy");
    }
}

bool ProviderSupervisor::should_restart(const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto policy_it = policies_.find(provider_id);
    auto state_it = states_.find(provider_id);

    if (policy_it == policies_.end() || state_it == states_.end()) {
        return false;  // Unknown provider
    }

    const auto &policy = policy_it->second;
    const auto &state = state_it->second;

    if (!policy.enabled) {
        return false;
    }

    if (state.circuit_open) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    if (now < state.next_restart_time) {
        return false;
    }

    return true;
}

int ProviderSupervisor::get_backoff_ms(const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto policy_it = policies_.find(provider_id);
    auto state_it = states_.find(provider_id);

    if (policy_it == policies_.end() || state_it == states_.end()) {
        return 0;
    }

    const auto &policy = policy_it->second;
    const auto &state = state_it->second;

    if (state.attempt_count == 0 || state.circuit_open) {
        return 0;
    }

    int attempt_index = state.attempt_count - 1;
    if (attempt_index < 0) {
        return 0;
    }
    // Clamp to last entry: if backoff_ms is shorter than max_attempts, use the
    // last element as the steady-state backoff rather than going out-of-bounds.
    if (static_cast<size_t>(attempt_index) >= policy.backoff_ms.size()) {
        attempt_index = static_cast<int>(policy.backoff_ms.size()) - 1;
    }
    return policy.backoff_ms[attempt_index];
}

bool ProviderSupervisor::record_crash(const std::string &provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto policy_it = policies_.find(provider_id);
    auto state_it = states_.find(provider_id);

    if (policy_it == policies_.end() || state_it == states_.end()) {
        return false;
    }

    const auto &policy = policy_it->second;
    auto &state = state_it->second;

    if (!policy.enabled) {
        state.circuit_open = true;
        LOG_INFO("[Supervisor] Provider '" << provider_id << "' crashed (restart policy disabled)");
        return false;
    }

    state.attempt_count++;
    // Crash ends the current process uptime window. The next healthy heartbeat
    // after restart will establish a fresh process_start_time.
    state.process_start_time = std::chrono::steady_clock::time_point{};

    if (state.attempt_count > policy.max_attempts) {
        state.circuit_open = true;
        LOG_ERROR("[Supervisor] Provider '" << provider_id << "' crashed (circuit breaker open, exceeded "
                                            << policy.max_attempts << " restart attempts)");
        return false;
    }

    int attempt_index = state.attempt_count - 1;
    // Clamp to last entry: if backoff_ms is shorter than max_attempts, use the
    // last element as the steady-state backoff rather than going out-of-bounds.
    if (static_cast<size_t>(attempt_index) >= policy.backoff_ms.size()) {
        attempt_index = static_cast<int>(policy.backoff_ms.size()) - 1;
    }
    int backoff_ms = policy.backoff_ms[attempt_index];

    auto now = std::chrono::steady_clock::now();
    state.next_restart_time = now + std::chrono::milliseconds(backoff_ms);

    LOG_WARN("[Supervisor] Provider '" << provider_id << "' crashed (attempt " << state.attempt_count << "/"
                                       << policy.max_attempts << ", retry in " << backoff_ms << "ms)");

    return true;
}

bool ProviderSupervisor::is_circuit_open(const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return false;
    }

    return state_it->second.circuit_open;
}

int ProviderSupervisor::get_attempt_count(const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return 0;
    }

    return state_it->second.attempt_count;
}

void ProviderSupervisor::record_success(const std::string &provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return;
    }

    auto &state = state_it->second;

    if (state.attempt_count > 0) {
        LOG_INFO("[Supervisor] Provider '" << provider_id << "' recovered successfully (after " << state.attempt_count
                                           << " restart attempts)");
    }

    state.attempt_count = 0;
    state.circuit_open = false;
    state.crash_detected = false;
    state.next_restart_time = std::chrono::steady_clock::time_point{};
}

bool ProviderSupervisor::should_mark_recovered(const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto policy_it = policies_.find(provider_id);
    auto state_it = states_.find(provider_id);

    if (policy_it == policies_.end() || state_it == states_.end()) {
        return false;
    }

    const auto &policy = policy_it->second;
    const auto &state = state_it->second;

    if (!policy.enabled || state.circuit_open || state.attempt_count == 0) {
        return false;
    }

    if (state.process_start_time == std::chrono::steady_clock::time_point{}) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto stable_for =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.process_start_time).count();
    return stable_for >= policy.success_reset_ms;
}

bool ProviderSupervisor::mark_crash_detected(const std::string &provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return false;
    }

    auto &state = state_it->second;

    if (state.crash_detected) {
        return false;
    }

    state.crash_detected = true;
    return true;
}

void ProviderSupervisor::clear_crash_detected(const std::string &provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return;
    }

    state_it->second.crash_detected = false;
}

void ProviderSupervisor::record_heartbeat(const std::string &provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.find(provider_id);

    if (state_it == states_.end()) {
        return;
    }

    auto &state = state_it->second;
    auto now = std::chrono::steady_clock::now();

    // Set process_start_time on the first heartbeat after initial start or after a crash/restart.
    if (state.process_start_time == std::chrono::steady_clock::time_point{}) {
        state.process_start_time = now;
    }

    state.last_healthy_time = now;
}

std::optional<ProviderSupervisor::ProviderSupervisionSnapshot> ProviderSupervisor::get_snapshot(
    const std::string &provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto policy_it = policies_.find(provider_id);
    auto state_it = states_.find(provider_id);

    if (policy_it == policies_.end() || state_it == states_.end()) {
        return std::nullopt;
    }

    const auto &policy = policy_it->second;
    const auto &state = state_it->second;
    const auto now = std::chrono::steady_clock::now();

    ProviderSupervisionSnapshot snap;
    snap.supervision_enabled = policy.enabled;
    snap.attempt_count = state.attempt_count;
    snap.max_attempts = policy.max_attempts;
    snap.crash_detected = state.crash_detected;
    snap.circuit_open = state.circuit_open;

    // next_restart_in_ms: nullopt when healthy (no crash) or circuit open
    if (state.circuit_open || state.attempt_count == 0) {
        snap.next_restart_in_ms = std::nullopt;
    } else if (now >= state.next_restart_time) {
        snap.next_restart_in_ms = int64_t{0};
    } else {
        snap.next_restart_in_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(state.next_restart_time - now).count();
    }

    // last_seen_ago_ms: nullopt if this provider has never had a healthy heartbeat
    if (state.last_healthy_time == std::chrono::steady_clock::time_point{}) {
        snap.last_seen_ago_ms = std::nullopt;
    } else {
        snap.last_seen_ago_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_healthy_time).count();
    }

    // uptime_seconds: 0 before first heartbeat; computed from process_start_time otherwise.
    // The HTTP handler additionally forces this to 0 when the provider is UNAVAILABLE.
    if (state.process_start_time == std::chrono::steady_clock::time_point{}) {
        snap.uptime_seconds = 0;
    } else {
        snap.uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - state.process_start_time).count();
    }

    return snap;
}

std::unordered_map<std::string, ProviderSupervisor::ProviderSupervisionSnapshot> ProviderSupervisor::get_all_snapshots()
    const {
    std::unordered_map<std::string, ProviderSupervisionSnapshot> result;
    // unlock quickly: collect IDs, then call get_snapshot for each
    // get_snapshot locks internally, so we don't hold the lock during the whole loop.
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.reserve(states_.size());
        for (const auto &[id, _] : states_) {
            ids.push_back(id);
        }
    }

    result.reserve(ids.size());
    for (const auto &id : ids) {
        auto snap = get_snapshot(id);
        if (snap) {
            result.emplace(id, *snap);
        }
    }

    return result;
}

}  // namespace provider
}  // namespace anolis
