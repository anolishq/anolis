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

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "control/i_device_loss_latch.hpp"
#include "protocol.pb.h"

namespace anolis {
namespace control {

class DeviceLossLatch : public IDeviceLossLatch {
public:
    /** @brief Latch a device. Idempotent: re-latching an engaged device is a no-op. */
    void engage(const std::string &device_handle);

    /**
     * @brief Apply one reachability edge from StateCache's sink (#285, #403).
     *
     * The sink runs outside StateCache's lock and on more than one thread, so
     * two edges for a device can arrive here in either order. `edge_seq` is the
     * number StateCache stamped on the edge under its lock. An edge whose number
     * is not newer than the last one applied for this device is stale and is
     * dropped: a late loss must not latch a device that has since answered,
     * and a late return must not re-assert safe state for a device that has
     * since gone again. The check and the latch change happen together under
     * `mutex_`, so no other edge can land between them.
     *
     * A loss engages the latch only on a transport-level failure
     * (`last_status` UNAVAILABLE or DEADLINE_EXCEEDED).
     *
     * @return true for a current return edge of a latched device. The caller
     *         re-asserts its safe state, outside this lock.
     */
    bool apply_reachability_edge(const std::string &device_handle, bool reachable,
                                 anolis::deviceprovider::v1::Status_Code last_status, std::uint64_t edge_seq);

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
    // Newest edge number applied per device. Kept across release_all(): a
    // re-arm does not make an edge decided before it any newer.
    std::unordered_map<std::string, std::uint64_t> last_edge_seq_;
};

}  // namespace control
}  // namespace anolis
