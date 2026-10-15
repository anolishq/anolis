#include "runtime/poll_hint_check.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

anolis::registry::RegisteredDevice device_with_hints(const std::string &device_id,
                                                     const std::vector<std::pair<std::string, double>> &hints) {
    anolis::registry::RegisteredDevice device;
    device.provider_id = "sim0";
    device.device_id = device_id;
    for (const auto &[signal_id, hint] : hints) {
        anolis::registry::SignalSpec spec{};
        spec.signal_id = signal_id;
        spec.poll_hint_hz = hint;
        device.capabilities.signals_by_id[signal_id] = spec;
    }
    return device;
}

}  // namespace

// #269: polling.interval_ms 500 is 2 Hz. A hint more than 2x away in either
// direction is reported once per device; one within 2x, exactly 2x, or unset
// is not.
TEST(PollHintCheckTest, ReportsOnlyHintsMoreThanTwiceAway) {
    const std::vector<anolis::registry::RegisteredDevice> devices = {
        device_with_hints("fast", {{"temp", 10.0}}),             // 5x faster
        device_with_hints("near", {{"temp", 3.0}}),              // 1.5x
        device_with_hints("slow", {{"ph", 0.5}, {"do", 0.25}}),  // 4x and 8x slower
        device_with_hints("edge", {{"temp", 1.0}}),              // exactly 2x
        device_with_hints("unset", {{"temp", 0.0}}),
    };

    const auto lines = anolis::runtime::poll_hint_mismatches(devices, 500);

    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines[0].find("sim0/fast"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("10 Hz (temp)"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("polling.interval_ms 500"), std::string::npos) << lines[0];

    // One line for the device, naming both mismatched signals.
    EXPECT_NE(lines[1].find("sim0/slow"), std::string::npos) << lines[1];
    EXPECT_NE(lines[1].find("0.25 Hz (do)"), std::string::npos) << lines[1];
    EXPECT_NE(lines[1].find("0.5 Hz (ph)"), std::string::npos) << lines[1];
}
