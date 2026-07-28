#pragma once

/**
 * @file i_actuation_latch.hpp
 * @brief Minimal read-only view of the e-stop actuation latch.
 *
 * CallRouter consults this to refuse actuating calls while the latch is
 * engaged. Kept as a narrow interface so `anolis_control` does not depend on
 * the full SafeStateController (which itself depends on CallRouter).
 */

namespace anolis {
namespace control {

class IActuationLatch {
public:
    virtual ~IActuationLatch() = default;

    /** @brief True while a software e-stop is latched (actuation blocked). */
    virtual bool is_engaged() const = 0;
};

}  // namespace control
}  // namespace anolis
