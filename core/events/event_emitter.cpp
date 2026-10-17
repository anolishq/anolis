#include "event_emitter.hpp"

#include <chrono>
#include <type_traits>
#include <utility>
#include <vector>

#include "logging/logger.hpp"

namespace anolis {
namespace events {

SubscriberQueue::SubscriberQueue(size_t max_size, const std::string &name)
    : max_size_(max_size), name_(name), dropped_count_(0) {}

bool SubscriberQueue::push(const Event &event) {
    // Capture log data while locked; log after unlocking.
    bool should_log = false;
    size_t dropped_total = 0;
    bool overflowed = false;
    std::optional<Event> dropped_event;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.size() >= max_size_) {
            dropped_event = std::move(queue_.front());
            queue_.pop();
            dropped_count_++;
            dropped_total = dropped_count_;
            overflowed = true;

            // Log first drop and then every 100 drops for the subscriber queue.
            should_log = (dropped_count_ % 100 == 1);
        }

        queue_.push(event);
    }

    cv_.notify_one();
    dropped_event.reset();

    if (should_log) {
        LOG_WARN("[EventEmitter] Queue '" << name_ << "' overflow, dropped " << dropped_total << " events total");
    }

    return !overflowed;
}

std::optional<Event> SubscriberQueue::pop(int timeout_ms) {
    std::optional<Event> event;

    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (timeout_ms > 0) {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty() || closed_; });
        }

        if (queue_.empty()) {
            return std::nullopt;
        }

        event = std::move(queue_.front());
        queue_.pop();
    }

    return event;
}

std::optional<Event> SubscriberQueue::try_pop() { return pop(0); }

size_t SubscriberQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool SubscriberQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

size_t SubscriberQueue::dropped_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_count_;
}

void SubscriberQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    cv_.notify_all();
}

bool SubscriberQueue::is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

Subscription::Subscription(SubscriptionId id, std::shared_ptr<SubscriberQueue> queue,
                           std::function<void(SubscriptionId)> unsubscribe_fn)
    : id_(id), queue_(std::move(queue)), unsubscribe_fn_(std::move(unsubscribe_fn)) {}

Subscription::~Subscription() { unsubscribe(); }

Subscription::Subscription(Subscription &&other) noexcept
    : id_(other.id_), queue_(std::move(other.queue_)), unsubscribe_fn_(std::move(other.unsubscribe_fn_)) {
    other.id_ = 0;
}

Subscription &Subscription::operator=(Subscription &&other) noexcept {
    if (this != &other) {
        unsubscribe();
        id_ = other.id_;
        queue_ = std::move(other.queue_);
        unsubscribe_fn_ = std::move(other.unsubscribe_fn_);
        other.id_ = 0;
    }
    return *this;
}

std::optional<Event> Subscription::pop(int timeout_ms) {
    if (!queue_) {
        return std::nullopt;
    }
    return queue_->pop(timeout_ms);
}

std::optional<Event> Subscription::try_pop() {
    if (!queue_) {
        return std::nullopt;
    }
    return queue_->try_pop();
}

Subscription::SubscriptionId Subscription::id() const { return id_; }

bool Subscription::is_active() const { return queue_ != nullptr && !queue_->is_closed(); }

size_t Subscription::queue_size() const { return queue_ ? queue_->size() : 0; }

size_t Subscription::dropped_count() const { return queue_ ? queue_->dropped_count() : 0; }

void Subscription::unsubscribe() {
    if (id_ != 0 && unsubscribe_fn_) {
        // Guard against calling into a destroyed emitter: if the emitter was
        // destroyed before this subscription, it will have closed the queue.
        // Only fire the raw-this lambda when the queue is still open.
        if (queue_ && !queue_->is_closed()) {
            unsubscribe_fn_(id_);
        }
        id_ = 0;
        if (queue_) {
            queue_->close();
        }
    }
}

bool EventFilter::matches(const Event &event) const {
    return std::visit(
        [this](auto &&e) -> bool {
            using T = std::decay_t<decltype(e)>;

            if (!provider_id.empty()) {
                if constexpr (std::is_same_v<T, StateUpdateEvent> || std::is_same_v<T, QualityChangeEvent> ||
                              std::is_same_v<T, DeviceAvailabilityEvent> || std::is_same_v<T, DeviceRemovedEvent>) {
                    if (e.provider_id != provider_id) {
                        return false;
                    }
                }
            }

            if (!device_id.empty()) {
                if constexpr (std::is_same_v<T, StateUpdateEvent> || std::is_same_v<T, QualityChangeEvent> ||
                              std::is_same_v<T, DeviceAvailabilityEvent> || std::is_same_v<T, DeviceRemovedEvent>) {
                    if (e.device_id != device_id) {
                        return false;
                    }
                }
            }

            if (!signal_id.empty()) {
                if constexpr (std::is_same_v<T, StateUpdateEvent> || std::is_same_v<T, QualityChangeEvent>) {
                    if (e.signal_id != signal_id) {
                        return false;
                    }
                }
            }

            return true;
        },
        event);
}

EventFilter EventFilter::all() { return EventFilter{}; }

EventEmitter::EventEmitter(size_t default_queue_size, size_t max_subscribers)
    : default_queue_size_(default_queue_size),
      max_subscribers_(max_subscribers),
      next_subscription_id_(1),
      next_event_id_(1) {}

EventEmitter::~EventEmitter() {
    // Close all subscriber queues so that any surviving Subscription objects
    // see is_active() == false and skip the raw-this unsubscribe_fn_ call in
    // their destructors, preventing use-after-free.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &[id, info] : subscribers_) {
        info.queue->close();
    }
}

std::unique_ptr<Subscription> EventEmitter::subscribe(const EventFilter &filter, size_t queue_size,
                                                      const std::string &name) {
    SubscriptionId id = 0;
    size_t subscriber_count = 0;
    std::shared_ptr<SubscriberQueue> queue;
    bool rejected = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (max_subscribers_ > 0 && subscribers_.size() >= max_subscribers_) {
            rejected = true;
            subscriber_count = max_subscribers_;
        } else {
            id = next_subscription_id_++;
            const auto actual_queue_size = queue_size > 0 ? queue_size : default_queue_size_;
            queue = std::make_shared<SubscriberQueue>(actual_queue_size, name);

            subscribers_[id] = SubscriberInfo{queue, filter, name};
            subscriber_count = subscribers_.size();
        }
    }

    if (rejected) {
        LOG_WARN("[EventEmitter] Max subscribers (" << subscriber_count << ") reached, rejecting subscription");
        return nullptr;
    }

    LOG_DEBUG("[EventEmitter] Subscription " << id << " created" << (name.empty() ? "" : " (" + name + ")")
                                             << ", total subscribers: " << subscriber_count);

    auto unsubscribe_fn = [this](SubscriptionId sub_id) { this->unsubscribe(sub_id); };
    return std::make_unique<Subscription>(id, std::move(queue), std::move(unsubscribe_fn));
}

void EventEmitter::emit(Event event) {
    std::vector<std::shared_ptr<SubscriberQueue>> targets;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t id = next_event_id_++;
        std::visit([id](auto &&e) { e.event_id = id; }, event);

        for (auto &[sub_id, info] : subscribers_) {
            static_cast<void>(sub_id);
            if (info.filter.matches(event)) {
                targets.push_back(info.queue);
            }
        }
    }

    for (auto &queue : targets) {
        queue->push(event);
    }
}

uint64_t EventEmitter::next_event_id() const { return next_event_id_.load(); }

size_t EventEmitter::subscriber_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribers_.size();
}

size_t EventEmitter::max_subscribers() const { return max_subscribers_; }

bool EventEmitter::at_capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_subscribers_ > 0 && subscribers_.size() >= max_subscribers_;
}

void EventEmitter::unsubscribe(SubscriptionId id) {
    bool found = false;
    size_t remaining = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscribers_.find(id);
        if (it != subscribers_.end()) {
            it->second.queue->close();
            subscribers_.erase(it);
            found = true;
            remaining = subscribers_.size();
        }
    }

    if (found) {
        LOG_DEBUG("[EventEmitter] Subscription " << id << " removed, remaining: " << remaining);
    }
}

}  // namespace events
}  // namespace anolis
