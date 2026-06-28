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

nlohmann::json to_json(const TagScope& scope) {
    return {{"provider_ids", scope.provider_ids}, {"device_ids", scope.device_ids}, {"signal_ids", scope.signal_ids}};
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
            LOG_INFO("[RunJournal] Recovered open run " << id << " as abandoned");
        }
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
