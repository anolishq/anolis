#include "provider/provider_handle.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

const char *kHelloMismatchProviderPath = PROVIDER_HELLO_MISMATCH_HELPER_PATH;

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
