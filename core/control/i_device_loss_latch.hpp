#pragma once

/**
 * @file i_device_loss_latch.hpp
 * @brief Read-only view of the per-device loss latch.
 *
 * CallRouter consults this to refuse behaviour-tree calls to a device that went
 * unreachable and has not been deliberately re-armed. Kept narrow so
 * `anolis_control`'s router does not depend on the latch's mutation surface.
 */

#include <string>
#include <vector>

namespace anolis {
namespace control {

class IDeviceLossLatch {
public:
    virtual ~IDeviceLossLatch() = default;

    /** @brief True while this device is latched against autonomous actuation. */
    virtual bool is_latched(const std::string &device_handle) const = 0;

    /**
     * @brief Handles currently latched, for the status surface.
     *
     * A latch means a real actuator is refusing automation. Log lines are not a
     * surface an operator watches, so it is reported on GET /v0/runtime/status.
     */
    virtual std::vector<std::string> latched() const = 0;
};

}  // namespace control
}  // namespace anolis
