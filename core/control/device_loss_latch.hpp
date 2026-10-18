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
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "control/i_device_loss_latch.hpp"
#include "protocol.pb.h"
#include "state/state_cache.hpp"

namespace anolis {
namespace control {

class DeviceLossLatch : public IDeviceLossLatch {
public:
    /**
     * @brief Apply one reachability edge from StateCache's sink (#285, #403).
     *
     * The sink runs outside StateCache's lock and on more than one thread, so
     * two edges for a device can arrive here in either order. `edge_seq` is the
     * number StateCache stamped on the edge under its lock. An edge whose number
     * is not newer than the last one applied for this device is stale. The
     * check and the latch change happen together under `mutex_`, so no other
     * edge can land between them. Every arrival order ends where the in-order
     * one does:
     *
     * - A loss engages the latch only on a transport-level failure
     *   (`last_status` UNAVAILABLE or DEADLINE_EXCEEDED), stale or not. A
     *   stale transport loss still latches, failing closed: it lost the race
     *   to a return that was applied while the device was unlatched, so no
     *   safe state was re-asserted then. If it newly engages the latch and the
     *   device's newest applied edge is a return, it asks for that re-issue.
     * - A stale return is dropped: the device has gone again since.
     * - A stale non-transport loss is dropped, as a current one would be.
     *
     * @return true when the caller should re-assert the device's safe state,
     *         outside this lock: a current return of a latched device, or a
     *         stale transport loss as above.
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
    struct AppliedEdge {
        std::uint64_t seq = 0;
        bool reachable = false;
    };

    mutable std::mutex mutex_;
    std::set<std::string> latched_;
    // Newest edge applied per device. Kept across release_all(): a re-arm does
    // not make an edge decided before it any newer.
    std::unordered_map<std::string, AppliedEdge> last_edge_;
};

/**
 * @brief The StateCache reachability sink that feeds `latch` (#285, #403).
 *
 * Applies each edge with its number and calls `reissue` with the device handle,
 * outside the latch's lock, when the latch asks for the device's safe state to
 * be re-asserted. `reissue` runs on the delivering thread, which may hold a
 * provider lock; Runtime installs the ReissueWorker overload
 * (control/reissue_worker.hpp), which posts it instead (#427).
 */
state::StateCache::DeviceReachabilitySink make_reachability_sink(DeviceLossLatch &latch,
                                                                 std::function<void(const std::string &)> reissue);

}  // namespace control
}  // namespace anolis
