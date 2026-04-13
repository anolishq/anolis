#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace anolis {
namespace automation {

/**
 * Runtime operating modes for automation layer.
 *
 * State transitions:
 * - MANUAL <-> AUTO (normal operation)
 * - MANUAL <-> IDLE (operator signaling standby)
 * - Any -> FAULT (error condition)
 * - FAULT -> MANUAL (manual recovery)
 * - FAULT -X-> AUTO (must recover through MANUAL first)
 */
enum class RuntimeMode {
    MANUAL,  // BT stopped, manual calls allowed
    AUTO,    // BT running, manual calls gated by policy
    IDLE,    // BT stopped, control calls blocked (default, safe startup)
    FAULT    // BT stopped due to error, manual calls allowed for recovery
};

/**
 * Convert RuntimeMode enum to string.
 */
const char* mode_to_string(RuntimeMode mode);

/**
 * Parse string to RuntimeMode enum.
 * Returns std::nullopt if string is invalid.
 * Valid values: "MANUAL", "AUTO", "IDLE", "FAULT"
 */
std::optional<RuntimeMode> string_to_mode(const std::string& str);

/**
 * ModeManager - Thread-safe runtime mode state machine.
 *
 * Enforces valid state transitions and notifies listeners on mode changes.
 * Integrates with BTRuntime for lifecycle control and CallRouter for gating.
 */
class ModeManager {
public:
    /**
     * Before mode change callback signature.
     *
     * Returning false vetoes the transition and sets an error message,
     * except when transitioning to FAULT.
     *
     * FAULT is treated as fail-safe: before-callback rejection/exception is
     * logged but does not block entry into FAULT mode.
     *
     * @param previous_mode Mode before transition
     * @param new_mode Requested mode
     * @param error Output error when returning false
     * @return true to allow transition, false to reject
     */
    using BeforeModeChangeCallback =
        std::function<bool(RuntimeMode previous_mode, RuntimeMode new_mode, std::string &error)>;

    /**
     * Mode change callback signature.
     * Called after successful mode transition.
     *
     * @param previous_mode Mode before transition
     * @param new_mode Mode after transition
     */
    using ModeChangeCallback = std::function<void(RuntimeMode previous_mode, RuntimeMode new_mode)>;

    /**
     * Construct mode manager with initial mode.
     *
     * @param initial_mode Starting mode (default: IDLE)
     */
    explicit ModeManager(RuntimeMode initial_mode = RuntimeMode::IDLE);

    /**
     * Get current runtime mode (thread-safe).
     */
    RuntimeMode current_mode() const;

    /**
     * Check if currently in IDLE mode (thread-safe).
     *
     * @return true if current mode is IDLE
     */
    bool is_idle() const;

    /**
     * Request mode transition.
     *
     * Validates transition rules:
     * - MANUAL <-> AUTO: allowed
     * - MANUAL <-> IDLE: allowed
     * - Any -> FAULT: allowed (error condition)
     * - FAULT -> MANUAL: allowed (recovery)
     * - FAULT -> AUTO: blocked (must go thru MANUAL)
     * - FAULT -> IDLE: blocked (must go thru MANUAL)
     *
     * @param new_mode Requested mode
     * @param error Output error message if transition fails
     * @return true if transition succeeded, false if blocked
     */
    bool set_mode(RuntimeMode new_mode, std::string& error);

    /**
     * Register callback invoked before mode transition is committed.
     *
     * Callbacks run with no internal lock held. Returning false rejects
     * the transition, except when transitioning to FAULT.
     */
    void on_before_mode_change(const BeforeModeChangeCallback& callback);

    /**
     * Register callback for mode change notifications.
     *
     * Callback is invoked AFTER mode transition with lock released.
     * Multiple callbacks can be registered.
     *
     * @param callback Function to call on mode change
     */
    void on_mode_change(const ModeChangeCallback& callback);

private:
    /**
     * Validate if transition from current_mode to new_mode is allowed.
     *
     * @param from Current mode
     * @param to Requested mode
     * @return true if transition is valid
     */
    bool is_valid_transition(RuntimeMode from, RuntimeMode to) const;

    mutable std::mutex mutex_;
    RuntimeMode current_mode_;
    std::vector<BeforeModeChangeCallback> before_callbacks_;
    std::vector<ModeChangeCallback> callbacks_;
};

}  // namespace automation
}  // namespace anolis
