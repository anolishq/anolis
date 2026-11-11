#pragma once

#include <cstdint>
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
        std::function<bool(RuntimeMode previous_mode, RuntimeMode new_mode, std::string& error)>;

    /**
     * Mode change callback signature.
     * Called after successful mode transition.
     *
     * @param previous_mode Mode before transition
     * @param new_mode Mode after transition
     */
    using ModeChangeCallback = std::function<void(RuntimeMode previous_mode, RuntimeMode new_mode)>;

    /**
     * Commit guard signature (#239).
     *
     * Same contract as BeforeModeChangeCallback, but evaluated UNDER the mode
     * lock, immediately before the new mode is written, so its verdict is about
     * the state the transition actually commits into. A before-callback runs
     * with no lock held and can be outlived by a concurrent change; a guard
     * cannot. Never consulted for FAULT, which cannot be vetoed.
     *
     * A guard must be fast and must not call back into this ModeManager: the
     * mode mutex is held and is not recursive.
     */
    using CommitGuard = std::function<bool(RuntimeMode previous_mode, RuntimeMode new_mode, std::string& error)>;

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
     * Whether a transition out of AUTO has started and not yet committed or
     * been abandoned (#762). The mode still reads AUTO during this window.
     */
    bool is_leaving_auto() const;

    /**
     * Whether automation may send a command now: the mode is AUTO and no
     * transition out of it has started (#762). Thread-safe, and judged as one
     * reading of both.
     */
    bool automation_may_dispatch() const;

    /**
     * Whether a transition moves toward actuation: into AUTO, FAULT -> MANUAL
     * or IDLE -> MANUAL. Such a transition is refused at its commit, and its
     * hook calls are refused, once a FAULT has been requested since it was
     * requested (#774, #775). A transition that leaves AUTO or enters IDLE
     * does not.
     */
    static bool moves_toward_actuation(RuntimeMode from, RuntimeMode to);

    /**
     * The count of FAULT requests noted when the ordinary transition running
     * on this thread was requested, while its callbacks run; empty outside one,
     * and for FAULT (#775). Held per thread, not per ModeManager.
     */
    std::optional<std::uint64_t> fault_requests_at_current_transition() const;

    /**
     * Whether a FAULT has been requested since `fault_requests` was noted, in
     * any mode, the no-op one in FAULT included (#775).
     */
    bool fault_requested_since(std::uint64_t fault_requests) const;

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
     * A request for FAULT always ends in FAULT. While its before-callbacks run,
     * an ordinary transition cannot commit, so FAULT commits from the mode its
     * before-callbacks were chosen for (#763). In FAULT a request for FAULT is a
     * no-op. Every request for FAULT, in any mode, is counted, and a recovery
     * from FAULT is refused at its commit if one was made since its own
     * request, so the runtime stays in FAULT (#774). That covers a FAULT
     * requested in FAULT, and a FAULT that commits from MANUAL after another
     * recovery, while the mode reads FAULT again at this one's commit. An ordinary request is refused
     * if a FAULT is in progress, or if the mode changed while its own
     * before-callbacks ran.
     *
     * A transition out of AUTO, to MANUAL or FAULT, stays in progress until
     * its after-callbacks return. Until then a valid MANUAL -> IDLE is
     * refused, so the IDLE gate cannot refuse an after-hook leaving AUTO
     * (#776). Other requests are not refused for this, so a recovery from a
     * FAULT after-callback still commits.
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
     * Register a guard evaluated under the mode lock just before a non-FAULT
     * transition commits. Returning false (or throwing) rejects the transition.
     */
    void add_commit_guard(const CommitGuard& guard);

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
    // FAULT requests between validation and their commit (or return).
    int fault_requests_pending_ = 0;
    // Transitions out of AUTO between validation and their commit (or return).
    int leaving_auto_pending_ = 0;
    // Transitions out of AUTO that have committed and are running their
    // after-callbacks (#776).
    int leave_auto_after_callbacks_running_ = 0;
    // FAULT requests made so far, a no-op one in FAULT included (#774).
    std::uint64_t fault_requests_ = 0;
    std::vector<BeforeModeChangeCallback> before_callbacks_;
    std::vector<CommitGuard> commit_guards_;
    std::vector<ModeChangeCallback> callbacks_;
};

}  // namespace automation
}  // namespace anolis
