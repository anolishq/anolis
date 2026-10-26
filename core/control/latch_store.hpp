#pragma once

/**
 * @file latch_store.hpp
 * @brief The software stop's latch on disk, so a restart comes back latched (#482).
 *
 * One file per machine: `<data_dir>/estop/<hash of the config directory>.json`.
 * A project's variants share their config/ directory, separate projects have
 * separate ones, and an install has one. The workbench starts every project it
 * launches with the same working directory, and its template gives every
 * project the same runtime.name, so neither could key it. The directory is
 * stored in the file; a file that names another directory counts as corrupt.
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
    /** @brief The store for the runtime whose config file lives in `config_dir`. */
    LatchStore(const std::filesystem::path &data_dir, std::string config_dir);

    /** @brief `<data_dir>/estop/<16 hex digits of FNV-1a over config_dir>.json`. */
    static std::filesystem::path path_for(const std::filesystem::path &data_dir, const std::string &config_dir);

    const std::filesystem::path &path() const { return file_; }

    PersistedLatch load() const;

    /** @brief Write the latch durably. On failure `error` says why and no file is left half-written. */
    bool save(int64_t latched_at_epoch_ms, const std::string &reason, std::string &error) const;

    /** @brief Remove the file. A missing file is success. */
    bool remove(std::string &error) const;

private:
    std::filesystem::path file_;
    std::string config_dir_;
};

}  // namespace control
}  // namespace anolis
