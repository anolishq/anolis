#pragma once

/**
 * @file device_loss_latch.hpp
 * @brief Per-device latch blocking autonomous actuation after a device is lost
 *        (anolishq/anolis#285).
 *
 * ISO 13850 §4.1.4: releasing an emergency stop must not restart the machine;
 * it may only permit restarting. On a machine whose stop removes device power,
 * the runtime process survives the cut, so the behaviour tree keeps ticking
 * with live parameters and re-issues its last command on its keepalive when the
 * devices come back. Measured on the reference rig 2026-08-20: the impeller
 * restarted by itself ~1.4 s after power returned.
 *
 * This blocks the command rather than stopping the machine. A latched device
 * cannot be commanded by automation until an operator deliberately re-enters
 * AUTO, which is the "permit restarting" the standard asks for. Manual calls,
 * mode-transition hooks and safe-state calls are all unaffected -- see
 * CallRouter for why the hook exemption is load-bearing rather than a
 * convenience.
 *
 * Threading: written from the polling thread, read from the automation thread.
 * All access is under `mutex_`.
 */

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "control/i_device_loss_latch.hpp"

namespace anolis {
namespace control {

class DeviceLossLatch : public IDeviceLossLatch {
public:
    /** @brief Latch a device. Idempotent: re-latching an engaged device is a no-op. */
    void engage(const std::string &device_handle);

    bool is_latched(const std::string &device_handle) const override;

    /**
     * @brief Release every latch and return what was released.
     *
     * Called on the deliberate MANUAL -> AUTO transition, post-commit. The
     * returned handles are logged: an operator re-arming a machine should see
     * which devices it covered.
     */
    std::vector<std::string> release_all();

    std::vector<std::string> latched() const override;

private:
    mutable std::mutex mutex_;
    std::set<std::string> latched_;
};

}  // namespace control
}  // namespace anolis
