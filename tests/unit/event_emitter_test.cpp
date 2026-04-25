/**
 * event_emitter_test.cpp - EventEmitter and SubscriberQueue unit tests
 *
 * Tests:
 * 1. Basic subscription lifecycle (subscribe, emit, pop, unsubscribe)
 * 2. Queue overflow handling (drops oldest, tracks count)
 * 3. Multiple subscriber isolation (slow subscriber doesn't block others)
 * 4. Timeout and blocking behavior (pop with timeout, try_pop)
 * 5. Event filtering (provider/device/signal filters)
 * 6. Subscriber limit enforcement (max_subscribers)
 * 7. RAII cleanup (Subscription destructor unsubscribes)
 * 8. Event ID monotonicity
 * 9. Thread-safety (concurrent emit, subscribe, unsubscribe)
 * 10. Subscription during emission
 */

#include "events/event_emitter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "events/event_types.hpp"

using namespace anolis::events;
using namespace std::chrono_literals;

/**
 * Helper: Create a test StateUpdateEvent
 */
StateUpdateEvent create_test_event(const std::string &provider_id = "test_provider",
                                   const std::string &device_id = "test_device",
                                   const std::string &signal_id = "test_signal", double value = 42.0) {
    StateUpdateEvent evt;
    evt.provider_id = provider_id;
    evt.device_id = device_id;
    evt.signal_id = signal_id;
    evt.timestamp_ms = 1234567890;
    evt.value = value;
    evt.event_id = 0;  // Will be assigned by emitter
    return evt;
}

/**
 * Helper: Create a test DeviceAvailabilityEvent
 */
DeviceAvailabilityEvent create_availability_event(const std::string &provider_id = "test_provider",
                                                  const std::string &device_id = "test_device", bool available = true) {
    DeviceAvailabilityEvent evt;
    evt.provider_id = provider_id;
    evt.device_id = device_id;
    evt.available = available;
    evt.event_id = 0;
    return evt;
}

// ============================================================================
// Basic Subscription Tests
// ============================================================================

TEST(EventEmitterTest, SubscribeAndEmit) {
    EventEmitter emitter(10);  // Small queue for testing

    // Subscribe
    auto sub = emitter.subscribe();
    ASSERT_NE(sub, nullptr);
    EXPECT_TRUE(sub->is_active());

    // Emit event
    Event event = create_test_event();
    emitter.emit(event);

    // Pop event
    auto received = sub->pop(100);
    ASSERT_TRUE(received.has_value());

    // Verify it's a StateUpdateEvent
    ASSERT_TRUE(std::holds_alternative<StateUpdateEvent>(*received));
    auto &state_evt = std::get<StateUpdateEvent>(*received);
    EXPECT_EQ(state_evt.provider_id, "test_provider");
    EXPECT_EQ(state_evt.device_id, "test_device");
    EXPECT_EQ(state_evt.signal_id, "test_signal");
    EXPECT_EQ(std::get<double>(state_evt.value), 42.0);
}

TEST(EventEmitterTest, MultipleSubscribers) {
    EventEmitter emitter;

    // Create 3 subscribers
    auto sub1 = emitter.subscribe(EventFilter::all(), 0, "sub1");
    auto sub2 = emitter.subscribe(EventFilter::all(), 0, "sub2");
    auto sub3 = emitter.subscribe(EventFilter::all(), 0, "sub3");

    EXPECT_EQ(emitter.subscriber_count(), 3);

    // Emit event
    Event event = create_test_event("prov1", "dev1", "sig1", 123.0);
    emitter.emit(event);

    // All subscribers should receive it
    auto evt1 = sub1->pop(100);
    auto evt2 = sub2->pop(100);
    auto evt3 = sub3->pop(100);

    EXPECT_TRUE(evt1.has_value());
    EXPECT_TRUE(evt2.has_value());
    EXPECT_TRUE(evt3.has_value());

    // Verify content
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt1).value), 123.0);
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt2).value), 123.0);
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt3).value), 123.0);
}

TEST(EventEmitterTest, UnsubscribeRemovesSubscriber) {
    EventEmitter emitter;

    auto sub1 = emitter.subscribe();
    auto sub2 = emitter.subscribe();
    EXPECT_EQ(emitter.subscriber_count(), 2);

    // Unsubscribe one
    sub1->unsubscribe();
    EXPECT_EQ(emitter.subscriber_count(), 1);

    // Emit event - only sub2 should receive
    emitter.emit(create_test_event());

    auto evt1 = sub1->try_pop();
    auto evt2 = sub2->pop(100);

    EXPECT_FALSE(evt1.has_value());  // sub1 unsubscribed
    EXPECT_TRUE(evt2.has_value());   // sub2 still active
}

TEST(EventEmitterTest, SubscriptionRAIICleanup) {
    EventEmitter emitter;

    {
        auto sub = emitter.subscribe();
        EXPECT_EQ(emitter.subscriber_count(), 1);
    }  // sub goes out of scope

    // Subscriber should be cleaned up
    EXPECT_EQ(emitter.subscriber_count(), 0);
}

// ============================================================================
// Queue Overflow Tests
// ============================================================================

TEST(EventEmitterTest, QueueOverflowDropsOldest) {
    EventEmitter emitter;
    auto sub = emitter.subscribe(EventFilter::all(), 3);  // Queue size = 3

    // Fill queue (3 events)
    emitter.emit(create_test_event("p", "d", "s", 1.0));
    emitter.emit(create_test_event("p", "d", "s", 2.0));
    emitter.emit(create_test_event("p", "d", "s", 3.0));

    EXPECT_EQ(sub->queue_size(), 3);
    EXPECT_EQ(sub->dropped_count(), 0);

    // Add one more - should drop oldest (value=1.0)
    emitter.emit(create_test_event("p", "d", "s", 4.0));

    EXPECT_EQ(sub->queue_size(), 3);
    EXPECT_EQ(sub->dropped_count(), 1);

    // Pop events - should get 2.0, 3.0, 4.0 (1.0 was dropped)
    auto evt1 = sub->pop(100);
    auto evt2 = sub->pop(100);
    auto evt3 = sub->pop(100);

    ASSERT_TRUE(evt1.has_value());
    ASSERT_TRUE(evt2.has_value());
    ASSERT_TRUE(evt3.has_value());

    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt1).value), 2.0);
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt2).value), 3.0);
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt3).value), 4.0);
}

TEST(EventEmitterTest, DroppedCountTracksOverflows) {
    EventEmitter emitter;
    auto sub = emitter.subscribe(EventFilter::all(), 2);  // Tiny queue

    // Overflow multiple times
    for (int i = 0; i < 10; ++i) {
        emitter.emit(create_test_event("p", "d", "s", static_cast<double>(i)));
    }

    // Queue stays at capacity, but dropped count increases
    EXPECT_EQ(sub->queue_size(), 2);
    EXPECT_EQ(sub->dropped_count(), 8);  // 10 pushed - 2 kept = 8 dropped
}

// ============================================================================
// Subscriber Isolation Tests
// ============================================================================

TEST(EventEmitterTest, SlowSubscriberDoesNotBlockEmit) {
    EventEmitter emitter;

    // Create slow subscriber (doesn't pop events)
    auto slow_sub = emitter.subscribe(EventFilter::all(), 5, "slow");

    // Create fast subscriber
    auto fast_sub = emitter.subscribe(EventFilter::all(), 100, "fast");

    // Emit 10 events quickly
    for (int i = 0; i < 10; ++i) {
        emitter.emit(create_test_event("p", "d", "s", static_cast<double>(i)));
    }

    // Slow subscriber overflowed (queue size = 5)
    EXPECT_EQ(slow_sub->queue_size(), 5);
    EXPECT_GT(slow_sub->dropped_count(), 0);

    // Fast subscriber got all events (queue size = 100)
    EXPECT_EQ(fast_sub->queue_size(), 10);
    EXPECT_EQ(fast_sub->dropped_count(), 0);
}

TEST(EventEmitterTest, OneSubscriberOverflowDoesNotAffectOthers) {
    EventEmitter emitter;

    auto sub1 = emitter.subscribe(EventFilter::all(), 2, "tiny");   // Tiny queue
    auto sub2 = emitter.subscribe(EventFilter::all(), 100, "big");  // Big queue

    // Emit 5 events
    for (int i = 0; i < 5; ++i) {
        emitter.emit(create_test_event());
    }

    // sub1 overflowed
    EXPECT_EQ(sub1->dropped_count(), 3);

    // sub2 unaffected
    EXPECT_EQ(sub2->dropped_count(), 0);
    EXPECT_EQ(sub2->queue_size(), 5);
}

// ============================================================================
// Timeout and Blocking Tests
// ============================================================================

TEST(EventEmitterTest, PopBlocksUntilEventAvailable) {
    EventEmitter emitter;
    auto sub = emitter.subscribe();

    // Scale timing for sanitizer builds (2-10x overhead)
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const auto emit_delay = 100ms;
    const int timeout_ms = 500;
    const auto min_elapsed = 50ms;
    const auto max_elapsed = 400ms;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    const auto emit_delay = 100ms;
    const int timeout_ms = 500;
    const auto min_elapsed = 50ms;
    const auto max_elapsed = 400ms;
#else
    const auto emit_delay = 50ms;
    const int timeout_ms = 200;
    const auto min_elapsed = 25ms;
    const auto max_elapsed = 150ms;
#endif
#else
    const auto emit_delay = 50ms;
    const int timeout_ms = 200;
    const auto min_elapsed = 40ms;
    const auto max_elapsed = 150ms;
#endif

    // Start thread that emits after delay
    std::thread emitter_thread([&emitter, emit_delay]() {
        std::this_thread::sleep_for(emit_delay);
        emitter.emit(create_test_event());
    });

    // pop() should block and return event
    auto start = std::chrono::steady_clock::now();
    auto event = sub->pop(timeout_ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(event.has_value());
    EXPECT_GE(elapsed, min_elapsed);
    EXPECT_LT(elapsed, max_elapsed);

    emitter_thread.join();
}

TEST(EventEmitterTest, PopTimesOutIfNoEvent) {
    EventEmitter emitter;
    auto sub = emitter.subscribe();

    // pop() with timeout, no event emitted
    auto start = std::chrono::steady_clock::now();
    auto event = sub->pop(50);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(event.has_value());  // Timeout
    EXPECT_GE(elapsed, 40ms);         // Waited ~50ms
    EXPECT_LT(elapsed, 80ms);
}

TEST(EventEmitterTest, TryPopReturnsImmediately) {
    EventEmitter emitter;
    auto sub = emitter.subscribe();

    // try_pop() should return immediately
    auto start = std::chrono::steady_clock::now();
    auto event = sub->try_pop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(event.has_value());
    EXPECT_LT(elapsed, 10ms);  // Should be nearly instant
}

// ============================================================================
// Event Filtering Tests
// ============================================================================

TEST(EventEmitterTest, FilterByProvider) {
    EventEmitter emitter;

    EventFilter filter;
    filter.provider_id = "provider1";

    auto sub = emitter.subscribe(filter);

    // Emit matching event
    emitter.emit(create_test_event("provider1", "dev1", "sig1"));

    // Emit non-matching event
    emitter.emit(create_test_event("provider2", "dev2", "sig2"));

    // Should receive only matching event
    auto evt = sub->pop(100);
    ASSERT_TRUE(evt.has_value());
    EXPECT_EQ(std::get<StateUpdateEvent>(*evt).provider_id, "provider1");

    // No more events
    auto evt2 = sub->try_pop();
    EXPECT_FALSE(evt2.has_value());
}

TEST(EventEmitterTest, FilterByDevice) {
    EventEmitter emitter;

    EventFilter filter;
    filter.device_id = "device1";

    auto sub = emitter.subscribe(filter);

    emitter.emit(create_test_event("p1", "device1", "sig1"));
    emitter.emit(create_test_event("p2", "device2", "sig2"));

    auto evt = sub->pop(100);
    ASSERT_TRUE(evt.has_value());
    EXPECT_EQ(std::get<StateUpdateEvent>(*evt).device_id, "device1");

    EXPECT_FALSE(sub->try_pop().has_value());
}

TEST(EventEmitterTest, FilterBySignal) {
    EventEmitter emitter;

    EventFilter filter;
    filter.signal_id = "temperature";

    auto sub = emitter.subscribe(filter);

    emitter.emit(create_test_event("p1", "d1", "temperature", 25.0));
    emitter.emit(create_test_event("p1", "d1", "pressure", 100.0));

    auto evt = sub->pop(100);
    ASSERT_TRUE(evt.has_value());
    EXPECT_EQ(std::get<StateUpdateEvent>(*evt).signal_id, "temperature");

    EXPECT_FALSE(sub->try_pop().has_value());
}

TEST(EventEmitterTest, CombinedFilters) {
    EventEmitter emitter;

    EventFilter filter;
    filter.provider_id = "provider1";
    filter.device_id = "device1";
    filter.signal_id = "signal1";

    auto sub = emitter.subscribe(filter);

    // Emit exact match
    emitter.emit(create_test_event("provider1", "device1", "signal1", 111.0));

    // Emit partial matches (should be filtered out)
    emitter.emit(create_test_event("provider1", "device1", "signal2", 222.0));
    emitter.emit(create_test_event("provider1", "device2", "signal1", 333.0));
    emitter.emit(create_test_event("provider2", "device1", "signal1", 444.0));

    // Should receive only exact match
    auto evt = sub->pop(100);
    ASSERT_TRUE(evt.has_value());
    EXPECT_EQ(std::get<double>(std::get<StateUpdateEvent>(*evt).value), 111.0);

    EXPECT_FALSE(sub->try_pop().has_value());
}

// ============================================================================
// Max Subscribers Tests
// ============================================================================

TEST(EventEmitterTest, MaxSubscriberLimitEnforced) {
    EventEmitter emitter(100, 3);  // Max 3 subscribers

    auto sub1 = emitter.subscribe();
    auto sub2 = emitter.subscribe();
    auto sub3 = emitter.subscribe();

    EXPECT_EQ(emitter.subscriber_count(), 3);
    EXPECT_TRUE(emitter.at_capacity());

    // 4th subscription should fail
    auto sub4 = emitter.subscribe();
    EXPECT_EQ(sub4, nullptr);
    EXPECT_EQ(emitter.subscriber_count(), 3);
}

TEST(EventEmitterTest, UnsubscribeFreesCapacity) {
    EventEmitter emitter(100, 2);  // Max 2 subscribers

    auto sub1 = emitter.subscribe();
    auto sub2 = emitter.subscribe();
    EXPECT_TRUE(emitter.at_capacity());

    // Unsubscribe one
    sub1->unsubscribe();
    EXPECT_FALSE(emitter.at_capacity());

    // Can now subscribe again
    auto sub3 = emitter.subscribe();
    EXPECT_NE(sub3, nullptr);
    EXPECT_TRUE(emitter.at_capacity());
}

// ============================================================================
// Event ID Monotonicity Tests
// ============================================================================

TEST(EventEmitterTest, EventIDsAreMonotonic) {
    EventEmitter emitter;
    auto sub = emitter.subscribe();

    // Emit 5 events
    for (int i = 0; i < 5; ++i) {
        emitter.emit(create_test_event());
    }

    // Pop and verify IDs are monotonically increasing
    uint64_t prev_id = 0;
    for (int i = 0; i < 5; ++i) {
        auto evt = sub->pop(100);
        ASSERT_TRUE(evt.has_value());

        uint64_t event_id = std::visit([](auto &&e) { return e.event_id; }, *evt);

        EXPECT_GT(event_id, prev_id);
        prev_id = event_id;
    }
}

// ============================================================================
// Thread-Safety Tests
// ============================================================================

TEST(EventEmitterTest, ConcurrentEmitAndSubscribe) {
    // Scale down for sanitizer builds (2-10x overhead)
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const int NUM_EVENTS = 50;
    const auto EMIT_DELAY = 2ms;
    const auto POP_TIMEOUT = 100;
    const int NUM_SUBSCRIBERS = 3;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    const int NUM_EVENTS = 50;
    const auto EMIT_DELAY = 2ms;
    const auto POP_TIMEOUT = 100;
    const int NUM_SUBSCRIBERS = 3;
#else
    const int NUM_EVENTS = 100;
    const auto EMIT_DELAY = 1ms;
    const auto POP_TIMEOUT = 50;
    const int NUM_SUBSCRIBERS = 5;
#endif
#else
    const int NUM_EVENTS = 100;
    const auto EMIT_DELAY = 1ms;
    const auto POP_TIMEOUT = 50;
    const int NUM_SUBSCRIBERS = 5;
#endif

    EventEmitter emitter(100, 50);

    std::atomic<int> total_received{0};
    std::vector<std::thread> threads;
    threads.reserve(NUM_SUBSCRIBERS + 1);

    // Emitter thread
    threads.emplace_back([&emitter, EMIT_DELAY, num_events = NUM_EVENTS]() {
        for (int i = 0; i < num_events; ++i) {
            emitter.emit(create_test_event("p", "d", "s", static_cast<double>(i)));
            std::this_thread::sleep_for(EMIT_DELAY);
        }
    });

    // Multiple subscriber threads
    for (int t = 0; t < NUM_SUBSCRIBERS; ++t) {
        threads.emplace_back([&emitter, &total_received, pop_timeout = POP_TIMEOUT]() {
            auto sub = emitter.subscribe();
            if (sub) {
                while (auto evt = sub->pop(pop_timeout)) {
                    total_received++;
                }
            }
        });
    }

    // Wait for completion
    for (auto &th : threads) {
        th.join();
    }

    // Each subscriber should have received events
    EXPECT_GT(total_received, 0);
}

TEST(EventEmitterTest, UnsubscribeDuringEmission) {
    EventEmitter emitter;

    std::vector<std::unique_ptr<Subscription>> subs;
    subs.reserve(5);
    for (int i = 0; i < 5; ++i) {
        subs.push_back(emitter.subscribe());
    }

    // Emit while unsubscribing
    std::thread emitter_thread([&emitter]() {
        for (int i = 0; i < 50; ++i) {
            emitter.emit(create_test_event());
            std::this_thread::sleep_for(2ms);
        }
    });

    std::thread unsubscriber_thread([&subs]() {
        std::this_thread::sleep_for(10ms);
        for (auto &sub : subs) {
            sub->unsubscribe();
            std::this_thread::sleep_for(5ms);
        }
    });

    emitter_thread.join();
    unsubscriber_thread.join();

    // Should complete without crashes
    EXPECT_EQ(emitter.subscriber_count(), 0);
}

// ============================================================================
// F3: Subscription Lifetime / Emitter Destruction Tests
// ============================================================================

// After emitter is destroyed the surviving subscription's queue must be closed
// so that is_active() returns false and pop() unblocks.
// FAILS before fix: emitter destructor does not close queues.
TEST(EventEmitterTest, SubscriptionQueueClosedWhenEmitterDestroyed) {
    std::unique_ptr<Subscription> sub;
    {
        EventEmitter emitter(10, 10);
        sub = emitter.subscribe();
        ASSERT_NE(sub, nullptr);
        EXPECT_TRUE(sub->is_active());
    }
    // Emitter destroyed above; queue must be closed by emitter destructor.
    EXPECT_FALSE(sub->is_active());
}

// Destroying a subscription after its emitter must not crash or invoke UB.
// The queue is closed by the emitter destructor, so unsubscribe() must skip
// the raw-this lambda call.
// FAILS before fix under ASan/UBSan due to UAF through dangling lambda.
TEST(EventEmitterTest, SubscriptionDestructorAfterEmitterDestroyedIsNoOp) {
    std::unique_ptr<Subscription> sub;
    {
        EventEmitter emitter(10, 10);
        sub = emitter.subscribe();
    }
    // Must not crash. Under ASan/UBSan this hits UAF before the fix.
    sub.reset();
}

// All surviving subscriptions must have their queues closed when the emitter
// is destroyed, even if unsubscribe() was never called on any of them.
// FAILS before fix.
TEST(EventEmitterTest, MultipleSubscriptionsQueueClosedOnEmitterDestroy) {
    std::vector<std::unique_ptr<Subscription>> subs;
    {
        EventEmitter emitter(10, 10);
        for (int i = 0; i < 5; ++i) {
            subs.push_back(emitter.subscribe());
        }
        EXPECT_EQ(emitter.subscriber_count(), 5);
    }
    for (auto &sub : subs) {
        EXPECT_FALSE(sub->is_active());
    }
}
