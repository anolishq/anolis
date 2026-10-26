#include "control/device_loss_latch.hpp"

#include <utility>

#include "logging/logger.hpp"

namespace anolis {
namespace control {

namespace {

void log_engaged(const std::string &device_handle) {
    LOG_WARN("[DeviceLossLatch] " << device_handle
                                  << " went unreachable; autonomous actuation to it is blocked until "
                                     "MANUAL -> AUTO re-arms it");
}

}  // namespace

bool DeviceLossLatch::apply_reachability_edge(const std::string &device_handle, bool reachable,
                                              anolis::deviceprovider::v1::Status_Code last_status,
                                              std::uint64_t edge_seq) {
    // Latch on TRANSPORT-level unreachability only. A decode or
    // frame-header mismatch reports INTERNAL and means the device
    // answered -- the reference rig produced three such reads in
    // three minutes while entirely healthy, and latching on those
    // would force a human re-arm several times a day. A powered-off
    // board cannot answer at all, so this loses no detection for the
    // case the latch exists for.
    const bool transport_failure = last_status == anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE ||
                                   last_status == anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
    bool engaged = false;
    bool reissue = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &last = last_edge_[device_handle];
        if (edge_seq <= last.seq) {
            if (reachable || !transport_failure) {
                LOG_DEBUG("[DeviceLossLatch] dropped a stale " << (reachable ? "return" : "loss") << " edge for "
                                                               << device_handle);
                return false;
            }
            // A stale transport loss fails closed. The return that overtook
            // it was applied while the device was unlatched, so it re-asserted
            // nothing; ask for that now.
            engaged = latched_.insert(device_handle).second;
            reissue = engaged && last.reachable;
        } else {
            last = {edge_seq, reachable};
            if (reachable) {
                return latched_.count(device_handle) != 0;
            }
            engaged = transport_failure && latched_.insert(device_handle).second;
        }
    }
    if (engaged) {
        if (const auto expected = expected_loss_ ? expected_loss_(device_handle) : std::nullopt) {
            LOG_INFO("[DeviceLossLatch] " << device_handle << " went unreachable under " << *expected
                                          << " (expected, not a device fault); held until a deliberate reset");
        } else {
            log_engaged(device_handle);
        }
    }
    return reissue;
}

bool DeviceLossLatch::is_latched(const std::string &device_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latched_.count(device_handle) != 0;
}

void DeviceLossLatch::set_expected_loss(std::function<std::optional<std::string>(const std::string &)> expected_loss) {
    expected_loss_ = std::move(expected_loss);
}

std::vector<std::string> DeviceLossLatch::release_all() {
    std::vector<std::string> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released.assign(latched_.begin(), latched_.end());
        latched_.clear();
    }
    return released;
}

std::vector<std::string> DeviceLossLatch::latched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {latched_.begin(), latched_.end()};
}

state::StateCache::DeviceReachabilitySink make_reachability_sink(DeviceLossLatch &latch,
                                                                 std::function<void(const std::string &)> reissue) {
    return [&latch, reissue = std::move(reissue)](const std::string &device_handle, bool reachable,
                                                  anolis::deviceprovider::v1::Status_Code last_status,
                                                  std::uint64_t edge_seq) {
        if (latch.apply_reachability_edge(device_handle, reachable, last_status, edge_seq)) {
            reissue(device_handle);
        }
    };
}

}  // namespace control
}  // namespace anolis
