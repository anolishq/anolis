#include "provider/provider_handle.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

namespace {

const char *kHelloMismatchProviderPath = PROVIDER_HELLO_MISMATCH_HELPER_PATH;
const char *kStalledReadProviderPath = PROVIDER_STALLED_READ_HELPER_PATH;
const char *kEmptyInventoryProviderPath = PROVIDER_EMPTY_INVENTORY_HELPER_PATH;

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

// #556: a startup budget is shared by Hello and WaitReady. Each answers in
// 700 ms, well inside its own 2000 ms timeout even on a slow process start, so
// start() accepts the provider; but together they take 1400 ms, so a 1000 ms
// budget refuses it at the budget.
namespace {

anolis::provider::ProviderHandle slow_startup_handle() {
    return anolis::provider::ProviderHandle("slow-startup", kEmptyInventoryProviderPath,
                                            {"--hello-delay-ms", "700", "--ready-delay-ms", "700"}, 1000, 2000, 2000,
                                            500);
}

}  // namespace

TEST(ProviderHandleTest, StartGivesHelloAndWaitReadyEachTheirOwnTimeout) {
    auto handle = slow_startup_handle();
    EXPECT_TRUE(handle.start()) << handle.last_error();
    EXPECT_TRUE(handle.is_available());
    EXPECT_TRUE(handle.stop());
}

TEST(ProviderHandleTest, StartWithinRefusesWhenHelloAndWaitReadyTogetherOutlastTheBudget) {
    auto handle = slow_startup_handle();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(handle.start_within(1000ms));
    const auto waited = elapsed_since(start);
    EXPECT_GE(waited, 1000ms);
    EXPECT_LT(waited, 2000ms) << "WaitReady waited its own timeout, not what Hello left of the budget";
    EXPECT_FALSE(handle.last_error().empty());
    EXPECT_FALSE(handle.is_available());
    EXPECT_TRUE(handle.stop()) << "the refused provider was not reaped";
}

TEST(ProviderHandleTest, StartWithinAcceptsWhenHelloAndWaitReadyFitTheBudget) {
    auto handle = slow_startup_handle();
    EXPECT_TRUE(handle.start_within(5000ms)) << handle.last_error();
    EXPECT_TRUE(handle.is_available());
    EXPECT_TRUE(handle.stop());
}

TEST(ProviderHandleTest, StartWithinCutsHelloToTheBudget) {
    anolis::provider::ProviderHandle handle("slow-hello", kEmptyInventoryProviderPath, {"--hello-delay-ms", "2500"},
                                            1000, 4000, 1000, 500);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(handle.start_within(1000ms));
    const auto waited = elapsed_since(start);
    EXPECT_GE(waited, 1000ms);
    EXPECT_LT(waited, 2500ms) << "Hello waited its own timeout, not the budget";
    EXPECT_FALSE(handle.is_available());
    EXPECT_TRUE(handle.stop()) << "the refused provider was not reaped";
}

// The provider is told how long it has: WaitReady's max_wait_ms_hint is the
// wait the runtime will give it, its own ready timeout at boot and what is left
// of the budget on a restart.
namespace {

int wait_ready_hint(bool budgeted) {
    const auto hint_file = std::filesystem::path(::testing::TempDir()) /
                           (std::string("anolis-556-hint-") + (budgeted ? "budgeted" : "boot"));
    std::filesystem::remove(hint_file);
    anolis::provider::ProviderHandle handle("hint", kEmptyInventoryProviderPath,
                                            {"--hello-delay-ms", "300", "--hint-to", hint_file.string()}, 1000, 3000,
                                            5000, 500);
    EXPECT_TRUE(budgeted ? handle.start_within(4000ms) : handle.start()) << handle.last_error();
    handle.stop();
    int hint = -1;
    std::ifstream(hint_file) >> hint;
    std::filesystem::remove(hint_file);
    return hint;
}

}  // namespace

TEST(ProviderHandleTest, WaitReadyHintIsTheReadyTimeoutWithoutABudget) { EXPECT_EQ(wait_ready_hint(false), 5000); }

TEST(ProviderHandleTest, WaitReadyHintIsWhatHelloLeftOfTheBudget) {
    const int hint = wait_ready_hint(true);
    EXPECT_GT(hint, 0);
    EXPECT_LE(hint, 3700) << "Hello took 300 ms of the 4000 ms budget";
}
