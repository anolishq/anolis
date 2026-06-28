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

/// Closed neutral lifecycle taxonomy for the per-run event stream. Domain
/// vocabulary (inoculation/feed/sample/…) is deliberately kept OUT of this enum —
/// operator markers ride as `Annotation` with an open `type` + `payload` — so the
/// core never bakes in bioprocess terms (the leak the BT vocabulary was).
enum class RunEventCategory {
    RunOpened,        ///< Run opened (the first event in a stream).
    RunClosed,        ///< Run closed (the last event; `type` carries the close reason).
    ModeChange,       ///< Runtime mode transition folded into the stream.
    ParameterChange,  ///< Parameter update folded into the stream.
    AutomationFault,  ///< Engine-abnormal fault (`type` carries the locus). Not unsuccessful execution.
    Annotation        ///< Operator-supplied domain marker (open `type` + `payload`).
};

const char* to_string(RunEventCategory category);
std::optional<RunEventCategory> run_event_category_from_string(const std::string& s);

/// A durable, append-only run event. The durable identity is `run_id + sequence`
/// (a per-run monotonic counter) — NOT the live EventEmitter's process-local ids,
/// which reset on restart. `occurred_at` is when the source observed it;
/// `recorded_at` is when the journal persisted it.
struct RunEvent {
    std::string run_id;
    uint64_t sequence = 0;
    RunEventCategory category = RunEventCategory::Annotation;
    std::string type;  ///< Open string: marker type, close reason, or fault locus; may be empty.
    uint64_t occurred_at_epoch_ms = 0;
    uint64_t recorded_at_epoch_ms = 0;
    nlohmann::json payload = nlohmann::json::object();
    uint32_t schema_version = 1;
};

nlohmann::json to_json(const TagScope& scope);
nlohmann::json to_json(const Run& run);
nlohmann::json to_json(const RunEvent& event);
/// Parse a durable record (index-rebuild path). Returns nullopt on a malformed line.
std::optional<Run> run_from_json(const nlohmann::json& j);
std::optional<RunEvent> run_event_from_json(const nlohmann::json& j);

}  // namespace runs
}  // namespace anolis
