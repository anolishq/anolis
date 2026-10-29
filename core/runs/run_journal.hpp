#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
 * Concurrency: all run/event writes take one mutex, so concurrent HTTP requests
 * serialize on the journal. Run open/close and operator markers write on the
 * calling (HTTP/runtime) thread; engine faults and mode changes from the
 * automation tick thread go through a non-blocking bounded queue drained in FIFO
 * order by a single background writer, so the tick thread never fsyncs and a
 * fault is journaled before the FAULT transition it caused.
 *
 * Cardinality: at most one run is open at a time (D8); open() rejects while a run
 * is open.
 */

namespace anolis {
namespace runs {

class RunJournal {
public:
    /// Flushes a file, or with `directory` set a directory, to storage; true on
    /// success.
    using SyncFn = std::function<bool(const std::filesystem::path& path, bool directory)>;

    /// `sync` stands in for the platform sync (fsync, or _commit on Windows), for
    /// tests; left empty, the platform sync is used.
    RunJournal(std::filesystem::path data_dir, std::string runtime_name, int polling_interval_ms, SyncFn sync = {});
    ~RunJournal();

    RunJournal(const RunJournal&) = delete;
    RunJournal& operator=(const RunJournal&) = delete;

    /// Rebuild the in-memory index from the log and recover (close-abandoned) any
    /// run left open by a prior process. Creates the data dir if needed. Starts the
    /// background event writer.
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

    struct AppendEventResult {
        bool ok = false;
        std::string error;
        RunEvent event;
    };
    /// Synchronously append an event to a run's stream (flushed before return).
    /// Rejected for an unknown or already-closed run (closed runs are immutable).
    /// Used by HTTP/runtime-thread sources (operator markers, mode/parameter
    /// changes). NEVER call from the automation tick thread — use enqueue_fault,
    /// or record_mode_change with from_tick_thread set.
    AppendEventResult append_event(const std::string& run_id, RunEventCategory category, const std::string& type,
                                   const nlohmann::json& payload, uint64_t occurred_at_epoch_ms);

    /// Non-blocking enqueue of an engine fault from the tick thread. The fault is
    /// journaled off-thread by the background writer against whichever run is open
    /// when it drains; if none is open it is dropped (it still rode the SSE stream)
    /// and counted. Performs no file I/O on the caller's thread.
    void enqueue_fault(const std::string& locus, const std::string& message, uint64_t occurred_at_epoch_ms);

    /// Journal a mode change into the open run, if any. From the automation tick
    /// thread (`from_tick_thread`) it rides the async fault queue, so it lands
    /// after any fault the tick enqueued first and does no file I/O on that
    /// thread. From any other thread it is written synchronously.
    void record_mode_change(const std::string& previous_mode, const std::string& new_mode, bool from_tick_thread);

    /// Oldest-first (ascending sequence) page of a run's events.
    std::vector<RunEvent> list_events(const std::string& run_id, size_t limit, size_t offset) const;

    /// Count of queued events dropped because the async queue was full, plus
    /// faults that drained with no run open.
    uint64_t events_dropped() const { return events_dropped_.load(); }

private:
    std::string mint_run_id();                               // requires mutex_ held
    bool append_record(const Run& run, std::string& error);  // sync write + flush; requires mutex_ held
    // Build + append an event to <run_id>.events.jsonl, assigning sequence and
    // recorded_at. Requires mutex_ held. On success returns the persisted event.
    AppendEventResult append_event_locked(const std::string& run_id, RunEventCategory category, const std::string& type,
                                          const nlohmann::json& payload, uint64_t occurred_at_epoch_ms);
    std::filesystem::path events_path(const std::string& run_id) const;
    void writer_loop();  // background fault-drain thread

    mutable std::mutex mutex_;
    std::filesystem::path data_dir_;
    std::filesystem::path index_path_;
    std::string runtime_name_;
    int polling_interval_ms_;
    SyncFn sync_;

    std::unordered_map<std::string, Run> runs_;  // run_id -> latest record
    std::vector<std::string> order_;             // run_ids in open order (for newest-first)
    std::optional<std::string> open_run_id_;
    uint64_t mint_counter_ = 0;                           // disambiguates ids minted within the same ms
    std::unordered_map<std::string, uint64_t> next_seq_;  // open run_id -> next event sequence

    // Bounded async queue for tick-thread events. A separate mutex (never held
    // during file I/O) keeps enqueueing from ever blocking on a journal flush.
    struct QueuedEvent {
        RunEventCategory category = RunEventCategory::AutomationFault;
        std::string type;
        nlohmann::json payload;
        uint64_t occurred_at_epoch_ms = 0;
    };
    void enqueue_event(QueuedEvent event);
    static constexpr size_t kMaxFaultQueue = 512;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedEvent> fault_queue_;
    std::thread writer_thread_;
    bool writer_stop_ = false;
    std::atomic<uint64_t> events_dropped_{0};
};

}  // namespace runs
}  // namespace anolis
