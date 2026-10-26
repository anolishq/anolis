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

void write_raw(const LatchStore &store, const std::string &body) {
    std::filesystem::create_directories(store.path().parent_path());
    std::ofstream(store.path()) << body;
}

}  // namespace

TEST(LatchStoreTest, NoFileMeansUnlatched) {
    LatchStore store(fresh_dir("latch-absent"), "/srv/rig/config");
    EXPECT_EQ(store.load().state, PersistedLatch::State::Absent);
}

TEST(LatchStoreTest, ASavedLatchReadsBackWithItsTime) {
    LatchStore store(fresh_dir("latch-saved"), "/srv/rig/config");
    std::string error;
    ASSERT_TRUE(store.save(1700000000123, "operator", error)) << error;

    const auto loaded = store.load();
    EXPECT_EQ(loaded.state, PersistedLatch::State::Latched);
    EXPECT_EQ(loaded.latched_at_epoch_ms, 1700000000123);
    EXPECT_FALSE(std::filesystem::exists(store.path().string() + ".tmp"));
}

TEST(LatchStoreTest, RemoveLeavesNothingAndIsIdempotent) {
    LatchStore store(fresh_dir("latch-removed"), "/srv/rig/config");
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
        LatchStore store(fresh_dir("latch-corrupt"), "/srv/rig/config");
        write_raw(store, body);

        const auto loaded = store.load();
        EXPECT_EQ(loaded.state, PersistedLatch::State::Unreadable);
        EXPECT_NE(loaded.detail.find("not a valid latch record"), std::string::npos) << loaded.detail;
    }
}

TEST(LatchStoreTest, AFileNamingAnotherConfigDirectoryFailsTowardHeld) {
    // A hash collision, or a file copied between machines: never silently ours.
    LatchStore store(fresh_dir("latch-other-dir"), "/srv/rig/config");
    write_raw(store, R"({"schema_version": 1, "latched": true, "latched_at_epoch_ms": 5, "config_dir": "/elsewhere"})");

    const auto loaded = store.load();
    EXPECT_EQ(loaded.state, PersistedLatch::State::Unreadable);
    EXPECT_NE(loaded.detail.find("does not name this config directory"), std::string::npos) << loaded.detail;
}

TEST(LatchStoreTest, TwoConfigDirectoriesSharingADataDirAreIndependent) {
    const auto dir = fresh_dir("latch-shared");
    LatchStore a(dir, "/work/projects/a/config");
    LatchStore b(dir, "/work/projects/b/config");
    std::string error;
    ASSERT_TRUE(a.save(1, "operator", error)) << error;

    EXPECT_NE(a.path(), b.path());
    EXPECT_EQ(a.load().state, PersistedLatch::State::Latched);
    EXPECT_EQ(b.load().state, PersistedLatch::State::Absent);
}

TEST(LatchStoreTest, TheFileNameIsAStableHashOfTheConfigDirectory) {
    // Pinned: a build that hashed differently would not find the latch an
    // older one left behind.
    EXPECT_EQ(LatchStore::path_for("data", "/opt/anolis/config"),
              std::filesystem::path("data") / "estop" / "6480874c9c5e048b.json");
}
