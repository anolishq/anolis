#include "control/reissue_worker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using anolis::control::ReissueWorker;

namespace {

constexpr auto kDeadline = std::chrono::seconds(5);

// A worker that never runs would hang the test in stop(); report it and exit
// instead of wedging the run.
void wait_or_exit(std::future<void> &f, const char *what) {
    if (f.wait_for(kDeadline) == std::future_status::timeout) {
        std::cout << "HANG: " << what << " within " << kDeadline.count() << " s" << std::endl;
        std::fflush(nullptr);
        std::_Exit(3);
    }
}

// Records each re-assert; the first one for `block_on` waits until opened.
class Recorder {
public:
    explicit Recorder(std::string block_on = "") : block_on_(std::move(block_on)) {}

    void operator()(const std::string &handle) {
        std::unique_lock<std::mutex> lock(mutex_);
        ran_.push_back(handle);
        threads_.push_back(std::this_thread::get_id());
        if (handle == block_on_ && !blocked_once_) {
            blocked_once_ = true;
            entered_.set_value();
            // Bounded: if the re-assert ran on the posting thread, nothing
            // would ever open this.
            if (!opened_cv_.wait_for(lock, kDeadline, [this] { return opened_; })) {
                std::cout << "HANG: a blocked re-assert was never released (did it run on the posting thread?)"
                          << std::endl;
                std::fflush(nullptr);
                std::_Exit(3);
            }
        }
        ran_cv_.notify_all();
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        opened_ = true;
        opened_cv_.notify_all();
    }

    std::future<void> entered() { return entered_.get_future(); }

    // Wait until `n` re-asserts have run.
    void wait_for_runs(size_t n) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ran_cv_.wait_for(lock, kDeadline, [&] { return ran_.size() >= n; })) {
            std::cout << "HANG: " << n << " re-asserts did not run; ran " << ran_.size() << std::endl;
            std::fflush(nullptr);
            std::_Exit(3);
        }
    }

    std::vector<std::string> ran() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ran_;
    }
    std::vector<std::thread::id> threads() {
        std::lock_guard<std::mutex> lock(mutex_);
        return threads_;
    }

private:
    std::string block_on_;
    std::mutex mutex_;
    std::condition_variable opened_cv_;
    std::condition_variable ran_cv_;
    bool opened_ = false;
    bool blocked_once_ = false;
    std::promise<void> entered_;
    std::vector<std::string> ran_;
    std::vector<std::thread::id> threads_;
};

}  // namespace

// The point of the worker: post() never runs the re-assert on its caller's
// thread, which may hold a provider lock.
TEST(ReissueWorkerTest, RunsOffThePostingThread) {
    Recorder rec;
    ReissueWorker worker([&rec](const std::string &h) { rec(h); });
    worker.post("sim0/dev1");
    rec.wait_for_runs(1);
    ASSERT_EQ(rec.threads().size(), 1U);
    EXPECT_NE(rec.threads()[0], std::this_thread::get_id());
}

// A handle already waiting is covered by the run it is waiting for.
TEST(ReissueWorkerTest, CoalescesAHandleThatIsAlreadyWaiting) {
    Recorder rec("sim0/dev1");
    ReissueWorker worker([&rec](const std::string &h) { rec(h); });
    auto entered = rec.entered();
    worker.post("sim0/dev1");
    wait_or_exit(entered, "the first re-assert did not start");

    worker.post("sim0/dev2");
    worker.post("sim0/dev2");
    rec.open();
    worker.stop();

    EXPECT_EQ(rec.ran(), (std::vector<std::string>{"sim0/dev1", "sim0/dev2"}));
}

// A device that returns again while its re-assert is running gets another
// one: the running one may already be past the call that mattered.
TEST(ReissueWorkerTest, PostDuringItsOwnRunQueuesItAgain) {
    Recorder rec("sim0/dev1");
    ReissueWorker worker([&rec](const std::string &h) { rec(h); });
    auto entered = rec.entered();
    worker.post("sim0/dev1");
    wait_or_exit(entered, "the first re-assert did not start");

    worker.post("sim0/dev1");
    rec.open();
    rec.wait_for_runs(2);
    EXPECT_EQ(rec.ran(), (std::vector<std::string>{"sim0/dev1", "sim0/dev1"}));
}

// stop() runs what is still queued before it returns; a re-assert accepted by
// post() is never silently lost.
TEST(ReissueWorkerTest, StopRunsWhatIsQueued) {
    Recorder rec("sim0/dev1");
    ReissueWorker worker([&rec](const std::string &h) { rec(h); });
    auto entered = rec.entered();
    worker.post("sim0/dev1");
    wait_or_exit(entered, "the first re-assert did not start");
    worker.post("sim0/dev2");
    worker.post("sim0/dev3");

    // Stop while dev2 and dev3 are queued. A post that warns proves stop()
    // has taken effect before the blocked re-assert is released.
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    std::thread stopper([&worker] { worker.stop(); });
    const auto give_up = std::chrono::steady_clock::now() + kDeadline;
    while (captured.str().find("during shutdown") == std::string::npos) {
        if (std::chrono::steady_clock::now() > give_up) {
            std::cerr.rdbuf(old);
            std::cout << "HANG: stop() never took effect" << std::endl;
            std::_Exit(3);
        }
        worker.post("sim0/probe");
        std::this_thread::yield();
    }
    rec.open();
    stopper.join();
    std::cerr.rdbuf(old);

    const auto ran = rec.ran();
    EXPECT_NE(std::find(ran.begin(), ran.end(), "sim0/dev2"), ran.end());
    EXPECT_NE(std::find(ran.begin(), ran.end(), "sim0/dev3"), ran.end());
}

// After stop() nothing runs, and in particular not on the caller's thread.
TEST(ReissueWorkerTest, PostAfterStopIsDroppedWithAWarning) {
    Recorder rec;
    ReissueWorker worker([&rec](const std::string &h) { rec(h); });
    worker.stop();

    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    worker.post("sim0/dev1");
    std::cerr.rdbuf(old);

    EXPECT_TRUE(rec.ran().empty());
    EXPECT_NE(captured.str().find("sim0/dev1 returned while latched during shutdown"), std::string::npos)
        << captured.str();
}

// A re-assert that throws is logged; the worker keeps serving.
TEST(ReissueWorkerTest, AThrowingReassertDoesNotStopTheWorker) {
    Recorder rec;
    ReissueWorker worker([&rec](const std::string &h) {
        if (h == "sim0/dev1") {
            throw std::runtime_error("provider gone");
        }
        rec(h);
    });
    std::ostringstream captured;
    std::streambuf *old = std::cerr.rdbuf(captured.rdbuf());
    worker.post("sim0/dev1");
    worker.post("sim0/dev2");
    rec.wait_for_runs(1);
    worker.stop();
    std::cerr.rdbuf(old);

    EXPECT_EQ(rec.ran(), (std::vector<std::string>{"sim0/dev2"}));
    EXPECT_NE(captured.str().find("sim0/dev1 threw: provider gone"), std::string::npos) << captured.str();
}
