#include "control/device_loss_latch.hpp"

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

void DeviceLossLatch::engage(const std::string &device_handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latched_.insert(device_handle).second) {
            return;  // already latched; the poll loop re-reports every cycle
        }
    }
    log_engaged(device_handle);
}

bool DeviceLossLatch::apply_reachability_edge(const std::string &device_handle, bool reachable,
                                              anolis::deviceprovider::v1::Status_Code last_status,
                                              std::uint64_t edge_seq) {
    bool engaged = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &last = last_edge_seq_[device_handle];
        if (edge_seq <= last) {
            LOG_DEBUG("[DeviceLossLatch] dropped a stale " << (reachable ? "return" : "loss") << " edge for "
                                                           << device_handle);
            return false;
        }
        last = edge_seq;

        if (reachable) {
            return latched_.count(device_handle) != 0;
        }

        // Latch on TRANSPORT-level unreachability only. A decode or
        // frame-header mismatch reports INTERNAL and means the device
        // answered -- the reference rig produced three such reads in
        // three minutes while entirely healthy, and latching on those
        // would force a human re-arm several times a day. A powered-off
        // board cannot answer at all, so this loses no detection for the
        // case the latch exists for.
        const bool transport_failure = last_status == anolis::deviceprovider::v1::Status_Code_CODE_UNAVAILABLE ||
                                       last_status == anolis::deviceprovider::v1::Status_Code_CODE_DEADLINE_EXCEEDED;
        engaged = transport_failure && latched_.insert(device_handle).second;
    }
    if (engaged) {
        log_engaged(device_handle);
    }
    return false;
}

bool DeviceLossLatch::is_latched(const std::string &device_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latched_.count(device_handle) != 0;
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

}  // namespace control
}  // namespace anolis
