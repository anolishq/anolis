#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "runs/run_types.hpp"

/**
 * @file run_journal.hpp
 * @brief Append-only JSONL run registry — the single writer/owner of run state.
 *
 * Durability: every state transition is appended to `<data_dir>/runs/index.jsonl`
 * and flushed before the call returns (run provenance must not lie about a
 * persisted run). A torn final line on crash is tolerated (skipped with a warn)
 * on the next startup, when the in-memory index is rebuilt from the log and any
 * still-open run is closed `abandoned`. SQLite is deliberately avoided — a
 * low-rate append-only audit log on a store-less runtime is served by a file.
 *
 * Concurrency: all public methods take one mutex, so concurrent HTTP requests
 * serialize on the journal. Writes happen on the calling (HTTP/runtime) thread,
 * never the automation tick thread.
 *
 * Cardinality: at most one run is open at a time (D8); open() rejects while a run
 * is open.
 */

namespace anolis {
namespace runs {

class RunJournal {
public:
    RunJournal(std::filesystem::path data_dir, std::string runtime_name, int polling_interval_ms);

    /// Rebuild the in-memory index from the log and recover (close-abandoned) any
    /// run left open by a prior process. Creates the data dir if needed.
    bool initialize(std::string& error);

    struct OpenResult {
        bool ok = false;
        std::string error;
        Run run;
    };
    /// Open a run. Rejected (ok=false) if a run is already open. `version` is the
    /// automation version captured at open (nullopt => manual-only run).
    OpenResult open(const RunOpenSpec& spec, std::optional<AutomationVersionRecord> version);

    struct CloseResult {
        bool ok = false;
        std::string error;
        std::optional<Run> run;
    };
    /// Close a run. Idempotent: closing an already-closed run returns ok with the
    /// existing record. Unknown id => ok=false.
    CloseResult close(const std::string& run_id, CloseReason reason);

    std::optional<Run> get(const std::string& run_id) const;

    /// Newest-first page of runs.
    std::vector<Run> list(size_t limit, size_t offset) const;

    /// The currently-open run id, if any.
    std::optional<std::string> open_run_id() const;

private:
    std::string mint_run_id();                               // requires mutex_ held
    bool append_record(const Run& run, std::string& error);  // sync write + flush; requires mutex_ held

    mutable std::mutex mutex_;
    std::filesystem::path data_dir_;
    std::filesystem::path index_path_;
    std::string runtime_name_;
    int polling_interval_ms_;

    std::unordered_map<std::string, Run> runs_;  // run_id -> latest record
    std::vector<std::string> order_;             // run_ids in open order (for newest-first)
    std::optional<std::string> open_run_id_;
    uint64_t mint_counter_ = 0;  // disambiguates ids minted within the same ms
};

}  // namespace runs
}  // namespace anolis
