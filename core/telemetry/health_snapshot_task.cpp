#include "health_snapshot_task.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include "../health/health_snapshot.hpp"
#include "../logging/logger.hpp"
#include "health_line_protocol.hpp"
#include "influx_sink.hpp"

namespace anolis {
namespace telemetry {

HealthSnapshotTask::HealthSnapshotTask(int interval_ms, std::string runtime_name,
                                       provider::ProviderRegistry &provider_registry,
                                       registry::DeviceRegistry &device_registry, state::StateCache &state_cache,
                                       provider::ProviderSupervisor *supervisor, InfluxSink &sink)
    : interval_ms_(interval_ms),
      runtime_name_(std::move(runtime_name)),
      provider_registry_(provider_registry),
      device_registry_(device_registry),
      state_cache_(state_cache),
      supervisor_(supervisor),
      sink_(sink) {}

HealthSnapshotTask::~HealthSnapshotTask() { stop(); }

void HealthSnapshotTask::start() {
    if (thread_.joinable()) {
        return;
    }
    stop_requested_ = false;
    thread_ = std::thread(&HealthSnapshotTask::loop, this);
    LOG_INFO("[HealthSnapshot] Ingesting provider/device health every " << interval_ms_ << "ms");
}

void HealthSnapshotTask::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HealthSnapshotTask::loop() {
    using namespace std::chrono;

    while (true) {
        {
            // First emission after one full interval: providers are still
            // coming up at start(), and the HTTP surface covers "right now".
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, milliseconds(interval_ms_), [this] { return stop_requested_; });
            if (stop_requested_) {
                return;
            }
        }

        auto snapshot =
            health::collect_providers_health(provider_registry_, device_registry_, state_cache_, supervisor_);

        const int64_t timestamp_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        std::vector<std::string> lines;
        for (const auto &ps : snapshot) {
            auto provider_lines = format_health_lines(ps, runtime_name_, timestamp_ms);
            lines.insert(lines.end(), std::make_move_iterator(provider_lines.begin()),
                         std::make_move_iterator(provider_lines.end()));
        }

        sink_.enqueue_lines(lines);
    }
}

}  // namespace telemetry
}  // namespace anolis
