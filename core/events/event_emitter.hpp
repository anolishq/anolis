#pragma once

/**
 * @file event_emitter.hpp
 * @brief Thread-safe fan-out event dispatcher with per-subscriber queues
 *
 * Architecture:
 * - StateCache emits events to a single EventEmitter instance
 * - Each subscriber (SSE client, telemetry sink) gets its own bounded queue
 * - Fan-out is non-blocking: slow subscribers don't block StateCache or others
 * - Overflow drops oldest events per-subscriber with warning log
 *
 * Thread safety:
 * - emit() is called from StateCache's polling threads, one per provider
 * - subscribe()/unsubscribe() called from HTTP threads (SSE handlers)
 * - pop() called from subscriber threads (SSE handler loop, telemetry flush)
 *
 * All operations are mutex-protected with minimal critical sections.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

#include "event_types.hpp"

namespace anolis {
namespace events {

/**
 * @brief Bounded event queue for a single subscriber
 *
 * Each subscriber gets their own queue with independent overflow handling.
 * This isolates slow consumers from affecting others.
 */
class SubscriberQueue {
public:
    explicit SubscriberQueue(size_t max_size, const std::string &name = "");

    /**
     * @brief Push event to queue (producer side)
     *
     * If queue is full, drops oldest event and logs warning.
     * Never blocks - always returns immediately.
     *
     * @param event Event to push
     * @return true if pushed, false if dropped (queue was full, oldest dropped)
     */
    bool push(const Event &event);

    /**
     * @brief Pop event from queue (consumer side)
     *
     * Blocks until event is available or timeout expires.
     *
     * @param timeout_ms Max time to wait (0 = non-blocking)
     * @return Event if available, std::nullopt on timeout
     */
    std::optional<Event> pop(int timeout_ms = 0);

    /**
     * @brief Try to pop without blocking
     */
    std::optional<Event> try_pop();

    /**
     * @brief Get current queue size
     */
    size_t size() const;

    /**
     * @brief Check if queue is empty
     */
    bool empty() const;

    /**
     * @brief Get total dropped event count
     */
    size_t dropped_count() const;

    /**
     * @brief Close queue (unblocks waiting consumers)
     */
    void close();

    /**
     * @brief Check if queue is closed
     */
    bool is_closed() const;

private:
    const size_t max_size_;
    const std::string name_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Event> queue_;
    size_t dropped_count_;
    bool closed_ = false;
};

/**
 * @brief Subscription handle returned to subscribers
 *
 * Subscribers hold this to receive events and unsubscribe.
 * RAII: unsubscribes automatically on destruction.
 */
class Subscription {
public:
    using SubscriptionId = uint64_t;

    Subscription(SubscriptionId id, std::shared_ptr<SubscriberQueue> queue,
                 std::function<void(SubscriptionId)> unsubscribe_fn);
    ~Subscription();

    // Non-copyable (unique ownership)
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    // Movable
    Subscription(Subscription &&other) noexcept;
    Subscription &operator=(Subscription &&other) noexcept;

    /**
     * @brief Pop next event (blocks up to timeout_ms)
     */
    std::optional<Event> pop(int timeout_ms = 100);

    /**
     * @brief Try pop without blocking
     */
    std::optional<Event> try_pop();

    /**
     * @brief Get subscription ID
     */
    SubscriptionId id() const;

    /**
     * @brief Check if still subscribed
     */
    bool is_active() const;

    /**
     * @brief Get queue size
     */
    size_t queue_size() const;

    /**
     * @brief Get dropped count
     */
    size_t dropped_count() const;

    /**
     * @brief Manually unsubscribe
     */
    void unsubscribe();

private:
    SubscriptionId id_;
    std::shared_ptr<SubscriberQueue> queue_;
    std::function<void(SubscriptionId)> unsubscribe_fn_;
};

/**
 * @brief Event filter for subscribers
 *
 * Allows filtering events by provider/device/signal.
 * Empty filter = receive all events.
 */
struct EventFilter {
    std::string provider_id;  // Empty = all providers
    std::string device_id;    // Empty = all devices
    std::string signal_id;    // Empty = all signals

    /**
     * @brief Check if event matches filter
     */
    bool matches(const Event &event) const;

    /**
     * @brief Create filter that matches all events
     */
    static EventFilter all();
};

/**
 * @brief Thread-safe event emitter with fan-out to per-subscriber queues
 *
 * Central hub for event distribution. StateCache calls emit() on changes,
 * and all subscribed consumers receive a copy in their own queue.
 */
class EventEmitter {
public:
    using SubscriptionId = Subscription::SubscriptionId;

    /**
     * @brief Create emitter with default queue sizes
     *
     * @param default_queue_size Default max events per subscriber queue
     * @param max_subscribers Maximum concurrent subscribers (0 = unlimited)
     */
    explicit EventEmitter(size_t default_queue_size = 100,
                          size_t max_subscribers = 32);  // NOLINT(bugprone-easily-swappable-parameters)

    /**
     * @brief Destroy emitter, closing all subscriber queues.
     *
     * Surviving Subscription objects will see is_active() == false and their
     * destructors will skip the raw-this unsubscribe callback, preventing UAF.
     */
    ~EventEmitter();

    /**
     * @brief Subscribe to events
     *
     * Creates a new subscription with its own bounded queue.
     *
     * @param filter Event filter (empty = all events)
     * @param queue_size Override default queue size (0 = use default)
     * @param name Debug name for logging
     * @return Subscription handle, or nullptr if max subscribers reached
     */
    std::unique_ptr<Subscription> subscribe(const EventFilter &filter = EventFilter::all(), size_t queue_size = 0,
                                            const std::string &name = "");

    /**
     * @brief Emit event to all subscribers
     *
     * Assigns monotonic event_id and fans out to all matching subscribers.
     * Non-blocking: each subscriber's queue handles its own overflow.
     *
     * @param event Event to emit (event_id will be assigned)
     */
    void emit(Event event);

    /**
     * @brief Get next event ID (for external use if needed)
     */
    uint64_t next_event_id() const;

    /**
     * @brief Get current subscriber count
     */
    size_t subscriber_count() const;

    /**
     * @brief Get max subscribers limit
     */
    size_t max_subscribers() const;

    /**
     * @brief Check if at capacity
     */
    bool at_capacity() const;

private:
    void unsubscribe(SubscriptionId id);

    struct SubscriberInfo {
        std::shared_ptr<SubscriberQueue> queue;
        EventFilter filter;
        std::string name;
    };

    const size_t default_queue_size_;
    const size_t max_subscribers_;

    mutable std::mutex mutex_;
    std::unordered_map<SubscriptionId, SubscriberInfo> subscribers_;
    std::atomic<SubscriptionId> next_subscription_id_;
    std::atomic<uint64_t> next_event_id_;
};

}  // namespace events
}  // namespace anolis
