#pragma once

/**
 * @file poll_hint_check.hpp
 * @brief Startup report of providers' poll-rate hints that the runtime ignores.
 *
 * ADPP defines `poll_hint_hz` as an advisory polling frequency. The runtime
 * polls every device at `polling.interval_ms` regardless (#269): honouring a
 * slow hint on a shared bus would starve the polls that keep devices' watchdogs
 * fed. A large mismatch is still worth saying once, so the operator knows the
 * hint is not what sets the rate.
 */

#include <string>
#include <vector>

#include "registry/device_registry.hpp"

namespace anolis {
namespace runtime {

/**
 * @brief One line per device whose signal poll hints differ from the polling
 * rate by more than 2x in either direction.
 *
 * Signals without a hint (`poll_hint_hz` <= 0) are ignored. Each line names the
 * device handle, the mismatched hints with their signals, and the interval.
 */
std::vector<std::string> poll_hint_mismatches(const std::vector<registry::RegisteredDevice> &devices,
                                              int polling_interval_ms);

}  // namespace runtime
}  // namespace anolis
