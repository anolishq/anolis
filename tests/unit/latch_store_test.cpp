// anolishq/anolis#482: the software stop's latch on disk.

#include "control/latch_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using anolis::control::LatchStore;
using anolis::control::PersistedLatch;

namespace {

std::filesystem::path fresh_dir(const std::string &name) {
    const auto dir = std::filesystem::path(::testing::TempDir()) / name;
    std::filesystem::remove_all(dir);
    return dir;
}

}  // namespace

TEST(LatchStoreTest, NoFileMeansUnlatched) {
    LatchStore store(LatchStore::path_for(fresh_dir("latch-absent"), "rig"));
    EXPECT_EQ(store.load().state, PersistedLatch::State::Absent);
}

TEST(LatchStoreTest, ASavedLatchReadsBackWithItsTime) {
    LatchStore store(LatchStore::path_for(fresh_dir("latch-saved"), "rig"));
    std::string error;
    ASSERT_TRUE(store.save(1700000000123, "operator", error)) << error;

    const auto loaded = store.load();
    EXPECT_EQ(loaded.state, PersistedLatch::State::Latched);
    EXPECT_EQ(loaded.latched_at_epoch_ms, 1700000000123);
    EXPECT_FALSE(std::filesystem::exists(store.path().string() + ".tmp"));
}

TEST(LatchStoreTest, RemoveLeavesNothingAndIsIdempotent) {
    LatchStore store(LatchStore::path_for(fresh_dir("latch-removed"), "rig"));
    std::string error;
    EXPECT_TRUE(store.remove(error)) << error;  // nothing there yet, not even the directory
    ASSERT_TRUE(store.save(1, "operator", error)) << error;
    ASSERT_TRUE(store.remove(error)) << error;
    EXPECT_FALSE(std::filesystem::exists(store.path()));
    EXPECT_EQ(store.load().state, PersistedLatch::State::Absent);
}

TEST(LatchStoreTest, ACorruptFileFailsTowardHeld) {
    for (const std::string body : {"not json", "{}", "{\"latched\": false}", "[]", ""}) {
        SCOPED_TRACE(body);
        LatchStore store(LatchStore::path_for(fresh_dir("latch-corrupt"), "rig"));
        std::filesystem::create_directories(store.path().parent_path());
        std::ofstream(store.path()) << body;

        const auto loaded = store.load();
        EXPECT_EQ(loaded.state, PersistedLatch::State::Unreadable);
        EXPECT_NE(loaded.detail.find("not a valid latch record"), std::string::npos) << loaded.detail;
    }
}

TEST(LatchStoreTest, TwoConfigsSharingADataDirAreIndependent) {
    const auto dir = fresh_dir("latch-shared");
    LatchStore a(LatchStore::path_for(dir, "project-a"));
    LatchStore b(LatchStore::path_for(dir, "project-b"));
    std::string error;
    ASSERT_TRUE(a.save(1, "operator", error)) << error;

    EXPECT_NE(a.path(), b.path());
    EXPECT_EQ(a.load().state, PersistedLatch::State::Latched);
    EXPECT_EQ(b.load().state, PersistedLatch::State::Absent);
}

TEST(LatchStoreTest, TheNameStaysInsideTheEstopDirectory) {
    const std::filesystem::path dir = "data";
    EXPECT_EQ(LatchStore::path_for(dir, ""), dir / "estop" / "default.json");
    EXPECT_EQ(LatchStore::path_for(dir, "bioreactor-v1.b3"), dir / "estop" / "bioreactor-v1.b3.json");
    EXPECT_EQ(LatchStore::path_for(dir, "../etc/passwd"), dir / "estop" / ".._etc_passwd.json");
    EXPECT_EQ(LatchStore::path_for(dir, ".."), dir / "estop" / "_.json");
}
