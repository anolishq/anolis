#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

int main(int argc, char **argv) {
    // Explicitly initialize GoogleTest so --gtest_list_tests and filters work
    // reliably during CTest discovery.
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::InitGoogleMock(&argc, argv);

    // Tests build their rigs at fixed names under TempDir(), so two runs on
    // one machine shared them and read each other's files (#947). Each run
    // gets its own directory, which TempDir() returns from TEST_TMPDIR.
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    const auto own = std::filesystem::path(::testing::TempDir()) / ("anolis-unit-" + std::to_string(pid));
    std::filesystem::create_directories(own);
#ifdef _WIN32
    _putenv_s("TEST_TMPDIR", own.string().c_str());
#else
    setenv("TEST_TMPDIR", own.string().c_str(), 1);
#endif

    const int result = RUN_ALL_TESTS();

    if (own.is_absolute() && own.filename().string().starts_with("anolis-unit-")) {
        std::error_code ignored;
        std::filesystem::remove_all(own, ignored);
    }
    return result;
}
