#include "automation/mode_manager.hpp"

#include <iostream>

#include "logging/logger.hpp"

namespace anolis {
namespace automation {

const char *mode_to_string(RuntimeMode mode) {
    switch (mode) {
        case RuntimeMode::MANUAL:
            return "MANUAL";
        case RuntimeMode::AUTO:
            return "AUTO";
        case RuntimeMode::IDLE:
            return "IDLE";
        case RuntimeMode::FAULT:
            return "FAULT";
        default:
            return "UNKNOWN";
    }
}

std::optional<RuntimeMode> string_to_mode(const std::string &str) {
    if (str == "MANUAL") {
        return RuntimeMode::MANUAL;
    }
    if (str == "AUTO") {
        return RuntimeMode::AUTO;
    }
    if (str == "IDLE") {
        return RuntimeMode::IDLE;
    }
    if (str == "FAULT") {
        return RuntimeMode::FAULT;
    }

    return std::nullopt;  // Strict validation - no silent defaults
}

ModeManager::ModeManager(RuntimeMode initial_mode) : current_mode_(initial_mode) {
    LOG_INFO("[ModeManager] Initialized in " << mode_to_string(initial_mode) << " mode");
}

RuntimeMode ModeManager::current_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_mode_;
}

bool ModeManager::is_idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_mode_ == RuntimeMode::IDLE;
}

bool ModeManager::set_mode(RuntimeMode new_mode, std::string &error) {
    RuntimeMode previous_mode;
    std::vector<BeforeModeChangeCallback> before_callbacks_copy;
    std::vector<ModeChangeCallback> callbacks_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // No-op if already in requested mode
        if (current_mode_ == new_mode) {
            return true;
        }

        // Validate transition
        if (!is_valid_transition(current_mode_, new_mode)) {
            error = std::string("Invalid mode transition: ") + mode_to_string(current_mode_) + " -> " +
                    mode_to_string(new_mode);
            LOG_ERROR("[ModeManager] " << error);
            return false;
        }

        previous_mode = current_mode_;

        // Copy callbacks while holding lock
        before_callbacks_copy = before_callbacks_;
        callbacks_copy = callbacks_;
    }  // Lock released here

    // Execute before-transition callbacks (may veto transition)
    // NOTE: FAULT transitions are fail-safe and cannot be vetoed by callbacks.
    for (const auto &callback : before_callbacks_copy) {
        try {
            std::string callback_error;
            if (!callback(previous_mode, new_mode, callback_error)) {
                if (new_mode == RuntimeMode::FAULT) {
                    const std::string warn = callback_error.empty() ? "Before-mode callback rejected FAULT transition"
                                                                    : callback_error;
                    LOG_WARN("[ModeManager] Ignoring before-mode rejection during FAULT transition: " << warn);
                    continue;
                }
                error = callback_error.empty() ? "Mode transition rejected by before-mode callback" : callback_error;
                LOG_ERROR("[ModeManager] " << error);
                return false;
            }
        } catch (const std::exception &e) {
            if (new_mode == RuntimeMode::FAULT) {
                LOG_WARN("[ModeManager] Ignoring before-mode callback exception during FAULT transition: " << e.what());
                continue;
            }
            error = std::string("Before mode change callback error: ") + e.what();
            LOG_ERROR("[ModeManager] " << error);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Transition may have changed while callbacks were running.
        if (current_mode_ != previous_mode) {
            error = "Concurrent mode change detected while processing transition callbacks";
            LOG_ERROR("[ModeManager] " << error);
            return false;
        }

        if (!is_valid_transition(current_mode_, new_mode)) {
            error = std::string("Invalid mode transition: ") + mode_to_string(current_mode_) + " -> " +
                    mode_to_string(new_mode);
            LOG_ERROR("[ModeManager] " << error);
            return false;
        }

        current_mode_ = new_mode;
    }

    LOG_INFO("[ModeManager] Mode changed: " << mode_to_string(previous_mode) << " -> " << mode_to_string(new_mode));

    // Notify callbacks without holding lock (prevents deadlocks)
    for (const auto &callback : callbacks_copy) {
        try {
            callback(previous_mode, new_mode);
        } catch (const std::exception &e) {
            LOG_ERROR("[ModeManager] Error in mode change callback: " << e.what());
        }
    }

    return true;
}

void ModeManager::on_before_mode_change(const BeforeModeChangeCallback &callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_callbacks_.push_back(callback);
}

void ModeManager::on_mode_change(const ModeChangeCallback &callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(callback);
}

bool ModeManager::is_valid_transition(RuntimeMode from, RuntimeMode to) const {
    // Same mode is always valid (no-op)
    if (from == to) {
        return true;
    }

    // Any mode can transition to FAULT (error condition)
    if (to == RuntimeMode::FAULT) {
        return true;
    }

    // FAULT can only transition to MANUAL (recovery path)
    if (from == RuntimeMode::FAULT) {
        return to == RuntimeMode::MANUAL;
    }

    // Normal operations: MANUAL <-> AUTO, MANUAL <-> IDLE
    if (from == RuntimeMode::MANUAL) {
        return to == RuntimeMode::AUTO || to == RuntimeMode::IDLE;
    }

    if (from == RuntimeMode::AUTO || from == RuntimeMode::IDLE) {
        return to == RuntimeMode::MANUAL;
    }

    // All other transitions blocked
    return false;
}

}  // namespace automation
}  // namespace anolis
