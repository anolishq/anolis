#pragma once

/**
 * @file stop_chain.hpp
 * @brief Observing the emergency stop through a signal any provider publishes (#483).
 *
 * The runtime reads `safety.stop_chain.status.signal` from the state cache and
 * classifies it. It never drives the chain, and no stop's effect depends on it:
 * the contactor or a rated STO is the safety function. What the runtime does
 * with the reading is hold: while the stop is in force, or its status cannot be
 * read, the software stop's latch stays engaged and cannot be cleared.
 *
 * A source that is stale, unreadable, unregistered or not a bool reads as
 * `Unknown`, never `Clear`.
 */

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/config.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace control {

enum class StopChainState {
    NotConfigured,  // no safety.stop_chain.status: behaviour as before
    InForce,
    Clear,
    Unknown,
};

const char *stop_chain_state_to_string(StopChainState state);

const char *stop_chain_expect_to_string(runtime::StopChainExpect expect);

/** @brief "Stop not reaching <device>": a declared `expect` contradicted while in force (#484). */
struct StopChainMismatch {
    std::string device;  // provider/device
    runtime::StopChainExpect expect;
    std::string reason;      // what contradicted it
    int64_t since_epoch_ms;  // when it was raised
};

/** @brief A mismatch raised, or resolved. */
struct StopChainMismatchChange {
    StopChainMismatch mismatch;
    bool raised;
};

struct StopChainSnapshot {
    StopChainState state = StopChainState::NotConfigured;
    std::string reason;                     // what the reading was, for status and the log
    std::optional<int64_t> since_epoch_ms;  // when the current state began
};

class StopChainMonitor {
public:
    StopChainMonitor(const runtime::StopChainConfig &config, const state::StateCache &cache,
                     std::chrono::milliseconds stale_after);

    bool configured() const { return config_.status.has_value(); }

    /** @brief Re-read the status. Returns {before, after} when the state changed. */
    std::optional<std::pair<StopChainSnapshot, StopChainSnapshot>> update();

    StopChainSnapshot snapshot() const;

    /**
     * @brief Re-judge the devices with a declared `expect` (#484).
     *
     * Only while the stop has been in force for the stale bound: before that, a
     * device that lost power may still have a fresh reading, and `estop` may lag
     * a poll. Returns the mismatches raised and resolved since the last call.
     */
    std::vector<StopChainMismatchChange> update_mismatches();

    /** @brief The mismatches standing now. */
    std::vector<StopChainMismatch> mismatches() const;

    /** @brief True while the stop is in force or its status unknown: hold, refuse the clear. */
    bool holds() const;

    /** @brief Why a clear must be refused now, or nullopt. */
    std::optional<std::string> clear_refusal() const;

    /** @brief Whether `device_handle` is in `safety.stop_chain.devices`. */
    bool in_scope(const std::string &device_handle) const;

    const runtime::StopChainConfig &config() const { return config_; }

private:
    std::pair<StopChainState, std::string> classify() const;

    const runtime::StopChainConfig &config_;
    const state::StateCache &cache_;
    const std::chrono::milliseconds stale_after_;
    std::string device_handle_;  // provider/device of the status signal
    std::string signal_id_;

    mutable std::mutex mutex_;
    StopChainSnapshot current_;                            // guarded by mutex_
    std::map<std::string, StopChainMismatch> mismatches_;  // by device; guarded by mutex_
};

}  // namespace control
}  // namespace anolis
