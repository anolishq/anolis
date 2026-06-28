#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

/**
 * @file run_types.hpp
 * @brief Run / experiment identity value types (#7 / epic anolishq/anolis#111, Phase 2).
 *
 * A "run" is an explicit operator/experiment primitive, decoupled from AUTO mode
 * (a run may span MANUAL phases). At most one run is open per runtime instance
 * (D8). A run records the immutable automation version it executed (nullable for
 * a manual-only run) plus build + timing provenance, so telemetry can later be
 * correlated to it by a time-window join — `run_id` is never a telemetry tag.
 */

namespace anolis {
namespace runs {

enum class RunState { Open, Closed };

/// Why a run ended. `aborted`-style provenance lives here, not in engine status.
enum class CloseReason {
    OperatorStop,  ///< Explicit operator close.
    Completed,     ///< Engine reached a terminal completed state.
    Failed,        ///< Engine reached a terminal failed state.
    Superseded,    ///< A digest-changing reload opened a successor.
    Abandoned      ///< Runtime restarted while the run was still open.
};

const char* to_string(RunState state);
const char* to_string(CloseReason reason);
std::optional<CloseReason> close_reason_from_string(const std::string& s);

/// Operator-supplied selector for which series a run "owns" (auto-capture is
/// deferred). An omitted/empty scope means "not scoped".
struct TagScope {
    std::vector<std::string> provider_ids;
    std::vector<std::string> device_ids;
    std::vector<std::string> signal_ids;
};

/// Decoupled mirror of automation::AutomationVersion (the run subsystem must not
/// depend on the automation layer).
struct AutomationVersionRecord {
    std::string engine_kind;
    std::string id;
    std::string digest;
    std::string digest_scope;
};

/// A run record — append-only and immutable once closed.
struct Run {
    std::string run_id;
    std::optional<std::string> experiment_label;
    RunState state = RunState::Open;
    std::optional<CloseReason> close_reason;
    uint64_t started_at_epoch_ms = 0;
    std::optional<uint64_t> ended_at_epoch_ms;
    std::optional<AutomationVersionRecord> automation_version;  ///< null for a manual-only run.
    std::string runtime_version;                                ///< ANOLIS_VERSION (+ build id later).
    int polling_interval_ms = 0;  ///< Recorded so a consumer can reason about boundary fuzz.
    nlohmann::json params = nlohmann::json::object();
    TagScope tag_scope;
    uint32_t schema_version = 1;  ///< Per-record durable schema version.
};

/// Operator-supplied inputs at run-open.
struct RunOpenSpec {
    std::optional<std::string> experiment_label;
    nlohmann::json params = nlohmann::json::object();
    TagScope tag_scope;
};

nlohmann::json to_json(const TagScope& scope);
nlohmann::json to_json(const Run& run);
/// Parse a durable record (index-rebuild path). Returns nullopt on a malformed line.
std::optional<Run> run_from_json(const nlohmann::json& j);

}  // namespace runs
}  // namespace anolis
