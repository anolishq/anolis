#include "runs/run_journal.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <random>

#include "anolis_build_config.hpp"
#include "logging/logger.hpp"

namespace anolis {
namespace runs {

namespace {

uint64_t now_epoch_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Crockford base32 for a compact, time-sortable, collision-safe id (ULID-style:
// 48-bit ms timestamp + 80 bits of entropy).
std::string make_ulid(uint64_t ts_ms, uint64_t counter) {
    static const char* kEnc = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::array<uint8_t, 16> bytes{};
    // 48-bit timestamp (big-endian) in the first 6 bytes.
    for (int i = 5; i >= 0; --i) {
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(ts_ms & 0xFF);
        ts_ms >>= 8;
    }
    // 80 bits of entropy, mixed with a per-ms counter so ids minted in the same
    // millisecond never collide.
    uint64_t r1 = rng() ^ (counter * 0x9E3779B97F4A7C15ULL);
    uint64_t r2 = rng();
    for (int i = 0; i < 5; ++i) {
        bytes[6 + static_cast<size_t>(i)] = static_cast<uint8_t>((r1 >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 5; ++i) {
        bytes[11 + static_cast<size_t>(i)] = static_cast<uint8_t>((r2 >> (i * 8)) & 0xFF);
    }
    // Encode 16 bytes (128 bits) as 26 base32 chars (130 bits, top 2 padding).
    std::string out(26, '0');
    uint32_t bit_buffer = 0;
    int bits = 0;
    size_t oi = 0;
    for (uint8_t b : bytes) {
        bit_buffer = (bit_buffer << 8) | b;
        bits += 8;
        while (bits >= 5 && oi < out.size()) {
            bits -= 5;
            out[oi++] = kEnc[(bit_buffer >> bits) & 0x1F];
        }
    }
    if (bits > 0 && oi < out.size()) {
        out[oi++] = kEnc[(bit_buffer << (5 - bits)) & 0x1F];
    }
    return out;
}

std::vector<std::string> to_str_vec(const nlohmann::json& arr) {
    std::vector<std::string> out;
    if (arr.is_array()) {
        for (const auto& v : arr) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    }
    return out;
}

}  // namespace

const char* to_string(RunState state) {
    switch (state) {
        case RunState::Open:
            return "open";
        case RunState::Closed:
            return "closed";
    }
    return "open";
}

const char* to_string(CloseReason reason) {
    switch (reason) {
        case CloseReason::OperatorStop:
            return "operator_stop";
        case CloseReason::Completed:
            return "completed";
        case CloseReason::Failed:
            return "failed";
        case CloseReason::Superseded:
            return "superseded";
        case CloseReason::Abandoned:
            return "abandoned";
    }
    return "operator_stop";
}

std::optional<CloseReason> close_reason_from_string(const std::string& s) {
    if (s == "operator_stop") return CloseReason::OperatorStop;
    if (s == "completed") return CloseReason::Completed;
    if (s == "failed") return CloseReason::Failed;
    if (s == "superseded") return CloseReason::Superseded;
    if (s == "abandoned") return CloseReason::Abandoned;
    return std::nullopt;
}

const char* to_string(RunEventCategory category) {
    switch (category) {
        case RunEventCategory::RunOpened:
            return "run_opened";
        case RunEventCategory::RunClosed:
            return "run_closed";
        case RunEventCategory::ModeChange:
            return "mode_change";
        case RunEventCategory::ParameterChange:
            return "parameter_change";
        case RunEventCategory::AutomationFault:
            return "automation_fault";
        case RunEventCategory::Annotation:
            return "annotation";
    }
    return "annotation";
}

std::optional<RunEventCategory> run_event_category_from_string(const std::string& s) {
    if (s == "run_opened") return RunEventCategory::RunOpened;
    if (s == "run_closed") return RunEventCategory::RunClosed;
    if (s == "mode_change") return RunEventCategory::ModeChange;
    if (s == "parameter_change") return RunEventCategory::ParameterChange;
    if (s == "automation_fault") return RunEventCategory::AutomationFault;
    if (s == "annotation") return RunEventCategory::Annotation;
    return std::nullopt;
}

nlohmann::json to_json(const TagScope& scope) {
    return {{"provider_ids", scope.provider_ids}, {"device_ids", scope.device_ids}, {"signal_ids", scope.signal_ids}};
}

nlohmann::json to_json(const RunEvent& event) {
    return {{"schema_version", event.schema_version},
            {"run_id", event.run_id},
            {"sequence", event.sequence},
            {"category", to_string(event.category)},
            {"type", event.type},
            {"occurred_at_epoch_ms", event.occurred_at_epoch_ms},
            {"recorded_at_epoch_ms", event.recorded_at_epoch_ms},
            {"payload", event.payload}};
}

std::optional<RunEvent> run_event_from_json(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("run_id") || !j["run_id"].is_string()) {
        return std::nullopt;
    }
    auto category = run_event_category_from_string(j.value("category", std::string("annotation")));
    if (!category) return std::nullopt;
    RunEvent event;
    event.schema_version = j.value("schema_version", 1U);
    event.run_id = j["run_id"].get<std::string>();
    event.sequence = j.value("sequence", 0ULL);
    event.category = *category;
    event.type = j.value("type", std::string());
    event.occurred_at_epoch_ms = j.value("occurred_at_epoch_ms", 0ULL);
    event.recorded_at_epoch_ms = j.value("recorded_at_epoch_ms", 0ULL);
    if (j.contains("payload") && j["payload"].is_object()) event.payload = j["payload"];
    return event;
}

nlohmann::json to_json(const Run& run) {
    nlohmann::json j = {{"schema_version", run.schema_version},
                        {"run_id", run.run_id},
                        {"state", to_string(run.state)},
                        {"started_at_epoch_ms", run.started_at_epoch_ms},
                        {"runtime_version", run.runtime_version},
                        {"polling_interval_ms", run.polling_interval_ms},
                        {"params", run.params},
                        {"tag_scope", to_json(run.tag_scope)}};
    j["experiment_label"] = run.experiment_label.has_value() ? nlohmann::json(*run.experiment_label) : nlohmann::json();
    j["close_reason"] = run.close_reason.has_value() ? nlohmann::json(to_string(*run.close_reason)) : nlohmann::json();
    j["ended_at_epoch_ms"] =
        run.ended_at_epoch_ms.has_value() ? nlohmann::json(*run.ended_at_epoch_ms) : nlohmann::json();
    if (run.automation_version.has_value()) {
        const auto& v = *run.automation_version;
        j["automation_version"] = {
            {"engine_kind", v.engine_kind}, {"id", v.id}, {"digest", v.digest}, {"digest_scope", v.digest_scope}};
    } else {
        j["automation_version"] = nlohmann::json();
    }
    return j;
}

std::optional<Run> run_from_json(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("run_id") || !j["run_id"].is_string()) {
        return std::nullopt;
    }
    Run run;
    run.schema_version = j.value("schema_version", 1U);
    run.run_id = j["run_id"].get<std::string>();
    run.state = (j.value("state", std::string("open")) == "closed") ? RunState::Closed : RunState::Open;
    run.started_at_epoch_ms = j.value("started_at_epoch_ms", 0ULL);
    run.runtime_version = j.value("runtime_version", std::string());
    run.polling_interval_ms = j.value("polling_interval_ms", 0);
    if (j.contains("params") && j["params"].is_object()) run.params = j["params"];
    if (j.contains("experiment_label") && j["experiment_label"].is_string())
        run.experiment_label = j["experiment_label"].get<std::string>();
    if (j.contains("close_reason") && j["close_reason"].is_string())
        run.close_reason = close_reason_from_string(j["close_reason"].get<std::string>());
    if (j.contains("ended_at_epoch_ms") && j["ended_at_epoch_ms"].is_number_unsigned())
        run.ended_at_epoch_ms = j["ended_at_epoch_ms"].get<uint64_t>();
    if (j.contains("tag_scope") && j["tag_scope"].is_object()) {
        run.tag_scope.provider_ids = to_str_vec(j["tag_scope"].value("provider_ids", nlohmann::json::array()));
        run.tag_scope.device_ids = to_str_vec(j["tag_scope"].value("device_ids", nlohmann::json::array()));
        run.tag_scope.signal_ids = to_str_vec(j["tag_scope"].value("signal_ids", nlohmann::json::array()));
    }
    if (j.contains("automation_version") && j["automation_version"].is_object()) {
        const auto& v = j["automation_version"];
        AutomationVersionRecord rec;
        rec.engine_kind = v.value("engine_kind", std::string());
        rec.id = v.value("id", std::string());
        rec.digest = v.value("digest", std::string());
        rec.digest_scope = v.value("digest_scope", std::string());
        run.automation_version = rec;
    }
    return run;
}

RunJournal::RunJournal(std::filesystem::path data_dir, std::string runtime_name, int polling_interval_ms)
    : data_dir_(std::move(data_dir)),
      runtime_name_(std::move(runtime_name)),
      polling_interval_ms_(polling_interval_ms) {
    index_path_ = data_dir_ / "runs" / "index.jsonl";
}

RunJournal::~RunJournal() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        writer_stop_ = true;
    }
    queue_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

bool RunJournal::initialize(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(index_path_.parent_path(), ec);
    if (ec) {
        error = "failed to create run data dir " + index_path_.parent_path().string() + ": " + ec.message();
        return false;
    }

    // Rebuild the in-memory index from the log; later records supersede earlier.
    std::ifstream in(index_path_);
    if (in.good()) {
        std::string line;
        size_t skipped = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::optional<Run> run;
            try {
                run = run_from_json(nlohmann::json::parse(line));
            } catch (const std::exception&) {
                run = std::nullopt;  // torn or malformed line: skip
            }
            if (!run) {
                ++skipped;
                continue;
            }
            if (runs_.find(run->run_id) == runs_.end()) {
                order_.push_back(run->run_id);
            }
            runs_[run->run_id] = *run;
        }
        if (skipped > 0) {
            LOG_WARN("[RunJournal] Skipped " << skipped << " malformed run-log line(s) during recovery");
        }
    }

    // Recover: close any run left open by a prior process as abandoned.
    open_run_id_.reset();
    for (const auto& id : order_) {
        Run& run = runs_[id];
        if (run.state == RunState::Open) {
            run.state = RunState::Closed;
            run.close_reason = CloseReason::Abandoned;
            run.ended_at_epoch_ms = now_epoch_ms();
            std::string werr;
            if (!append_record(run, werr)) {
                error = "failed to persist abandoned-run recovery: " + werr;
                return false;
            }
            // A recovery marker closes the event stream too (sequence restored from
            // the existing per-run events file by append_event_locked).
            auto ev = append_event_locked(id, RunEventCategory::RunClosed, "abandoned",
                                          nlohmann::json{{"recovery", true}}, *run.ended_at_epoch_ms);
            if (!ev.ok) {
                LOG_WARN("[RunJournal] Failed to record abandoned run_closed event: " << ev.error);
            }
            next_seq_.erase(id);
            LOG_INFO("[RunJournal] Recovered open run " << id << " as abandoned");
        }
    }

    // Start the background fault writer (idempotent: initialize runs once).
    if (!writer_thread_.joinable()) {
        writer_stop_ = false;
        writer_thread_ = std::thread([this] { writer_loop(); });
    }

    LOG_INFO("[RunJournal] Initialized (" << runs_.size() << " run(s) known) at " << index_path_.string());
    return true;
}

std::string RunJournal::mint_run_id() { return runtime_name_ + "-" + make_ulid(now_epoch_ms(), mint_counter_++); }

bool RunJournal::append_record(const Run& run, std::string& error) {
    std::ofstream out(index_path_, std::ios::app);
    if (!out.good()) {
        error = "cannot open run log for append: " + index_path_.string();
        return false;
    }
    out << to_json(run).dump() << "\n";
    out.flush();
    if (!out.good()) {
        error = "failed to write run record to " + index_path_.string();
        return false;
    }
    return true;
}

std::filesystem::path RunJournal::events_path(const std::string& run_id) const {
    return index_path_.parent_path() / (run_id + ".events.jsonl");
}

RunJournal::AppendEventResult RunJournal::append_event_locked(const std::string& run_id, RunEventCategory category,
                                                              const std::string& type, const nlohmann::json& payload,
                                                              uint64_t occurred_at_epoch_ms) {
    AppendEventResult result;

    // Resolve the per-run sequence. If unknown (e.g. abandoned-run recovery after a
    // restart), restore it from the existing stream so sequences stay monotonic.
    uint64_t seq = 0;
    auto seq_it = next_seq_.find(run_id);
    if (seq_it != next_seq_.end()) {
        seq = seq_it->second;
    } else {
        uint64_t max_seq = 0;
        std::ifstream existing(events_path(run_id));
        std::string line;
        while (std::getline(existing, line)) {
            if (line.empty()) continue;
            try {
                auto parsed = run_event_from_json(nlohmann::json::parse(line));
                if (parsed && parsed->sequence > max_seq) max_seq = parsed->sequence;
            } catch (const std::exception&) {
                continue;  // torn/malformed line: skip it in the max-sequence scan
            }
        }
        seq = max_seq + 1;
    }

    RunEvent event;
    event.run_id = run_id;
    event.sequence = seq;
    event.category = category;
    event.type = type;
    event.occurred_at_epoch_ms = occurred_at_epoch_ms != 0 ? occurred_at_epoch_ms : now_epoch_ms();
    event.recorded_at_epoch_ms = now_epoch_ms();
    event.payload = payload.is_object() ? payload : nlohmann::json::object();

    std::ofstream out(events_path(run_id), std::ios::app);
    if (!out.good()) {
        result.error = "cannot open run event log for append: " + events_path(run_id).string();
        return result;
    }
    out << to_json(event).dump() << "\n";
    out.flush();
    if (!out.good()) {
        result.error = "failed to write run event to " + events_path(run_id).string();
        return result;
    }

    next_seq_[run_id] = seq + 1;
    result.ok = true;
    result.event = std::move(event);
    return result;
}

RunJournal::AppendEventResult RunJournal::append_event(const std::string& run_id, RunEventCategory category,
                                                       const std::string& type, const nlohmann::json& payload,
                                                       uint64_t occurred_at_epoch_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    AppendEventResult result;
    auto it = runs_.find(run_id);
    if (it == runs_.end()) {
        result.error = "unknown run: " + run_id;
        return result;
    }
    if (it->second.state == RunState::Closed) {
        result.error = "run is closed (events are immutable): " + run_id;
        return result;
    }
    return append_event_locked(run_id, category, type, payload, occurred_at_epoch_ms);
}

void RunJournal::enqueue_fault(const std::string& locus, const std::string& message, uint64_t occurred_at_epoch_ms) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (fault_queue_.size() >= kMaxFaultQueue) {
            events_dropped_.fetch_add(1);
            return;
        }
        fault_queue_.push_back({locus, message, occurred_at_epoch_ms});
    }
    queue_cv_.notify_one();
}

void RunJournal::writer_loop() {
    for (;;) {
        QueuedFault item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return writer_stop_ || !fault_queue_.empty(); });
            if (fault_queue_.empty()) {
                if (writer_stop_) return;
                continue;
            }
            item = std::move(fault_queue_.front());
            fault_queue_.pop_front();
        }
        // Persist off the queue lock, against whichever run is open now.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_run_id_.has_value()) {
            events_dropped_.fetch_add(1);  // no run to attribute the fault to
            continue;
        }
        auto res = append_event_locked(*open_run_id_, RunEventCategory::AutomationFault, item.locus,
                                       nlohmann::json{{"message", item.message}}, item.occurred_at_epoch_ms);
        if (!res.ok) {
            LOG_WARN("[RunJournal] Failed to persist automation_fault: " << res.error);
        }
    }
}

std::vector<RunEvent> RunJournal::list_events(const std::string& run_id, size_t limit, size_t offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RunEvent> out;
    std::ifstream in(events_path(run_id));
    if (!in.good()) return out;  // no events file yet => empty page
    std::string line;
    size_t index = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::optional<RunEvent> event;
        try {
            event = run_event_from_json(nlohmann::json::parse(line));
        } catch (const std::exception&) {
            event = std::nullopt;  // torn/malformed line: skip
        }
        if (!event) continue;
        if (index++ < offset) continue;
        if (out.size() >= limit) break;
        out.push_back(*event);
    }
    return out;
}

RunJournal::OpenResult RunJournal::open(const RunOpenSpec& spec, std::optional<AutomationVersionRecord> version) {
    std::lock_guard<std::mutex> lock(mutex_);
    OpenResult result;
    if (open_run_id_.has_value()) {
        result.error = "a run is already open: " + *open_run_id_ + " (one open run per runtime)";
        return result;
    }

    Run run;
    run.run_id = mint_run_id();
    run.experiment_label = spec.experiment_label;
    run.state = RunState::Open;
    run.started_at_epoch_ms = now_epoch_ms();
    run.automation_version = std::move(version);
    run.runtime_version = ANOLIS_VERSION;
    run.polling_interval_ms = polling_interval_ms_;
    run.params = spec.params;
    run.tag_scope = spec.tag_scope;

    std::string werr;
    if (!append_record(run, werr)) {  // durably flushed before we report success
        result.error = werr;
        return result;
    }

    runs_[run.run_id] = run;
    order_.push_back(run.run_id);
    open_run_id_ = run.run_id;

    // run_opened is the first event in the stream (sequence 1).
    auto ev = append_event_locked(run.run_id, RunEventCategory::RunOpened, "", nlohmann::json::object(),
                                  run.started_at_epoch_ms);
    if (!ev.ok) {
        LOG_WARN("[RunJournal] Failed to record run_opened event: " << ev.error);
    }

    result.ok = true;
    result.run = std::move(run);
    return result;
}

RunJournal::CloseResult RunJournal::close(const std::string& run_id, CloseReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseResult result;
    auto it = runs_.find(run_id);
    if (it == runs_.end()) {
        result.error = "unknown run: " + run_id;
        return result;
    }
    if (it->second.state == RunState::Closed) {
        result.ok = true;  // idempotent
        result.run = it->second;
        return result;
    }

    Run updated = it->second;
    updated.state = RunState::Closed;
    updated.close_reason = reason;
    updated.ended_at_epoch_ms = now_epoch_ms();

    std::string werr;
    if (!append_record(updated, werr)) {
        result.error = werr;
        return result;
    }

    it->second = updated;

    // run_closed is the final event in the stream; the close reason rides as `type`.
    auto ev = append_event_locked(run_id, RunEventCategory::RunClosed, to_string(reason), nlohmann::json::object(),
                                  updated.ended_at_epoch_ms.value_or(now_epoch_ms()));
    if (!ev.ok) {
        LOG_WARN("[RunJournal] Failed to record run_closed event: " << ev.error);
    }
    next_seq_.erase(run_id);  // closed runs are immutable — no more events

    if (open_run_id_ == run_id) {
        open_run_id_.reset();
    }
    result.ok = true;
    result.run = std::move(updated);
    return result;
}

std::optional<Run> RunJournal::get(const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) return std::nullopt;
    return it->second;
}

std::vector<Run> RunJournal::list(size_t limit, size_t offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Run> out;
    // Newest-first.
    if (offset >= order_.size()) return out;
    size_t start = order_.size() - offset;  // exclusive index from the end
    for (size_t i = start; i-- > 0 && out.size() < limit;) {
        auto it = runs_.find(order_[i]);
        if (it != runs_.end()) out.push_back(it->second);
    }
    return out;
}

std::optional<std::string> RunJournal::open_run_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_run_id_;
}

}  // namespace runs
}  // namespace anolis
