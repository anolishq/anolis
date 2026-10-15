#include "parameter_manager.hpp"

#include <algorithm>
#include <iostream>

#include "logging/logger.hpp"

namespace anolis {
namespace automation {

bool ParameterDef::validate(const ParameterValue &new_value, std::string &error) const {
    // Type must match the declared parameter type exactly.
    if (!parameter_value_matches_type(type, new_value)) {
        error = "Type mismatch: expected " + std::string(parameter_type_to_string(type)) + ", got " +
                std::string(parameter_value_type_name(new_value));
        return false;
    }

    // Numeric range validation
    if (type == ParameterType::DOUBLE || type == ParameterType::INT64) {
        double numeric_value = 0.0;
        if (std::holds_alternative<double>(new_value)) {
            numeric_value = std::get<double>(new_value);
        } else if (std::holds_alternative<int64_t>(new_value)) {
            numeric_value = static_cast<double>(std::get<int64_t>(new_value));
        }

        if (min.has_value() && numeric_value < min.value()) {
            error = "Value " + std::to_string(numeric_value) + " is below minimum " + std::to_string(min.value());
            return false;
        }

        if (max.has_value() && numeric_value > max.value()) {
            error = "Value " + std::to_string(numeric_value) + " exceeds maximum " + std::to_string(max.value());
            return false;
        }
    }

    // String enum validation
    if (type == ParameterType::STRING && allowed_values.has_value()) {
        const auto &str_value = std::get<std::string>(new_value);
        const auto &allowed = allowed_values.value();

        if (std::find(allowed.begin(), allowed.end(), str_value) == allowed.end()) {
            error = "Value '" + str_value + "' not in allowed values: [";
            for (size_t i = 0; i < allowed.size(); ++i) {
                error += allowed[i];
                if (i < allowed.size() - 1) {
                    error += ", ";
                }
            }
            error += "]";
            return false;
        }
    }

    return true;
}

bool ParameterManager::define(const std::string &name, ParameterType type, const ParameterValue &default_value,
                              std::optional<double> min, std::optional<double> max,
                              std::optional<std::vector<std::string>> allowed_values) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (parameters_.find(name) != parameters_.end()) {
        LOG_WARN("[ParameterManager] Parameter '" << name << "' already defined, ignoring redefinition");
        return false;
    }

    // Validate default value against constraints
    ParameterDef def{name, type, default_value, min, max, std::move(allowed_values)};
    std::string error;
    if (!def.validate(default_value, error)) {
        LOG_ERROR("[ParameterManager] Parameter '" << name << "' default value invalid: " << error);
        return false;
    }

    parameters_[name] = def;
    // Debug logging omitted for brevity
    return true;
}

bool ParameterManager::set(const std::string &name, const ParameterValue &value, std::string &error) {
    ParameterValue old_value;
    std::vector<ParameterChangeCallback> callbacks_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = parameters_.find(name);
        if (it == parameters_.end()) {
            error = "Parameter '" + name + "' not found";
            return false;
        }

        // Validate new value
        if (!it->second.validate(value, error)) {
            return false;
        }

        // Check if value actually changed
        old_value = it->second.value;
        if (old_value.index() == value.index()) {
            bool changed = true;
            std::visit(
                [&](auto &&old_val) {
                    using T = std::decay_t<decltype(old_val)>;
                    const auto &new_val = std::get<T>(value);
                    if (old_val == new_val) {
                        changed = false;
                    }
                },
                old_value);

            if (!changed) {
                return true;  // No change, but not an error
            }
        }

        // Update value
        it->second.value = value;
        it->second.set_by_operator = true;
        callbacks_copy = callbacks_;

        LOG_INFO("[ParameterManager] Parameter '" << name << "' updated");
    }  // Release lock before callbacks

    // Notify callbacks without holding lock
    for (const auto &callback : callbacks_copy) {
        callback(name, old_value, value);
    }

    return true;
}

std::optional<ParameterValue> ParameterManager::get(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = parameters_.find(name);
    if (it == parameters_.end()) {
        return std::nullopt;
    }

    return it->second.value;
}

double ParameterManager::get_double(const std::string &name, double default_value) const {
    auto value_opt = get(name);
    if (!value_opt.has_value()) return default_value;

    if (std::holds_alternative<double>(value_opt.value())) {
        return std::get<double>(value_opt.value());
    }

    return default_value;
}

int64_t ParameterManager::get_int64(const std::string &name, int64_t default_value) const {
    auto value_opt = get(name);
    if (!value_opt.has_value()) return default_value;

    if (std::holds_alternative<int64_t>(value_opt.value())) {
        return std::get<int64_t>(value_opt.value());
    }

    return default_value;
}

bool ParameterManager::get_bool(const std::string &name, bool default_value) const {
    auto value_opt = get(name);
    if (!value_opt.has_value()) return default_value;

    if (std::holds_alternative<bool>(value_opt.value())) {
        return std::get<bool>(value_opt.value());
    }

    return default_value;
}

std::string ParameterManager::get_string(const std::string &name, const std::string &default_value) const {
    auto value_opt = get(name);
    if (!value_opt.has_value()) return default_value;

    if (std::holds_alternative<std::string>(value_opt.value())) {
        return std::get<std::string>(value_opt.value());
    }

    return default_value;
}

std::optional<ParameterDef> ParameterManager::get_definition(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = parameters_.find(name);
    if (it == parameters_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::unordered_map<std::string, ParameterDef> ParameterManager::get_all_definitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parameters_;
}

void ParameterManager::on_parameter_change(const ParameterChangeCallback &callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(callback);
}

bool ParameterManager::has_parameter(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parameters_.find(name) != parameters_.end();
}

size_t ParameterManager::parameter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parameters_.size();
}

}  // namespace automation
}  // namespace anolis
