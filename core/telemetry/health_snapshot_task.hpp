#pragma once

/**
 * @file health_snapshot_task.hpp
 * @brief Periodic provider/device health ingestion into InfluxDB (#203).
 *
 * Every `interval_ms` this task collects the shared health snapshot (the same
 * view `GET /v0/providers/health` serves) and enqueues it on the InfluxSink as
 * `anolis_provider_health` / `anolis_device_health` line protocol.
 *
 * Cadence is a real cost decision, not a free parameter: each snapshot is a
 * live ADPP GetHealth round-trip per provider, and bread answers that with a
 * live GET_WATCHDOG bus transaction per armed device.
 */

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "../health/health_snapshot.hpp"

namespace anolis {

namespace provider {
class ProviderRegistry;
class ProviderSupervisor;
}  // namespace provider

namespace registry {
class DeviceRegistry;
}

namespace state {
class StateCache;
}

namespace telemetry {

class InfluxSink;

class HealthSnapshotTask {
public:
    HealthSnapshotTask(int interval_ms, std::string runtime_name, provider::ProviderRegistry &provider_registry,
                       registry::DeviceRegistry &device_registry, state::StateCache &state_cache,
                       provider::ProviderSupervisor *supervisor, InfluxSink &sink,
                       health::StalenessPolicy staleness_policy);

    ~HealthSnapshotTask();

    // Non-copyable
    HealthSnapshotTask(const HealthSnapshotTask &) = delete;
    HealthSnapshotTask &operator=(const HealthSnapshotTask &) = delete;

    void start();

    /**
     * @brief Request stop and join.
     *
     * Interrupts the interval sleep immediately, but an in-flight snapshot
     * completes first — bounded by the per-provider ADPP request timeout.
     */
    void stop();

private:
    void loop();

    int interval_ms_;
    std::string runtime_name_;
    provider::ProviderRegistry &provider_registry_;
    registry::DeviceRegistry &device_registry_;
    state::StateCache &state_cache_;
    provider::ProviderSupervisor *supervisor_;
    InfluxSink &sink_;
    health::StalenessPolicy staleness_policy_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
    std::thread thread_;
};

}  // namespace telemetry
}  // namespace anolis
