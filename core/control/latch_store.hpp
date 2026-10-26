#pragma once

/**
 * @file latch_store.hpp
 * @brief The software stop's latch on disk, so a restart comes back latched (#482).
 *
 * One file per runtime config: `<data_dir>/estop/<runtime.name>.json`. The
 * workbench starts every project it launches with the same working directory,
 * so a file shared between configs would let one project's latch hold another.
 *
 * The file exists exactly while the latch is engaged. It is written with
 * write-temp, fsync and rename, then the directory is synced, and removed on a
 * clear. Reading fails toward held: a file that is present but cannot be read
 * or parsed means latched.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace anolis {
namespace control {

/** @brief What a startup read of the latch file found. */
struct PersistedLatch {
    enum class State {
        Absent,      // no file: start unlatched
        Latched,     // a well-formed file: start latched
        Unreadable,  // present but unreadable or corrupt: start latched
    };
    State state = State::Absent;
    std::optional<int64_t> latched_at_epoch_ms;  // from a well-formed file
    std::string detail;                          // why, for the log and the status surface
};

class LatchStore {
public:
    explicit LatchStore(std::filesystem::path file);

    /** @brief `<data_dir>/estop/<name>.json`, `name` made filename-safe ("default" if empty). */
    static std::filesystem::path path_for(const std::filesystem::path &data_dir, const std::string &runtime_name);

    const std::filesystem::path &path() const { return file_; }

    PersistedLatch load() const;

    /** @brief Write the latch durably. On failure `error` says why and no file is left half-written. */
    bool save(int64_t latched_at_epoch_ms, const std::string &reason, std::string &error) const;

    /** @brief Remove the file. A missing file is success. */
    bool remove(std::string &error) const;

private:
    std::filesystem::path file_;
};

}  // namespace control
}  // namespace anolis
