#include "provider/provider_registry.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mocks/mock_provider_handle.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

class ProviderRegistryTest : public Test {
protected:
    void SetUp() override { registry = std::make_unique<provider::ProviderRegistry>(); }

    std::shared_ptr<MockProviderHandle> create_mock_provider(const std::string &id) {
        auto mock = std::make_shared<StrictMock<MockProviderHandle>>();
        mock->_id = id;
        EXPECT_CALL(*mock, provider_id()).WillRepeatedly(ReturnRef(mock->_id));
        return mock;
    }

    std::unique_ptr<provider::ProviderRegistry> registry;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(ProviderRegistryTest, EmptyRegistry) {
    EXPECT_EQ(registry->provider_count(), 0);
    EXPECT_FALSE(registry->has_provider("nonexistent"));
    EXPECT_EQ(registry->get_provider("nonexistent"), nullptr);
    EXPECT_TRUE(registry->get_all_providers().empty());
    EXPECT_TRUE(registry->get_provider_ids().empty());
}

TEST_F(ProviderRegistryTest, AddAndGetProvider) {
    auto mock = create_mock_provider("sim0");
    registry->add_provider("sim0", mock);

    EXPECT_EQ(registry->provider_count(), 1);
    EXPECT_TRUE(registry->has_provider("sim0"));

    auto retrieved = registry->get_provider("sim0");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->provider_id(), "sim0");
    EXPECT_EQ(retrieved.get(), mock.get());  // Same instance
}

TEST_F(ProviderRegistryTest, AddMultipleProviders) {
    auto mock1 = create_mock_provider("sim0");
    auto mock2 = create_mock_provider("sim1");
    auto mock3 = create_mock_provider("hw0");

    registry->add_provider("sim0", mock1);
    registry->add_provider("sim1", mock2);
    registry->add_provider("hw0", mock3);

    EXPECT_EQ(registry->provider_count(), 3);
    EXPECT_TRUE(registry->has_provider("sim0"));
    EXPECT_TRUE(registry->has_provider("sim1"));
    EXPECT_TRUE(registry->has_provider("hw0"));

    auto all = registry->get_all_providers();
    EXPECT_EQ(all.size(), 3);
    EXPECT_NE(all.find("sim0"), all.end());
    EXPECT_NE(all.find("sim1"), all.end());
    EXPECT_NE(all.find("hw0"), all.end());

    auto ids = registry->get_provider_ids();
    EXPECT_EQ(ids.size(), 3);
    EXPECT_THAT(ids, UnorderedElementsAre("sim0", "sim1", "hw0"));
}

TEST_F(ProviderRegistryTest, ReplaceProvider) {
    auto mock1 = create_mock_provider("sim0");
    auto mock2 = create_mock_provider("sim0");

    registry->add_provider("sim0", mock1);
    EXPECT_EQ(registry->provider_count(), 1);

    // Replace with new instance
    registry->add_provider("sim0", mock2);
    EXPECT_EQ(registry->provider_count(), 1);  // Still 1 provider

    auto retrieved = registry->get_provider("sim0");
    EXPECT_EQ(retrieved.get(), mock2.get());  // New instance
    EXPECT_NE(retrieved.get(), mock1.get());  // Old instance replaced
}

TEST_F(ProviderRegistryTest, RemoveProvider) {
    auto mock = create_mock_provider("sim0");
    registry->add_provider("sim0", mock);

    EXPECT_TRUE(registry->remove_provider("sim0"));
    EXPECT_EQ(registry->provider_count(), 0);
    EXPECT_FALSE(registry->has_provider("sim0"));
    EXPECT_EQ(registry->get_provider("sim0"), nullptr);
}

TEST_F(ProviderRegistryTest, RemoveNonexistentProvider) {
    EXPECT_FALSE(registry->remove_provider("nonexistent"));
    EXPECT_EQ(registry->provider_count(), 0);
}

TEST_F(ProviderRegistryTest, ClearProviders) {
    auto mock1 = create_mock_provider("sim0");
    auto mock2 = create_mock_provider("sim1");

    registry->add_provider("sim0", mock1);
    registry->add_provider("sim1", mock2);
    EXPECT_EQ(registry->provider_count(), 2);

    registry->clear();
    EXPECT_EQ(registry->provider_count(), 0);
    EXPECT_FALSE(registry->has_provider("sim0"));
    EXPECT_FALSE(registry->has_provider("sim1"));
}

// ============================================================================
// Lifetime and Ownership
// ============================================================================

TEST_F(ProviderRegistryTest, SharedPtrLifetime) {
    auto mock = create_mock_provider("sim0");
    std::weak_ptr<MockProviderHandle> weak_ref = mock;

    registry->add_provider("sim0", mock);
    mock.reset();  // Release original owner

    // Provider should still be alive (registry holds shared_ptr)
    EXPECT_FALSE(weak_ref.expired());

    auto retrieved = registry->get_provider("sim0");
    ASSERT_NE(retrieved, nullptr);

    // Remove from registry
    registry->remove_provider("sim0");
    retrieved.reset();  // Release last shared_ptr

    // Provider should now be destroyed
    EXPECT_TRUE(weak_ref.expired());
}

TEST_F(ProviderRegistryTest, GetProviderReturnsSnapshot) {
    auto mock = create_mock_provider("sim0");
    registry->add_provider("sim0", mock);

    auto ref1 = registry->get_provider("sim0");
    ASSERT_NE(ref1, nullptr);

    // Replace provider in registry
    auto mock2 = create_mock_provider("sim0");
    registry->add_provider("sim0", mock2);

    // Old reference still valid (points to old instance)
    EXPECT_EQ(ref1.get(), mock.get());

    // New get returns new instance
    auto ref2 = registry->get_provider("sim0");
    EXPECT_EQ(ref2.get(), mock2.get());
}

// ============================================================================
// Thread Safety - Concurrent Readers
// ============================================================================

TEST_F(ProviderRegistryTest, ConcurrentReaders) {
    const int NUM_READERS = 10;
    const int READS_PER_THREAD = 1000;

    // Setup: Add providers
    for (int i = 0; i < 5; ++i) {
        auto mock = create_mock_provider("provider" + std::to_string(i));
        EXPECT_CALL(*mock, is_available()).WillRepeatedly(Return(true));
        registry->add_provider("provider" + std::to_string(i), mock);
    }

    // Test: Multiple threads reading concurrently
    std::vector<std::thread> threads;
    threads.reserve(NUM_READERS);
    std::atomic<int> success_count{0};

    for (int t = 0; t < NUM_READERS; ++t) {
        threads.emplace_back([this, &success_count, reads_per_thread = READS_PER_THREAD]() {
            for (int i = 0; i < reads_per_thread; ++i) {
                // Read individual provider
                auto provider = registry->get_provider("provider0");
                if (provider && provider->is_available()) {
                    ++success_count;
                }

                // Read all providers
                auto all = registry->get_all_providers();
                if (all.size() == 5) {
                    ++success_count;
                }

                // Check existence
                if (registry->has_provider("provider1")) {
                    ++success_count;
                }

                // Get provider IDs
                auto ids = registry->get_provider_ids();
                if (ids.size() == 5) {
                    ++success_count;
                }
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    // All reads should succeed
    EXPECT_EQ(success_count.load(), NUM_READERS * READS_PER_THREAD * 4);
}

// ============================================================================
// Thread Safety - Concurrent Readers and Writers
// ============================================================================

TEST_F(ProviderRegistryTest, ConcurrentReadersAndWriters) {
    // Scale down for sanitizer builds (2-10x overhead)
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const int NUM_READERS = 3;
    const int NUM_WRITERS = 1;
    const int OPERATIONS_PER_THREAD = 100;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    const int NUM_READERS = 3;
    const int NUM_WRITERS = 1;
    const int OPERATIONS_PER_THREAD = 100;
#else
    const int NUM_READERS = 8;
    const int NUM_WRITERS = 2;
    const int OPERATIONS_PER_THREAD = 500;
#endif
#else
    const int NUM_READERS = 8;
    const int NUM_WRITERS = 2;
    const int OPERATIONS_PER_THREAD = 500;
#endif

    std::atomic<int> active_writers{NUM_WRITERS};
    std::atomic<int> completed_writer_operations{0};
    std::atomic<int> reader_iterations{0};
    std::vector<std::thread> threads;
    threads.reserve(NUM_WRITERS + NUM_READERS);

    // Writer threads: add/remove/replace providers
    for (int t = 0; t < NUM_WRITERS; ++t) {
        threads.emplace_back(
            [this, t, &active_writers, &completed_writer_operations, operations_per_thread = OPERATIONS_PER_THREAD]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    std::string provider_id = "writer" + std::to_string(t) + "_" + std::to_string(i % 10);
                    auto mock = create_mock_provider(provider_id);

                    // Add provider
                    registry->add_provider(provider_id, mock);

                    // Small delay to increase contention
                    std::this_thread::sleep_for(std::chrono::microseconds(10));

                    // Remove provider
                    registry->remove_provider(provider_id);
                    ++completed_writer_operations;
                }
                active_writers.fetch_sub(1, std::memory_order_release);
            });
    }

    // Reader threads: read while writers modify
    for (int t = 0; t < NUM_READERS; ++t) {
        threads.emplace_back([this, &active_writers, &reader_iterations]() {
            while (active_writers.load(std::memory_order_acquire) > 0) {
                // Various read operations - each is individually thread-safe
                auto count = registry->provider_count();
                auto all = registry->get_all_providers();
                auto ids = registry->get_provider_ids();

                // Try to get specific provider (may or may not exist)
                auto provider = registry->get_provider("writer0_5");

                static_cast<void>(count);
                static_cast<void>(all);
                static_cast<void>(ids);
                static_cast<void>(provider);
                ++reader_iterations;

                // Prevent shared-lock monopolization and reduce writer starvation
                // sensitivity from platform scheduler/stdlib behavior.
                std::this_thread::yield();
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_EQ(completed_writer_operations.load(), NUM_WRITERS * OPERATIONS_PER_THREAD);
    EXPECT_GT(reader_iterations.load(), 0);
}

// ============================================================================
// Thread Safety - Provider Restart Simulation
// ============================================================================

TEST_F(ProviderRegistryTest, ProviderRestartSimulation) {
    const int NUM_RESTARTS = 100;
    const int NUM_READERS = 4;

    std::atomic<bool> restart_complete{false};
    std::atomic<int> read_successes{0};

    // Restart thread: simulates Runtime::restart_provider()
    std::thread restart_thread([this, &restart_complete, num_restarts = NUM_RESTARTS]() {
        for (int i = 0; i < num_restarts; ++i) {
            auto old_mock = create_mock_provider("sim0");
            EXPECT_CALL(*old_mock, is_available()).WillRepeatedly(Return(false));
            registry->add_provider("sim0", old_mock);

            // Simulate crash detection delay
            std::this_thread::sleep_for(std::chrono::microseconds(50));

            // Remove crashed provider
            registry->remove_provider("sim0");

            // Simulate restart delay
            std::this_thread::sleep_for(std::chrono::microseconds(50));

            // Add new provider instance
            auto new_mock = create_mock_provider("sim0");
            EXPECT_CALL(*new_mock, is_available()).WillRepeatedly(Return(true));
            registry->add_provider("sim0", new_mock);
        }
        restart_complete = true;
    });

    // Reader threads: simulate StateCache polling during restarts
    std::vector<std::thread> reader_threads;
    reader_threads.reserve(NUM_READERS);
    for (int t = 0; t < NUM_READERS; ++t) {
        reader_threads.emplace_back([this, &restart_complete, &read_successes]() {
            while (!restart_complete.load()) {
                auto provider = registry->get_provider("sim0");
                if (provider) {
                    // Provider exists - check if available
                    // This should never crash, even during restart
                    bool available = provider->is_available();
                    if (available) {
                        ++read_successes;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    restart_thread.join();
    for (auto &thread : reader_threads) {
        thread.join();
    }

    // Verify final state
    EXPECT_TRUE(registry->has_provider("sim0"));
    auto final_provider = registry->get_provider("sim0");
    ASSERT_NE(final_provider, nullptr);
    EXPECT_TRUE(final_provider->is_available());

    // Primary goal: No crashes occurred during concurrent access
    // Secondary: Some reads may have succeeded (timing-dependent)
    // The test passes as long as thread safety is maintained
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(ProviderRegistryTest, GetAllProvidersReturnsSnapshot) {
    auto mock1 = create_mock_provider("sim0");
    registry->add_provider("sim0", mock1);

    // Get snapshot
    auto snapshot = registry->get_all_providers();
    EXPECT_EQ(snapshot.size(), 1);

    // Modify registry
    auto mock2 = create_mock_provider("sim1");
    registry->add_provider("sim1", mock2);
    registry->remove_provider("sim0");

    // Snapshot unchanged
    EXPECT_EQ(snapshot.size(), 1);
    EXPECT_NE(snapshot.find("sim0"), snapshot.end());

    // New snapshot reflects changes
    auto new_snapshot = registry->get_all_providers();
    EXPECT_EQ(new_snapshot.size(), 1);
    EXPECT_NE(new_snapshot.find("sim1"), new_snapshot.end());
}

TEST_F(ProviderRegistryTest, EmptyIdHandling) {
    auto mock = create_mock_provider("");
    registry->add_provider("", mock);

    EXPECT_TRUE(registry->has_provider(""));
    EXPECT_EQ(registry->provider_count(), 1);
    EXPECT_TRUE(registry->remove_provider(""));
    EXPECT_EQ(registry->provider_count(), 0);
}

TEST_F(ProviderRegistryTest, MultipleOperationsIdempotent) {
    auto mock = create_mock_provider("sim0");
    registry->add_provider("sim0", mock);

    // Multiple removes
    EXPECT_TRUE(registry->remove_provider("sim0"));
    EXPECT_FALSE(registry->remove_provider("sim0"));
    EXPECT_FALSE(registry->remove_provider("sim0"));

    // Multiple clears
    registry->clear();
    registry->clear();
    EXPECT_EQ(registry->provider_count(), 0);
}
