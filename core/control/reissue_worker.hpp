#pragma once

/**
 * @file reissue_worker.hpp
 * @brief Runs a returning latched device's safe-state re-assert off the
 *        caller's thread (anolishq/anolis#427).
 *
 * A reachability edge can be delivered on the thread of CallRouter's post-call
 * poll, which holds that provider's lock. The re-assert goes back through
 * CallRouter to the same provider, and on that thread would block on the lock
 * it already holds. post() only queues; one worker thread, which holds no
 * provider lock when it starts a re-assert, runs them in order.
 *
 * Every posted handle is re-asserted at least once after the post:
 * - A handle already waiting is not queued twice; the one run covers both.
 *   A handle posted while its own re-assert is running is queued again.
 * - stop() runs everything still queued, then joins. Only a post() after
 *   stop() is dropped, with a warning; the runtime stops this after
 *   polling, HTTP and automation have stopped, so nothing posts then.
 * - An exception from the re-assert is logged and the worker carries on.
 *
 * Lock order: `mutex_` is a leaf. post() takes it, possibly under a
 * provider lock, and takes nothing else; the worker releases it before
 * calling the re-assert.
 */

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "control/device_loss_latch.hpp"
#include "state/state_cache.hpp"

namespace anolis {
namespace control {

class ReissueWorker {
public:
    /** @brief Start the worker thread. `reissue` re-asserts one device's safe state. */
    explicit ReissueWorker(std::function<void(const std::string &)> reissue);

    /** @brief stop(). */
    ~ReissueWorker();

    ReissueWorker(const ReissueWorker &) = delete;
    ReissueWorker &operator=(const ReissueWorker &) = delete;

    /** @brief Queue a re-assert for this device. Never runs it on the caller's thread. */
    void post(const std::string &device_handle);

    /** @brief Run what is queued, then join the worker. Idempotent. */
    void stop();

private:
    void run();

    std::function<void(const std::string &)> reissue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

/**
 * @brief The sink Runtime installs: make_reachability_sink(latch, reissue)
 *        with each re-assert posted to `worker` instead of run on the thread
 *        that delivered the edge.
 */
state::StateCache::DeviceReachabilitySink make_reachability_sink(DeviceLossLatch &latch, ReissueWorker &worker);

}  // namespace control
}  // namespace anolis
