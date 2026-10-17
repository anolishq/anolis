#include "health_snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_map>

#include "../provider/i_provider_handle.hpp"
#include "../provider/provider_registry.hpp"
#include "../provider/provider_supervisor.hpp"
#include "../registry/device_registry.hpp"
#include "../state/state_cache.hpp"

namespace anolis {
namespace health {

namespace {

namespace adpp = anolis::deviceprovider::v1;

std::string derive_lifecycle_state(bool is_available,
                                   const provider::ProviderSupervisor::ProviderSupervisionSnapshot &snap) {
    if (is_available) {
        return snap.attempt_count > 0 ? "RECOVERING" : "RUNNING";
    }

    if (snap.circuit_open) {
        return "CIRCUIT_OPEN";
    }

    if (snap.crash_detected || snap.attempt_count > 0 || snap.next_restart_in_ms.has_value()) {
        return "RESTARTING";
    }

    return "DOWN";
}

// Bounds on provider-declared descriptor identity. Generous next to what any
// real provider emits (the largest current provider declares 13 short tags)
// and small enough that a misbehaving one cannot inflate a health response or
// a telemetry batch.
constexpr size_t kMaxIdentityLen = 128;
constexpr size_t kMaxDescriptorTags = 32;

// Truncate to kMaxIdentityLen *bytes*, never mid-codepoint.
//
// Cutting a UTF-8 sequence in half produces invalid UTF-8, and
// nlohmann::json::dump() throws type_error.316 on that — which the HTTP layer
// turns into a 500 for the whole /v0/providers/health response, every provider
// and every device, not just the offending one. So a bound added to protect the
// endpoint from a misbehaving provider would instead let a *well-behaved* one
// take it down with an ordinary non-ASCII label longer than 128 bytes.
std::string clamp_identity(const std::string &value) {
    if (value.size() <= kMaxIdentityLen) {
        return value;
    }
    size_t cut = kMaxIdentityLen;
    // Walk back off any continuation bytes (0b10xxxxxx) to a boundary.
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return value.substr(0, cut);
}

ReportedDeviceHealth reported_device_health(const adpp::DeviceHealth &dh) {
    ReportedDeviceHealth out;
    out.state = adpp::DeviceHealth::State_Name(dh.state());
    out.message = dh.message();
    for (const auto &[key, value] : dh.metrics()) {
        out.metrics[key] = value;
    }
    if (dh.has_last_seen()) {
        out.last_seen_epoch_ms =
            static_cast<int64_t>(dh.last_seen().seconds()) * 1000 + dh.last_seen().nanos() / 1000000;
    }
    return out;
}

}  // namespace

StalenessBounds resolve_staleness_bounds(const StalenessPolicy &policy, size_t device_count) {
    const int64_t interval = policy.poll_interval_ms > 0 ? policy.poll_interval_ms : 500;
    const int64_t cycle_budget = interval * static_cast<int64_t>(std::max<size_t>(1, device_count));

    int64_t warn_ms = policy.warn_after_ms > 0 ? policy.warn_after_ms
                                               : std::max(kStalenessWarnCycles * cycle_budget, kStalenessWarnFloorMs);
    int64_t stale_ms = policy.stale_after_ms > 0
                           ? policy.stale_after_ms
                           : std::max(kStalenessStaleCycles * cycle_budget, kStalenessStaleFloorMs);

    // Ordering clamp: WARNING must never sit past STALE (possible only when one
    // side is an explicit override).
    warn_ms = std::min(warn_ms, stale_ms);
    return {warn_ms, stale_ms};
}

int64_t effective_signal_stale_after_ms(uint32_t declared_ms, int64_t liveness_stale_ms) {
    return std::max(static_cast<int64_t>(declared_ms), liveness_stale_ms);
}

std::vector<std::string> unmatched_excluded_signals(
    const std::map<std::string, std::set<std::string>> &excluded_signals, const registry::DeviceRegistry &registry) {
    std::map<std::string, const registry::RegisteredDevice *> by_handle;
    const auto devices = registry.get_all_devices();
    for (const auto &device : devices) {
        by_handle[device.get_handle()] = &device;
    }

    std::vector<std::string> lines;
    for (const auto &[handle, signals] : excluded_signals) {
        const auto it = by_handle.find(handle);
        if (it == by_handle.end()) {
            lines.push_back(std::format(
                "health.excluded_signals: no registered device '{}'; its exclusions have no effect", handle));
            continue;
        }
        const auto &declared = it->second->capabilities.signals_by_id;
        for (const auto &signal_id : signals) {
            if (declared.find(signal_id) == declared.end()) {
                lines.push_back(
                    std::format("health.excluded_signals: {} declares no signal '{}'; the exclusion has no effect",
                                handle, signal_id));
            }
        }
    }
    return lines;
}

std::vector<ProviderHealthSnapshot> collect_providers_health(
    provider::ProviderRegistry &provider_registry, registry::DeviceRegistry &device_registry,
    state::StateCache &state_cache, provider::ProviderSupervisor *supervisor, const StalenessPolicy &staleness_policy,
    std::optional<std::chrono::system_clock::time_point> fixed_now) {
    using namespace std::chrono;
    using SupervisionSnapshot = provider::ProviderSupervisor::ProviderSupervisionSnapshot;

    // Collect all supervision snapshots once (thread-safe copy).
    std::unordered_map<std::string, SupervisionSnapshot> snapshots;
    if (supervisor) {
        snapshots = supervisor->get_all_snapshots();
    }

    std::vector<ProviderHealthSnapshot> result;

    for (const auto &[provider_id, provider] : provider_registry.get_all_providers()) {
        ProviderHealthSnapshot ps;
        ps.provider_id = provider_id;

        // Provider-reported ADPP health (#185), fetched on demand: the
        // provider's own view — per-device metrics and last_seen, plus devices
        // it expected but could not bring up (absent from the registry,
        // reported UNREACHABLE by the provider).
        adpp::GetHealthResponse reported;
        bool has_reported = provider->is_available() && provider->get_health(reported);

        // GetHealth can wait behind an in-flight call for up to the provider's
        // timeout (#411). Read availability and the clock after it returns, so
        // the answer is no older than the request.
        const bool is_available = provider->is_available();
        has_reported = has_reported && is_available;
        const auto now = fixed_now.value_or(system_clock::now());
        ps.state = is_available ? "AVAILABLE" : "UNAVAILABLE";

        auto devices = device_registry.get_devices_for_provider(provider_id);
        ps.device_count = devices.size();

        // Cadence-derived liveness thresholds (#220): scale with the serialized
        // cycle cost so a healthy multi-device bus stops false-flapping STALE.
        const StalenessBounds bounds = resolve_staleness_bounds(staleness_policy, devices.size());

        std::map<std::string, const adpp::DeviceHealth *> reported_by_id;
        if (has_reported) {
            for (const auto &dh : reported.devices()) {
                reported_by_id[dh.device_id()] = &dh;
                if (dh.state() == adpp::DeviceHealth::STATE_UNREACHABLE ||
                    dh.state() == adpp::DeviceHealth::STATE_FAULT) {
                    ++ps.failed_device_count;
                }
            }
        }

        for (const auto &device : devices) {
            DeviceHealthSnapshot ds;
            ds.device_id = device.device_id;

            std::string device_handle = provider_id + "/" + device.device_id;
            auto device_state = state_cache.get_device_state(device_handle);

            if (device_state) {
                ds.last_poll_ms = duration_cast<milliseconds>(device_state->last_poll_time.time_since_epoch()).count();

                auto age_ms = duration_cast<milliseconds>(now - device_state->last_poll_time).count();
                if (age_ms < 0) {
                    // Backward system-clock step (RTC-less Pi + NTP): a negative
                    // age would wrap uint64_t and overflow Influx's int64.
                    age_ms = 0;
                }

                ds.staleness_ms = age_ms;

                // Liveness health from cadence-derived thresholds (#220):
                // OK < warn <= WARNING < stale <= STALE.
                if (!is_available) {
                    ds.health = "UNAVAILABLE";
                } else if (age_ms < bounds.warn_ms) {
                    ds.health = "OK";
                } else if (age_ms < bounds.stale_ms) {
                    ds.health = "WARNING";
                } else {
                    ds.health = "STALE";
                }

                // Per-signal freshness/quality rollup (#220 phase 2): a device
                // whose poll is fresh can still be serving a cached FAULT/stale
                // sample. Count the degraded signals (truthful even when the
                // provider is unavailable) and, when available, escalate the
                // device health: FAULT beats liveness STALE/WARNING/OK; a
                // freshness-stale signal escalates OK/WARNING to STALE.
                const auto excluded_it = staleness_policy.excluded_signals.find(device_handle);
                for (const auto &[signal_id, cached] : device_state->signals) {
                    // Signals the machine leaves unused (#325): still read and
                    // served, but not evidence about the device.
                    if (excluded_it != staleness_policy.excluded_signals.end() &&
                        excluded_it->second.count(signal_id) > 0) {
                        continue;
                    }
                    const auto spec_it = device.capabilities.signals_by_id.find(signal_id);
                    const uint32_t declared =
                        spec_it != device.capabilities.signals_by_id.end() ? spec_it->second.stale_after_ms : 0;
                    const int64_t effective = effective_signal_stale_after_ms(declared, bounds.stale_ms);
                    if (cached.quality == adpp::SignalValue_Quality_QUALITY_FAULT) {
                        ++ds.fault_signal_count;
                    } else if (cached.quality == adpp::SignalValue_Quality_QUALITY_STALE ||
                               cached.quality == adpp::SignalValue_Quality_QUALITY_UNKNOWN ||
                               cached.age(now).count() > effective) {
                        ++ds.stale_signal_count;
                    }
                }
                if (is_available) {
                    if (ds.fault_signal_count > 0) {
                        ds.health = "FAULT";
                    } else if (ds.stale_signal_count > 0) {
                        ds.health = "STALE";
                    }
                }
            } else {
                ds.health = "UNKNOWN";
            }

            // Descriptor identity (bread#126). Registry-derived, so it is
            // present even when the provider is unavailable and its live health
            // cannot be fetched — which is exactly when knowing what was
            // installed matters most.
            //
            // Bounded on the way in. This is provider-supplied data crossing
            // into a JSON response and a timeseries write, and the only limit
            // upstream is the 1 MiB provider frame — a buggy provider could put
            // a megabyte of identity on every device, re-serialized on every
            // health request and every telemetry tick.
            ds.type_version = clamp_identity(device.capabilities.proto.type_version());
            for (const auto &[key, value] : device.capabilities.proto.tags()) {
                if (ds.descriptor_tags.size() >= kMaxDescriptorTags) {
                    break;
                }
                // Over-long keys are dropped rather than truncated: truncating
                // would silently collide two distinct keys into one entry,
                // last-write-wins, and the survivor would be unidentifiable.
                if (key.size() > kMaxIdentityLen) {
                    continue;
                }
                ds.descriptor_tags[key] = clamp_identity(value);
            }

            const auto reported_it = reported_by_id.find(device.device_id);
            if (reported_it != reported_by_id.end()) {
                ds.reported = reported_device_health(*reported_it->second);
                reported_by_id.erase(reported_it);
            }
            ps.devices.push_back(std::move(ds));
        }

        // Devices the provider reports but the registry does not carry — e.g.
        // expected devices that failed their startup probe. Surfacing them is
        // the point of #185: a "successful" install with a missing device is
        // otherwise invisible here.
        for (const auto &[reported_id, dh] : reported_by_id) {
            DeviceHealthSnapshot ds;
            ds.device_id = reported_id;
            ds.registered = false;
            ds.reported = reported_device_health(*dh);
            ps.devices.push_back(std::move(ds));
        }

        ps.lifecycle_state = is_available ? "RUNNING" : "DOWN";

        auto snap_it = snapshots.find(provider_id);
        if (snap_it != snapshots.end()) {
            const auto &snap = snap_it->second;
            ps.lifecycle_state = derive_lifecycle_state(is_available, snap);

            // last_seen_ago_ms stays unset before the first heartbeat.
            ps.last_seen_ago_ms = snap.last_seen_ago_ms;

            // uptime_seconds forced to 0 when UNAVAILABLE (process may have crashed).
            ps.uptime_seconds = is_available ? snap.uptime_seconds : 0;

            ps.supervision.enabled = snap.supervision_enabled;
            ps.supervision.attempt_count = snap.attempt_count;
            ps.supervision.max_attempts = snap.max_attempts;
            ps.supervision.crash_detected = snap.crash_detected;
            ps.supervision.circuit_open = snap.circuit_open;
            ps.supervision.next_restart_in_ms = snap.next_restart_in_ms;
        }
        // No supervisor or provider not yet registered: SupervisionView defaults
        // already carry the zeroed block the HTTP surface emits.

        // Provider-level reported block + degraded flag (#185). degraded means
        // the provider is up but its own health report says something is wrong:
        // a failed/unreachable device or a self-reported DEGRADED/FAULT state.
        if (has_reported) {
            ReportedProviderHealth rp;
            rp.state = adpp::ProviderHealth::State_Name(reported.provider().state());
            rp.message = reported.provider().message();
            for (const auto &[key, value] : reported.provider().metrics()) {
                rp.metrics[key] = value;
            }
            ps.reported = std::move(rp);
            ps.degraded = ps.failed_device_count > 0 ||
                          reported.provider().state() == adpp::ProviderHealth::STATE_DEGRADED ||
                          reported.provider().state() == adpp::ProviderHealth::STATE_FAULT;
        }

        result.push_back(std::move(ps));
    }

    return result;
}

std::vector<std::string> actuating_devices_with_watchdog_disarmed(provider::ProviderRegistry &provider_registry,
                                                                  const registry::DeviceRegistry &device_registry) {
    std::vector<std::string> out;
    for (const auto &[provider_id, provider] : provider_registry.get_all_providers()) {
        adpp::GetHealthResponse reported;
        if (!provider->is_available() || !provider->get_health(reported)) {
            continue;
        }
        for (const auto &dh : reported.devices()) {
            const auto armed = dh.metrics().find("watchdog_armed");
            if (armed == dh.metrics().end() || armed->second != "false") {
                continue;  // absent means unknown, not disarmed
            }
            const auto device = device_registry.get_device_copy(provider_id, dh.device_id());
            if (!device.has_value()) {
                continue;
            }
            const auto &functions = device->capabilities.functions_by_id;
            const bool actuates = std::any_of(functions.begin(), functions.end(), [](const auto &entry) {
                return entry.second.category == adpp::FunctionPolicy_Category_CATEGORY_ACTUATE;
            });
            if (actuates) {
                out.push_back(provider_id + "/" + dh.device_id());
            }
        }
    }
    return out;
}

}  // namespace health
}  // namespace anolis
