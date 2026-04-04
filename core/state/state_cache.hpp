#pragma once

/**
 * @file state_cache.hpp
 * @brief Polled device-state cache used as the runtime read path.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.pb.h"
#include "provider/i_provider_handle.hpp"
#include "provider/provider_registry.hpp"
#include "registry/device_registry.hpp"

// Forward declaration
namespace anolis {
namespace events {
class EventEmitter;
}
}  // namespace anolis

namespace anolis {
namespace state {

/**
 * @brief Cached signal value plus timestamp and provider-reported quality.
 *
 * Staleness is derived from both age and quality. A signal can therefore be
 * stale even when its timestamp is recent if the provider explicitly reports a
 * degraded quality level.
 */
struct CachedSignalValue {
    anolis::deviceprovider::v1::Value value;
    std::chrono::system_clock::time_point timestamp;
    anolis::deviceprovider::v1::SignalValue_Quality quality;

    /**
     * @brief Check whether the cached value should be treated as stale.
     *
     * @param timeout Maximum allowed age before time-based staleness applies
     * @param now Optional current time override, mainly for tests
     * @return true if quality or age marks the value stale
     */
    bool is_stale(std::chrono::milliseconds timeout,
                  std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

    /** @brief Return the elapsed time since the cached timestamp. */
    std::chrono::milliseconds age(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;
};

/**
 * @brief Snapshot of one device's cached state.
 *
 * `provider_available` reflects whether polling could currently reach the
 * provider for this device. It does not guarantee that every signal in the
 * snapshot is present or healthy.
 */
struct DeviceState {
    std::string device_handle;                                   // "provider_id/device_id"
    std::unordered_map<std::string, CachedSignalValue> signals;  // signal_id -> value
    std::chrono::system_clock::time_point last_poll_time;
    bool provider_available;
};

/**
 * @brief Background-polled snapshot cache for live device state.
 *
 * StateCache builds a poll plan from the registry's default signals, executes
 * periodic `ReadSignals` calls, and publishes copy-on-read snapshots for HTTP
 * handlers, automation, and other runtime consumers.
 *
 * Threading:
 * Polling runs on a dedicated background thread when started. Read APIs return
 * copies owned by the caller so readers are isolated from concurrent cache
 * updates.
 *
 * Invariants:
 * Only signals marked as default are included in the periodic poll plan.
 * Successful control calls may trigger a best-effort immediate refresh through
 * `poll_device_now()`.
 */
class StateCache {
public:
    /**
     * @brief Construct the cache against a published device registry.
     *
     * @param registry Device registry used to build the poll plan
     * @param poll_interval_ms Period between poll cycles
     */
    StateCache(const registry::DeviceRegistry &registry, int poll_interval_ms = 500);

    /** @brief Stop polling on destruction if the background thread is active. */
    ~StateCache();

    /**
     * @brief Set event emitter for change notifications
     *
     * When set, StateCache will emit events on value/quality changes.
     * Must be called before start_polling().
     *
     * @param emitter Shared pointer to EventEmitter (can be nullptr to disable)
     */
    void set_event_emitter(const std::shared_ptr<events::EventEmitter> &emitter);

    /**
     * @brief Build poll configuration and empty device snapshots from the registry.
     *
     * Must be called before `poll_once()` or `start_polling()`.
     *
     * @return true if initialization completed
     */
    bool initialize();

    /**
     * @brief Rebuild cached poll configuration for one provider.
     *
     * This is used after provider restart or rediscovery when the published
     * device set may have changed.
     *
     * @param provider_id Provider whose poll plan should be rebuilt
     */
    void rebuild_poll_configs(const std::string &provider_id);

    /**
     * @brief Start the background polling thread.
     *
     * Requires `initialize()` to have completed successfully. If polling is
     * already active, the call is ignored.
     */
    void start_polling(provider::ProviderRegistry &provider_registry);

    /** @brief Stop the background polling thread and wait for it to exit. */
    void stop_polling();

    /**
     * @brief Execute one best-effort poll pass immediately.
     *
     * Requires `initialize()` to have completed successfully.
     */
    void poll_once(provider::ProviderRegistry &provider_registry);

    /**
     * @brief Get a snapshot copy of a device's cached state.
     *
     * @return Shared pointer to a copied snapshot, or `nullptr` if unknown
     */
    std::shared_ptr<DeviceState> get_device_state(const std::string &device_handle) const;

    /**
     * @brief Get a snapshot copy of one cached signal value.
     *
     * @return Shared pointer to a copied signal value, or `nullptr` if unknown
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::shared_ptr<CachedSignalValue> get_signal_value(const std::string &device_handle,
                                                        const std::string &signal_id) const;

    /**
     * @brief Trigger an immediate best-effort refresh for one device.
     *
     * This is primarily used after a successful control call so the cache can
     * observe any resulting state change without waiting for the next full poll
     * cycle.
     */
    void poll_device_now(const std::string &device_handle, provider::ProviderRegistry &provider_registry);

    /** @brief Get the number of devices currently represented in the cache. */
    size_t device_count() const;
    const std::string &last_error() const { return error_; }

private:
    const registry::DeviceRegistry &registry_;
    std::string error_;

    // Event emitter for change notifications
    std::shared_ptr<events::EventEmitter> event_emitter_;

    mutable std::mutex mutex_;

    // Cached state (indexed by device_handle)
    std::unordered_map<std::string, DeviceState> device_states_;

    // Polling configuration (built from registry at init)
    struct PollConfig {
        std::string provider_id;
        std::string device_id;
        std::vector<std::string> signal_ids;  // Default signals only
    };
    std::vector<PollConfig> poll_configs_;

    // Polling control
    std::atomic<bool> polling_active_;
    std::atomic<bool> initialized_;
    std::atomic<bool> preinit_poll_warning_emitted_;
    std::thread polling_thread_;
    std::chrono::milliseconds poll_interval_;

    struct ProviderOutageLogState {
        bool in_outage = false;
        bool provider_missing = false;
        std::chrono::steady_clock::time_point last_log_time{};
    };
    std::unordered_map<std::string, ProviderOutageLogState> provider_outage_log_state_;

    // Helper: Poll single device
    bool poll_device(const std::string &provider_id, const std::string &device_id,
                     const std::vector<std::string> &signal_ids, provider::IProviderHandle &provider);

    // Helper: Update cached values from ReadSignalsResponse (emits events on change)
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void update_device_state(const std::string &device_handle, const std::string &provider_id,
                             const std::string &device_id,
                             const anolis::deviceprovider::v1::ReadSignalsResponse &response);

    // Helper: Check if value changed (uses bitwise comparison for doubles)
    bool value_changed(const anolis::deviceprovider::v1::Value &old_val,
                       const anolis::deviceprovider::v1::Value &new_val) const;

    // Helper: Check if quality changed
    bool quality_changed(anolis::deviceprovider::v1::SignalValue_Quality old_q,
                         anolis::deviceprovider::v1::SignalValue_Quality new_q) const;

    // Helper: Emit state update event
    void emit_state_update(const std::string &provider_id, const std::string &device_id, const std::string &signal_id,
                           const anolis::deviceprovider::v1::Value &value,
                           anolis::deviceprovider::v1::SignalValue_Quality quality);
};

}  // namespace state
}  // namespace anolis
