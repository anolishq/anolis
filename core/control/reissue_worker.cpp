#include "control/reissue_worker.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "logging/logger.hpp"

namespace anolis {
namespace control {

ReissueWorker::ReissueWorker(std::function<void(const std::string &)> reissue)
    : reissue_(std::move(reissue)), thread_([this] { run(); }) {}

ReissueWorker::~ReissueWorker() { stop(); }

void ReissueWorker::post(const std::string &device_handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            LOG_WARN("[ReissueWorker] " << device_handle
                                        << " returned while latched during shutdown; its safe state was not "
                                           "re-asserted");
            return;
        }
        if (std::find(queue_.begin(), queue_.end(), device_handle) != queue_.end()) {
            return;  // already waiting; that run covers this request too
        }
        queue_.push_back(device_handle);
    }
    wake_.notify_one();
}

void ReissueWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ReissueWorker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
            return;  // stopping, and everything queued has run
        }
        const std::string device_handle = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        try {
            reissue_(device_handle);
        } catch (const std::exception &e) {
            LOG_ERROR("[ReissueWorker] safe-state re-assert for " << device_handle << " threw: " << e.what());
        } catch (...) {
            LOG_ERROR("[ReissueWorker] safe-state re-assert for " << device_handle << " threw");
        }
        lock.lock();
    }
}

state::StateCache::DeviceReachabilitySink make_reachability_sink(DeviceLossLatch &latch, ReissueWorker &worker) {
    return make_reachability_sink(latch, [&worker](const std::string &device_handle) { worker.post(device_handle); });
}

}  // namespace control
}  // namespace anolis
