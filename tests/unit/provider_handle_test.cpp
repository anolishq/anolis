#include "provider/provider_handle.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

namespace {

const char *kHelloMismatchProviderPath = PROVIDER_HELLO_MISMATCH_HELPER_PATH;
const char *kStalledReadProviderPath = PROVIDER_STALLED_READ_HELPER_PATH;

using namespace std::chrono_literals;
namespace adpp = anolis::deviceprovider::v1;

// Normal operation timeout for the stalled-read provider: the read that never
// answers holds the session this long.
constexpr int kStalledTimeoutMs = 1500;

std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

// Starts a ReadSignals that the provider never answers, and returns once it
// has had time to take the session.
std::future<bool> start_stuck_read(anolis::provider::ProviderHandle &handle) {
    auto stuck = std::async(std::launch::async, [&handle] {
        adpp::ReadSignalsResponse response;
        adpp::Status_Code status{};
        return handle.read_signals("dev0", {}, response, status);
    });
    std::this_thread::sleep_for(200ms);
    return stuck;
}

}  // namespace

TEST(ProviderHandleTest, StartFailsOnHelloProtocolVersionMismatch) {
    const std::filesystem::path helper_path(kHelloMismatchProviderPath);
    ASSERT_TRUE(std::filesystem::exists(helper_path)) << "Missing helper executable: " << helper_path.string();

    anolis::provider::ProviderHandle handle("proto-mismatch", helper_path.string(), {}, 1000, 1000, 1000, 1000);

    EXPECT_FALSE(handle.start());
    EXPECT_NE(handle.last_error().find("Protocol version mismatch"), std::string::npos)
        << "Unexpected error: " << handle.last_error();
    EXPECT_FALSE(handle.is_available());
}

TEST(ProviderHandleTest, TryGetHealthAnswersOnAnIdleSession) {
    anolis::provider::ProviderHandle handle("stalled", kStalledReadProviderPath, {}, kStalledTimeoutMs, 1000, 1000,
                                            1000);
    ASSERT_TRUE(handle.start()) << handle.last_error();
    adpp::GetHealthResponse response;
    EXPECT_TRUE(handle.try_get_health(response, 250ms)) << handle.last_error();
    handle.stop();
}

// #411: behind a call that holds the session, try_get_health gives up after
// its wait instead of the call's timeout.
TEST(ProviderHandleTest, TryGetHealthGivesUpOnABusySession) {
    anolis::provider::ProviderHandle handle("stalled", kStalledReadProviderPath, {}, kStalledTimeoutMs, 1000, 1000,
                                            1000);
    ASSERT_TRUE(handle.start()) << handle.last_error();
    auto stuck = start_stuck_read(handle);

    const auto start = std::chrono::steady_clock::now();
    adpp::GetHealthResponse response;
    EXPECT_FALSE(handle.try_get_health(response, 250ms));
    const auto waited = elapsed_since(start);
    EXPECT_GE(waited, 250ms);
    EXPECT_LT(waited, 1000ms) << "waited out the stuck read";

    EXPECT_FALSE(stuck.get());
    handle.stop();
}

// The plain get_health still queues behind the stuck read, which is what the
// startup watchdog preflight relies on.
TEST(ProviderHandleTest, GetHealthWaitsForABusySession) {
    anolis::provider::ProviderHandle handle("stalled", kStalledReadProviderPath, {}, kStalledTimeoutMs, 1000, 1000,
                                            1000);
    ASSERT_TRUE(handle.start()) << handle.last_error();
    auto stuck = start_stuck_read(handle);

    const auto start = std::chrono::steady_clock::now();
    adpp::GetHealthResponse response;
    handle.get_health(response);
    EXPECT_GE(elapsed_since(start), 1000ms) << "did not wait for the stuck read";

    EXPECT_FALSE(stuck.get());
    handle.stop();
}
