#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "mocks/mock_provider_handle.hpp"
#include "registry/device_registry.hpp"

using namespace anolis;
using namespace testing;
using namespace anolis::tests;

class DeviceRegistryConcurrencyTest : public Test {
protected:
    void SetUp() override { registry = std::make_unique<registry::DeviceRegistry>(); }

    // Helper: Create a mock provider that returns 3 test devices
    std::shared_ptr<MockProviderHandle> create_mock_provider_with_devices(const std::string &provider_id) {
        auto mock = std::make_shared<StrictMock<MockProviderHandle>>();
        mock->_id = provider_id;
        EXPECT_CALL(*mock, provider_id()).WillRepeatedly(ReturnRef(mock->_id));

        // Setup Hello response (no-op, signature changed)
        EXPECT_CALL(*mock, hello(_)).WillRepeatedly(Return(true));

        // Setup ListDevices response
        EXPECT_CALL(*mock, list_devices(_)).WillRepeatedly(Invoke([](std::vector<Device> &devices) {
            Device dev0, dev1, dev2;
            dev0.set_device_id("device0");
            dev0.set_label("Test Device 0");
            dev1.set_device_id("device1");
            dev1.set_label("Test Device 1");
            dev2.set_device_id("device2");
            dev2.set_label("Test Device 2");
            devices.push_back(dev0);
            devices.push_back(dev1);
            devices.push_back(dev2);
            return true;
        }));

        // Setup DescribeDevice responses for each device
        EXPECT_CALL(*mock, describe_device(_, _))
            .WillRepeatedly([](const std::string &device_id, DescribeDeviceResponse &response) {
                auto *device = response.mutable_device();
                device->set_device_id(device_id);
                device->set_label("Test Device " + device_id);

                // Add capabilities
                auto *caps = response.mutable_capabilities();

                // Add one signal
                auto *signal = caps->add_signals();
                signal->set_signal_id("temp");
                signal->set_value_type(deviceprovider::v1::VALUE_TYPE_DOUBLE);
                signal->set_poll_hint_hz(1.0);

                // Add one function
                auto *function = caps->add_functions();
                function->set_function_id(1);
                function->set_name("reset");

                return true;
            });

        return mock;
    }

    // Helper: Populate registry with test devices
    void populate_registry(const std::string &provider_id) {
        auto mock = create_mock_provider_with_devices(provider_id);
        size_t count_before = registry->device_count();
        ASSERT_TRUE(registry->discover_provider(provider_id, *mock));
        EXPECT_EQ(registry->device_count(), count_before + 3);  // Verify 3 devices added
    }

    std::unique_ptr<registry::DeviceRegistry> registry;
};

// ============================================================================
// Replacement Semantics Tests (used by two-phase restart flow)
// ============================================================================

TEST_F(DeviceRegistryConcurrencyTest, DiscoverProviderWithReplaceExistingReplacesProviderInventory) {
    populate_registry("sim0");
    ASSERT_EQ(registry->device_count(), 3u);

    auto mock = std::make_shared<StrictMock<MockProviderHandle>>();
    EXPECT_CALL(*mock, list_devices(_)).WillOnce(Invoke([](std::vector<Device> &devices) {
        Device dev;
        dev.set_device_id("replacement0");
        dev.set_label("Replacement Device");
        devices.push_back(dev);
        return true;
    }));
    EXPECT_CALL(*mock, describe_device("replacement0", _))
        .WillOnce(Invoke([](const std::string &, DescribeDeviceResponse &response) {
            auto *device = response.mutable_device();
            device->set_device_id("replacement0");
            device->set_label("Replacement Device");

            auto *caps = response.mutable_capabilities();
            auto *signal = caps->add_signals();
            signal->set_signal_id("temp");
            signal->set_value_type(deviceprovider::v1::VALUE_TYPE_DOUBLE);
            signal->set_poll_hint_hz(1.0);

            auto *function = caps->add_functions();
            function->set_function_id(1);
            function->set_name("reset");
            return true;
        }));

    ASSERT_TRUE(registry->discover_provider("sim0", *mock, true));
    EXPECT_EQ(registry->device_count(), 1u);
    EXPECT_FALSE(registry->get_device_copy("sim0", "device0").has_value());

    auto replacement = registry->get_device_copy("sim0", "replacement0");
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(replacement->provider_id, "sim0");
}

TEST_F(DeviceRegistryConcurrencyTest, DiscoverProviderReplaceFailureKeepsPreviousInventory) {
    populate_registry("sim0");
    ASSERT_EQ(registry->device_count(), 3u);
    ASSERT_TRUE(registry->get_device_copy("sim0", "device0").has_value());

    auto mock = std::make_shared<StrictMock<MockProviderHandle>>();
    mock->_err = "describe failed";

    EXPECT_CALL(*mock, list_devices(_)).WillOnce(Invoke([](std::vector<Device> &devices) {
        Device dev;
        dev.set_device_id("replacement0");
        devices.push_back(dev);
        return true;
    }));
    EXPECT_CALL(*mock, describe_device("replacement0", _)).WillOnce(Return(false));
    EXPECT_CALL(*mock, last_error()).WillOnce(ReturnPointee(&mock->_err));

    ASSERT_FALSE(registry->discover_provider("sim0", *mock, true));

    // Replacement discovery failure must not mutate existing inventory.
    EXPECT_EQ(registry->device_count(), 3u);
    EXPECT_TRUE(registry->get_device_copy("sim0", "device0").has_value());
    EXPECT_FALSE(registry->get_device_copy("sim0", "replacement0").has_value());
}

// ============================================================================
// Concurrency Tests
// ============================================================================

// Test: Clear provider devices while concurrent threads call get_device_copy()
// Ensures: No dangling pointers, no crashes, no data races
TEST_F(DeviceRegistryConcurrencyTest, ClearWhileConcurrentReads) {
    // Populate registry
    populate_registry("sim0");

    // Start 50 reader threads
    std::atomic<bool> stop_reading{false};
    std::atomic<size_t> successful_reads{0};
    std::atomic<size_t> device_not_found{0};
    std::vector<std::thread> reader_threads;
    reader_threads.reserve(50);

    for (int i = 0; i < 50; ++i) {
        reader_threads.emplace_back([&]() {
            while (!stop_reading.load()) {
                auto device = registry->get_device_copy("sim0", "device1");
                if (device.has_value()) {
                    ++successful_reads;
                    // Verify device copy is valid
                    EXPECT_EQ(device->provider_id, "sim0");
                    EXPECT_EQ(device->device_id, "device1");
                } else {
                    ++device_not_found;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Wait for readers to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Clear devices while readers are active
    registry->clear_provider_devices("sim0");
    EXPECT_EQ(registry->device_count(), 0);

    // Stop readers
    stop_reading.store(true);
    for (auto &t : reader_threads) {
        t.join();
    }

    // Both outcomes are valid:
    // - Found device before clear
    // - Did not find device after clear
    EXPECT_GT(successful_reads.load() + device_not_found.load(), 0);
    std::cout << "Reads: " << successful_reads.load() << " successful, " << device_not_found.load() << " not found\n";
}

// Test: Restart simulation - repeatedly clear and re-discover while readers active
// Ensures: No crashes during repeated mutations
TEST_F(DeviceRegistryConcurrencyTest, RestartSimulationStressTest) {
    // Initial population
    populate_registry("sim0");

    // Scale for sanitizer builds and CI environments
    // Sanitizers add 2-10x overhead, CI has limited resources
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    constexpr int num_reader_threads = 10;
    constexpr int num_restarts = 3;
#else
    constexpr int num_reader_threads = 30;
    constexpr int num_restarts = 5;
#endif
#else
    constexpr int num_reader_threads = 10;
    constexpr int num_restarts = 3;
#endif
#else
    constexpr int num_reader_threads = 30;
    constexpr int num_restarts = 5;
#endif

    std::atomic<bool> stop_test{false};
    std::atomic<size_t> successful_reads{0};
    std::atomic<size_t> failed_reads{0};
    std::vector<std::thread> reader_threads;
    reader_threads.reserve(num_reader_threads);

    for (int i = 0; i < num_reader_threads; ++i) {
        reader_threads.emplace_back([&]() {
            while (!stop_test.load()) {
                auto device = registry->get_device_copy("sim0", "device0");
                if (device.has_value()) {
                    ++successful_reads;
                    // Validate device contents
                    EXPECT_EQ(device->provider_id, "sim0");
                    EXPECT_EQ(device->device_id, "device0");
                    EXPECT_FALSE(device->capabilities.signals_by_id.empty());
                } else {
                    ++failed_reads;
                }
                // Small yield to reduce contention in sanitizer builds
                std::this_thread::yield();
            }
        });
    }

    // Simulate rapid provider restarts
    auto mock = create_mock_provider_with_devices("sim0");
    for (int restart = 0; restart < num_restarts; ++restart) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Clear devices (simulates provider disconnect)
        registry->clear_provider_devices("sim0");

        // Re-discover (simulates provider reconnect)
        ASSERT_TRUE(registry->discover_provider("sim0", *mock));
    }

    // Stop readers
    stop_test.store(true);
    for (auto &t : reader_threads) {
        t.join();
    }

    EXPECT_GT(successful_reads.load(), 0);
    std::cout << "Restarts: " << num_restarts << "\n";
    std::cout << "Threads: " << num_reader_threads << "\n";
    std::cout << "Reads: " << successful_reads.load() << " successful, " << failed_reads.load() << " failed\n";
}

// Test: Multiple providers with concurrent access
// Ensures: Clearing one provider doesn't affect others
TEST_F(DeviceRegistryConcurrencyTest, MultiProviderIsolation) {
    // Populate two providers
    populate_registry("sim0");
    populate_registry("sim1");
    EXPECT_EQ(registry->device_count(), 6);

    // Read from sim1 while clearing sim0
    std::atomic<bool> stop_reading{false};
    std::atomic<size_t> sim1_reads{0};
    std::thread reader([&]() {
        while (!stop_reading.load()) {
            auto device = registry->get_device_copy("sim1", "device0");
            if (device.has_value()) {
                ++sim1_reads;
                EXPECT_EQ(device->provider_id, "sim1");
            }
        }
    });

    // Wait for reader to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Clear sim0 (should not affect sim1)
    registry->clear_provider_devices("sim0");
    EXPECT_EQ(registry->device_count(), 3);

    // Verify sim1 devices still accessible
    auto device = registry->get_device_copy("sim1", "device0");
    ASSERT_TRUE(device.has_value());
    EXPECT_EQ(device->provider_id, "sim1");

    stop_reading.store(true);
    reader.join();

    EXPECT_GT(sim1_reads.load(), 0);
}

// Test: get_device_by_handle_copy concurrency
// Ensures: Handle-based lookup is thread-safe
TEST_F(DeviceRegistryConcurrencyTest, HandleLookupConcurrency) {
    populate_registry("sim0");

    std::atomic<bool> stop_reading{false};
    std::atomic<size_t> handle_reads{0};
    std::vector<std::thread> threads;
    threads.reserve(30);

    // 30 threads reading by handle
    for (int i = 0; i < 30; ++i) {
        threads.emplace_back([&]() {
            while (!stop_reading.load()) {
                auto device = registry->get_device_by_handle_copy("sim0/device1");
                if (device.has_value()) {
                    ++handle_reads;
                    EXPECT_EQ(device->get_handle(), "sim0/device1");
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Run for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_reading.store(true);

    for (auto &t : threads) {
        t.join();
    }

    EXPECT_GT(handle_reads.load(), 0);
    std::cout << "Handle reads: " << handle_reads.load() << "\n";
}

// Test: get_all_devices concurrency
// Ensures: Snapshot returns don't cause data races
TEST_F(DeviceRegistryConcurrencyTest, GetAllDevicesConcurrency) {
    populate_registry("sim0");

    std::atomic<bool> stop_test{false};
    std::atomic<size_t> snapshot_count{0};
    std::vector<std::thread> threads;
    threads.reserve(20);

    // 20 threads calling get_all_devices repeatedly
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&]() {
            while (!stop_test.load()) {
                auto all_devices = registry->get_all_devices();
                if (all_devices.size() == 3) {
                    ++snapshot_count;
                    // Verify device contents
                    for (const auto &device : all_devices) {
                        EXPECT_EQ(device.provider_id, "sim0");
                        EXPECT_FALSE(device.capabilities.signals_by_id.empty());
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Clear and re-discover while snapshots being taken
    auto mock = create_mock_provider_with_devices("sim0");
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        registry->clear_provider_devices("sim0");
        registry->discover_provider("sim0", *mock);
    }

    stop_test.store(true);
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_GT(snapshot_count.load(), 0);
    std::cout << "Snapshots taken: " << snapshot_count.load() << "\n";
}

// Test: Stress test with mixed operations
// Ensures: No deadlocks or crashes under heavy load
TEST_F(DeviceRegistryConcurrencyTest, MixedOperationsStress) {
    populate_registry("sim0");

    // Scale down for sanitizer builds and CI environments
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    constexpr int threads_per_operation = 3;
    constexpr int test_duration_ms = 150;
#else
    constexpr int threads_per_operation = 10;
    constexpr int test_duration_ms = 300;
#endif
#else
    constexpr int threads_per_operation = 3;
    constexpr int test_duration_ms = 150;
#endif
#else
    constexpr int threads_per_operation = 10;
    constexpr int test_duration_ms = 300;
#endif

    std::atomic<bool> stop_test{false};
    std::atomic<size_t> get_device_calls{0};
    std::atomic<size_t> get_all_calls{0};
    std::atomic<size_t> get_by_handle_calls{0};
    std::vector<std::thread> threads;
    threads.reserve(3 * threads_per_operation + 1);

    auto mock = create_mock_provider_with_devices("sim0");

    // threads_per_operation threads: get_device_copy
    for (int i = 0; i < threads_per_operation; ++i) {
        threads.emplace_back([&]() {
            while (!stop_test.load()) {
                auto device = registry->get_device_copy("sim0", "device0");
                if (device.has_value()) {
                    ++get_device_calls;
                }
                std::this_thread::yield();  // Reduce contention
            }
        });
    }

    // threads_per_operation threads: get_all_devices
    for (int i = 0; i < threads_per_operation; ++i) {
        threads.emplace_back([&]() {
            while (!stop_test.load()) {
                auto all = registry->get_all_devices();
                if (!all.empty()) {
                    ++get_all_calls;
                }
                std::this_thread::yield();  // Reduce contention
            }
        });
    }

    // threads_per_operation threads: get_device_by_handle_copy
    for (int i = 0; i < threads_per_operation; ++i) {
        threads.emplace_back([&]() {
            while (!stop_test.load()) {
                auto device = registry->get_device_by_handle_copy("sim0/device1");
                if (device.has_value()) {
                    ++get_by_handle_calls;
                }
                std::this_thread::yield();  // Reduce contention
            }
        });
    }

    // 1 writer thread: clear + discover repeatedly
    threads.emplace_back([&]() {
        for (int i = 0; i < 10 && !stop_test.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            registry->clear_provider_devices("sim0");
            registry->discover_provider("sim0", *mock);
        }
    });

    // Run stress test
    std::this_thread::sleep_for(std::chrono::milliseconds(test_duration_ms));
    stop_test.store(true);

    for (auto &t : threads) {
        t.join();
    }

    // Verify operations occurred
    EXPECT_GT(get_device_calls.load(), 0);
    EXPECT_GT(get_all_calls.load(), 0);
    // Note: get_by_handle_calls may be 0 due to lock contention in stress scenarios
    // Dedicated test HandleLookupConcurrency validates this method works

    std::cout << "Stress test results (threads_per_op=" << threads_per_operation << "):\n";
    std::cout << "  get_device_copy: " << get_device_calls.load() << "\n";
    std::cout << "  get_all_devices: " << get_all_calls.load() << "\n";
    std::cout << "  get_by_handle_copy: " << get_by_handle_calls.load() << "\n";
}
