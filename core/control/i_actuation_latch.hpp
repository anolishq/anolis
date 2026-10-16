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

    /**
     * @brief True while the e-stop's own FAULT entry may run `-> FAULT` hooks.
     *
     * CallRouter lets a request marked `fault_entry_hook` through the engaged
     * latch only while this holds. It holds only on the thread running the
     * e-stop's FAULT entry, and only when the ladder planned no rung (#251).
     */
    virtual bool admits_fault_entry_hooks() const { return false; }
};

}  // namespace control
}  // namespace anolis
