#pragma once

/**
 * @file derived_config.hpp
 * @brief Configuration of the derived-signals spike (docs/rfcs/derived-signals.md, O10).
 *
 * Throwaway: this answers what hosting the engine in the runtime costs, and is not a design.
 */

#include <optional>
#include <string>
#include <vector>

namespace anolis {
namespace derived {

/// One input of the graph: a signal of the state cache, or a constant.
struct DerivedInputConfig {
    std::string name;                // The graph's name for it
    std::string device_handle;       // "provider/device"; empty for a constant
    std::string signal_id;           //
    std::optional<double> constant;  // Written at every call, stamped with the call's time
    std::string not_acting_on;       // "provider/device": true while the runtime's automation is not acting on it
};

struct DerivedConfig {
    bool enabled = false;
    std::string id = "derived0";         // The provider id the outputs appear under
    std::string graph_path;              // A graph file in the shape of section 9 of FluxGraph's version 2 proposal
    int call_interval_ms = 0;            // How often the engine is called; 0 is the graph's period
    bool sample_time_on_arrival = true;  // O7: a sample's time is when its read returned, on the monotonic clock
    int sample_skew_ms = 0;  // Added to every sample time: a provider whose clock is that far from the runtime's
    // A clock that steps, which no run can make of a real one: from step_after_s after the start, clock_step_ms
    // is added to the wall clock the calls and the cache's sample times are on (the runtime's and a provider's
    // on one machine), and sample_step_ms to the sample times alone (a provider's clock by itself). Neither
    // touches samples timed on arrival.
    // An input given by not_acting_on is true as well while that device's provider is down, and from then until
    // a read of the device has returned: a restart the runtime stays in AUTO through leaves the plant new and
    // the graph as it was.
    bool idle_on_provider_loss = true;
    int clock_step_ms = 0;
    int sample_step_ms = 0;
    double step_after_s = 0.0;
    std::vector<DerivedInputConfig> inputs;
};

}  // namespace derived
}  // namespace anolis
