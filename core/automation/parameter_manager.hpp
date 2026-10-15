#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "parameter_types.hpp"

namespace anolis {
namespace automation {

/**
 * @brief Parameter definition with validation constraints
 */
struct ParameterDef {
    std::string name;
    ParameterType type;
    ParameterValue value;

    // Validation constraints
    std::optional<double> min;                               // For numeric types
    std::optional<double> max;                               // For numeric types
    std::optional<std::vector<std::string>> allowed_values;  // For string enums

    // False until an operator set changes the value; served as `source`.
    bool set_by_operator = false;

    /**
     * @brief Validate a new value against constraints
     *
     * @param new_value Value to validate
     * @param error Output error message if validation fails
     * @return true if valid, false otherwise
     */
    bool validate(const ParameterValue &new_value, std::string &error) const;
};

/**
 * @brief Parameter change callback
 *
 * Invoked when a parameter value changes.
 * Parameters: (name, old_value, new_value)
 */
using ParameterChangeCallback =
    std::function<void(const std::string &name, const ParameterValue &old_value, const ParameterValue &new_value)>;

/**
 * @brief ParameterManager
 *
 * Manages runtime-tunable parameters for behavior tree automation.
 *
 * Features:
 * - Type-safe parameter storage with validation
 * - Range/enum constraints loaded from YAML
 * - Thread-safe parameter access and updates
 * - Change notification callbacks for telemetry
 * - No persistence: every start begins at the config defaults, and an
 *   operator change lasts until the runtime restarts
 *
 * Architecture constraints:
 * - Parameters are READ-ONLY from BT perspective (accessed via blackboard)
 * - Updates only via ParameterManager API or HTTP endpoints
 * - Parameters do NOT bypass kernel validation (they're just config values)
 *
 * Usage from BT:
 * 1. ParameterManager populates blackboard before tick
 * 2. GetParameterNode reads from blackboard using input port `param`
 * 3. Parameter values used in CallDevice arguments or condition checks
 *
 * Example:
 * ```cpp
 * ParameterManager pm;
 * pm.define("temp_setpoint", ParameterType::DOUBLE, 25.0, 10.0, 50.0);
 * pm.set("temp_setpoint", 30.0, error);
 * double value = pm.get_double("temp_setpoint");
 * ```
 */
class ParameterManager {
public:
    ParameterManager() = default;

    // Non-copyable, non-movable (manages thread-safe state)
    ParameterManager(const ParameterManager &) = delete;
    ParameterManager &operator=(const ParameterManager &) = delete;
    ParameterManager(ParameterManager &&) = delete;
    ParameterManager &operator=(ParameterManager &&) = delete;

    /**
     * @brief Define a parameter with constraints
     *
     * @param name Parameter name (must be unique)
     * @param type Parameter type
     * @param default_value Initial value
     * @param min Minimum value (for numeric types, nullopt = no constraint)
     * @param max Maximum value (for numeric types, nullopt = no constraint)
     * @param allowed_values Enum values (for string types, nullopt = no constraint)
     * @return true if defined, false if name already exists
     */
    bool define(const std::string &name, ParameterType type, const ParameterValue &default_value,
                std::optional<double> min = std::nullopt, std::optional<double> max = std::nullopt,
                std::optional<std::vector<std::string>> allowed_values = std::nullopt);

    /**
     * @brief Set parameter value with validation
     *
     * @param name Parameter name
     * @param value New value
     * @param error Output error message if validation fails
     * @return true if set successfully, false if invalid or not found
     */
    bool set(const std::string &name, const ParameterValue &value, std::string &error);

    /**
     * @brief Get parameter value by name
     *
     * @param name Parameter name
     * @return Parameter value, nullopt if not found
     */
    std::optional<ParameterValue> get(const std::string &name) const;

    /**
     * @brief Get parameter value as double (convenience)
     *
     * @param name Parameter name
     * @param default_value Value to return if not found or wrong type
     * @return Parameter value or default_value
     */
    double get_double(const std::string &name, double default_value = 0.0) const;

    /**
     * @brief Get parameter value as int64 (convenience)
     */
    int64_t get_int64(const std::string &name, int64_t default_value = 0) const;

    /**
     * @brief Get parameter value as bool (convenience)
     */
    bool get_bool(const std::string &name, bool default_value = false) const;

    /**
     * @brief Get parameter value as string (convenience)
     */
    std::string get_string(const std::string &name, const std::string &default_value = "") const;

    /**
     * @brief Get parameter definition (for validation/introspection)
     *
     * @param name Parameter name
     * @return Parameter definition, nullopt if not found
     */
    std::optional<ParameterDef> get_definition(const std::string &name) const;

    /**
     * @brief Get all parameter definitions
     *
     * @return Map of name -> ParameterDef
     */
    std::unordered_map<std::string, ParameterDef> get_all_definitions() const;

    /**
     * @brief Register callback for parameter changes
     *
     * @param callback Function to call when parameters change
     */
    void on_parameter_change(const ParameterChangeCallback &callback);

    /**
     * @brief Check if parameter exists
     */
    bool has_parameter(const std::string &name) const;

    /**
     * @brief Get count of defined parameters
     */
    size_t parameter_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ParameterDef> parameters_;
    std::vector<ParameterChangeCallback> callbacks_;
};

}  // namespace automation
}  // namespace anolis
