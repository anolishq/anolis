#include "runtime/poll_hint_check.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace anolis {
namespace runtime {

std::vector<std::string> poll_hint_mismatches(const std::vector<registry::RegisteredDevice> &devices,
                                              int polling_interval_ms) {
    std::vector<std::string> lines;
    if (polling_interval_ms <= 0) {
        return lines;
    }
    const double polling_hz = 1000.0 / polling_interval_ms;

    for (const auto &device : devices) {
        std::vector<std::pair<std::string, double>> mismatched;
        for (const auto &[signal_id, spec] : device.capabilities.signals_by_id) {
            const double hint = spec.poll_hint_hz;
            if (hint > 0.0 && (hint > 2.0 * polling_hz || hint * 2.0 < polling_hz)) {
                mismatched.emplace_back(signal_id, hint);
            }
        }
        if (mismatched.empty()) {
            continue;
        }
        std::sort(mismatched.begin(), mismatched.end());

        std::string hints;
        for (const auto &[signal_id, hint] : mismatched) {
            hints += std::format("{}{} Hz ({})", hints.empty() ? "" : ", ", hint, signal_id);
        }
        lines.push_back(
            std::format("{}: poll_hint_hz more than 2x away from polling.interval_ms {} ({} Hz): {}. The hint is "
                        "advisory; this device is polled at polling.interval_ms.",
                        device.get_handle(), polling_interval_ms, polling_hz, hints));
    }
    return lines;
}

}  // namespace runtime
}  // namespace anolis
