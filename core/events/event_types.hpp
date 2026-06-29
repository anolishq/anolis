#pragma once

/**
 * @file event_types.hpp
 * @brief Event types for the Anolis observability layer
 *
 * This file defines the core event structures emitted by StateCache
 * when device state changes. Events are consumed by:
 * - SSE endpoint (real-time streaming to dashboards)
 * - Telemetry sink (historical storage in InfluxDB)
 *
 * Design principles:
 * - Events are immutable value types (cheap to copy)
 * - All fields use established conventions (typed values, quality enums)
 * - Timestamps are epoch milliseconds (consistent with HTTP API)
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>

namespace anolis {
namespace events {

/**
 * @brief Quality levels for signal values
 *
 * Maps directly to ADPP quality values but uses simple enum
 * for cleaner event handling without protobuf dependency.
 */
enum class Quality {
    OK,           // Value is current and valid
    STALE,        // Value is outdated (poll timeout or provider slow)
    UNAVAILABLE,  // Cannot read value (provider disconnected)
    FAULT         // Hardware fault or invalid reading
};

/**
 * @brief Convert Quality enum to string
 */
inline const char *quality_to_string(Quality q) {
    switch (q) {
        case Quality::OK:
            return "OK";
        case Quality::STALE:
            return "STALE";
        case Quality::UNAVAILABLE:
            return "UNAVAILABLE";
        case Quality::FAULT:
            return "FAULT";
        default:
            return "UNAVAILABLE";
    }
}

/**
 * @brief Typed value union
 *
 * Uses std::variant for type-safe value storage.
 * Matches the JSON format: {"type": "double", "double": 23.5}
 */
using TypedValue = std::variant<double,      // "double"
                                int64_t,     // "int64"
                                uint64_t,    // "uint64"
                                bool,        // "bool"
                                std::string  // "string" or "bytes" (base64 encoded)
                                >;

/**
 * @brief Get the type name for a TypedValue
 */
inline const char *value_type_name(const TypedValue &v) {
    return std::visit(
        [](auto &&arg) -> const char * {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, double>)
                return "double";
            else if constexpr (std::is_same_v<T, int64_t>)
                return "int64";
            else if constexpr (std::is_same_v<T, uint64_t>)
                return "uint64";
            else if constexpr (std::is_same_v<T, bool>)
                return "bool";
            else if constexpr (std::is_same_v<T, std::string>)
                return "string";
            else
                return "unknown";
        },
        v);
}

/**
 * @brief Compare two TypedValues for equality
 *
 * Uses bitwise comparison for doubles to handle NaN and +/-0 deterministically:
 * - NaN == NaN -> true (bitwise same)
 * - +0 == -0 -> false (bitwise different)
 *
 * This prevents spurious change events from floating-point edge cases.
 */
inline bool values_equal(const TypedValue &a, const TypedValue &b) {
    if (a.index() != b.index()) {
        return false;
    }

    return std::visit(
        [&b](auto &&arg_a) -> bool {
            using T = std::decay_t<decltype(arg_a)>;
            const auto &arg_b = std::get<T>(b);

            if constexpr (std::is_same_v<T, double>) {
                // Bitwise comparison for deterministic NaN/+/-0 handling
                uint64_t bits_a, bits_b;
                std::memcpy(&bits_a, &arg_a, sizeof(double));
                std::memcpy(&bits_b, &arg_b, sizeof(double));
                return bits_a == bits_b;
            } else {
                return arg_a == arg_b;
            }
        },
        a);
}

/**
 * @brief State update event emitted when a signal value or quality changes
 *
 * This is the primary event type. Events are emitted on change only:
 * - Value changes (using bitwise comparison for doubles)
 * - Quality changes (any transition)
 *
 * Events are NOT emitted on every poll (prevents flooding).
 */
struct StateUpdateEvent {
    uint64_t event_id;        // Monotonic event ID for gap detection
    std::string provider_id;  // e.g., "sim0"
    std::string device_id;    // e.g., "tempctl-0"
    std::string signal_id;    // e.g., "temperature"
    TypedValue value;         // The signal value
    Quality quality;          // Signal quality
    int64_t timestamp_ms;     // Epoch milliseconds

    /**
     * @brief Create event with current timestamp
     */
    static StateUpdateEvent create(uint64_t id, const std::string &provider_id, const std::string &device_id,
                                   const std::string &signal_id, const TypedValue &value, Quality quality) {
        auto now = std::chrono::system_clock::now();
        auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        return StateUpdateEvent{id, provider_id, device_id, signal_id, value, quality, epoch_ms};
    }
};

/**
 * @brief Quality-only change event (no value change)
 *
 * Emitted when quality changes but value hasn't (e.g., OK -> STALE).
 * Allows clients to update quality indicators without full value payload.
 */
struct QualityChangeEvent {
    uint64_t event_id;
    std::string provider_id;
    std::string device_id;
    std::string signal_id;
    Quality old_quality;
    Quality new_quality;
    int64_t timestamp_ms;
};

/**
 * @brief Device availability event
 *
 * Emitted when a device's provider becomes available or unavailable.
 */
struct DeviceAvailabilityEvent {
    uint64_t event_id;
    std::string provider_id;
    std::string device_id;
    bool available;
    int64_t timestamp_ms;
};

/**
 * @brief Mode change event
 *
 * Emitted when runtime mode transitions (MANUAL/AUTO/IDLE/FAULT).
 */
struct ModeChangeEvent {
    uint64_t event_id;
    std::string previous_mode;
    std::string new_mode;
    int64_t timestamp_ms;
};

/**
 * @brief Parameter change event
 *
 * Emitted when a runtime parameter value is updated.
 */
struct ParameterChangeEvent {
    uint64_t event_id;
    std::string parameter_name;
    std::string old_value_str;  // String representation for telemetry
    std::string new_value_str;  // String representation for telemetry
    int64_t timestamp_ms;
};

/**
 * @brief Automation fault event (engine-abnormal only)
 *
 * Emitted when the automation engine itself hits an abnormal condition — a tick
 * exception, a corrupt/unparseable definition, or an invariant violation. This
 * is NOT an ordinary unsuccessful execution: a behaviour tree returning FAILURE
 * surfaces as `execution_status=failed`, not a fault.
 *
 * Engine-neutral by design: `locus` is a generic location hint (e.g. "tick"),
 * never a BT-specific node path. The SSE serializer renders this as a neutral
 * `automation_fault` frame (the legacy `bt_error` alias was removed in v0.1.26).
 */
struct AutomationFaultEvent {
    uint64_t event_id;
    std::string locus;  // Generic location hint (e.g. "tick"); empty if unknown
    std::string error;  // Error message
    int64_t timestamp_ms;
};

/**
 * @brief Provider health change event
 *
 * Emitted when a provider's availability state changes.
 */
struct ProviderHealthChangeEvent {
    uint64_t event_id;
    std::string provider_id;
    std::string state;  // "AVAILABLE" or "UNAVAILABLE"
    int64_t timestamp_ms;
};

/**
 * @brief Union of all event types
 *
 * Subscribers receive Event variants and can dispatch on type.
 */
using Event = std::variant<StateUpdateEvent, QualityChangeEvent, DeviceAvailabilityEvent, ModeChangeEvent,
                           ParameterChangeEvent, AutomationFaultEvent, ProviderHealthChangeEvent>;

/**
 * @brief Get event ID from any event type
 */
inline uint64_t get_event_id(const Event &event) {
    return std::visit([](auto &&e) { return e.event_id; }, event);
}

/**
 * @brief Get timestamp from any event type
 */
inline int64_t get_timestamp_ms(const Event &event) {
    return std::visit([](auto &&e) { return e.timestamp_ms; }, event);
}

}  // namespace events
}  // namespace anolis
