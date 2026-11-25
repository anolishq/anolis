#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// A directory of this run's own under TempDir(), or empty when none can be
// made. The pid alone is not unique: processes in separate PID namespaces
// share one, so the name adds the clock and create_directory, which reports
// a path that already exists, decides.
std::filesystem::path make_own_temp_dir() {
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    std::error_code error;
    const auto base = std::filesystem::absolute(::testing::TempDir(), error);
    if (error) {
        return {};
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto own = base / ("anolis-unit-" + std::to_string(pid) + "-" + std::to_string(stamp + attempt));
        if (std::filesystem::create_directory(own, error)) {
            return own;
        }
        if (error) {
            return {};
        }
    }
    return {};
}

}  // namespace

int main(int argc, char **argv) {
    // Explicitly initialize GoogleTest so --gtest_list_tests and filters work
    // reliably during CTest discovery.
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::InitGoogleMock(&argc, argv);

    // Tests build their rigs at fixed names under TempDir(), so two runs on
    // one machine shared them and read each other's files (#947). Each run
    // gets its own directory, which TempDir() returns from TEST_TMPDIR.
    const auto own = make_own_temp_dir();
    if (own.empty()) {
        std::cerr << "anolis_unit_tests: could not make a directory of this run's own under " << ::testing::TempDir()
                  << "; tests use it directly\n";
    } else {
#ifdef _WIN32
        _wputenv_s(L"TEST_TMPDIR", own.wstring().c_str());
#else
        setenv("TEST_TMPDIR", own.string().c_str(), 1);
#endif
    }

    const int result = RUN_ALL_TESTS();

    if (!own.empty() && own.is_absolute() && own.filename().string().starts_with("anolis-unit-")) {
        std::error_code ignored;
        std::filesystem::remove_all(own, ignored);
    }
    return result;
}
