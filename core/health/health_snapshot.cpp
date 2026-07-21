#include "health_snapshot.hpp"

#include <chrono>
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

std::vector<ProviderHealthSnapshot> collect_providers_health(provider::ProviderRegistry &provider_registry,
                                                             registry::DeviceRegistry &device_registry,
                                                             state::StateCache &state_cache,
                                                             provider::ProviderSupervisor *supervisor) {
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

        bool is_available = provider->is_available();
        ps.state = is_available ? "AVAILABLE" : "UNAVAILABLE";

        auto devices = device_registry.get_devices_for_provider(provider_id);
        ps.device_count = devices.size();

        // Provider-reported ADPP health (#185), fetched on demand: the
        // provider's own view — per-device metrics and last_seen, plus devices
        // it expected but could not bring up (absent from the registry,
        // reported UNREACHABLE by the provider).
        adpp::GetHealthResponse reported;
        const bool has_reported = is_available && provider->get_health(reported);
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

                auto now = system_clock::now();
                auto age_ms = duration_cast<milliseconds>(now - device_state->last_poll_time).count();
                if (age_ms < 0) {
                    // Backward system-clock step (RTC-less Pi + NTP): a negative
                    // age would wrap uint64_t and overflow Influx's int64.
                    age_ms = 0;
                }

                ds.staleness_ms = age_ms;

                // Determine health based on staleness
                // OK: < 2 seconds, WARNING: 2-5 seconds, STALE: > 5 seconds
                if (!is_available) {
                    ds.health = "UNAVAILABLE";
                } else if (age_ms < 2000) {
                    ds.health = "OK";
                } else if (age_ms < 5000) {
                    ds.health = "WARNING";
                } else {
                    ds.health = "STALE";
                }
            } else {
                ds.health = "UNKNOWN";
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

}  // namespace health
}  // namespace anolis
