#include "control/device_loss_latch.hpp"

#include "logging/logger.hpp"

namespace anolis {
namespace control {

void DeviceLossLatch::engage(const std::string &device_handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latched_.insert(device_handle).second) {
            return;  // already latched; the poll loop re-reports every cycle
        }
    }
    LOG_WARN("[DeviceLossLatch] " << device_handle
                                  << " went unreachable; autonomous actuation to it is blocked until "
                                     "MANUAL -> AUTO re-arms it");
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
