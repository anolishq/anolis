#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file automation_engine.hpp
 * @brief Neutral automation-engine seam (#6 / epic anolishq/anolis#111, Phase 0).
 *
 * `IAutomationEngine` is the engine-agnostic interface the runtime talks to.
 * Behavior trees are the first (and today only) implementation; the interface
 * deliberately exposes NO BehaviorTree.CPP vocabulary (no `BT::NodeStatus`, no
 * tree/node/XML concepts), so a future state-machine / recipe / schedule engine
 * can drop in behind it. This is an INTERNAL C++ seam — no other repo links
 * anolis C++; the wire taxonomy is single-sourced in the OpenAPI contract.
 *
 * Phase 0 is a no-wire-change extraction: the neutral types exist and `BTRuntime`
 * implements the interface, but the HTTP layer still reads the BT-specific
 * surface (`get_health`/`get_tree_path`). Phase 1 (#114) moves the wire onto the
 * neutral `status()` view.
 */

namespace anolis {

namespace events {
class EventEmitter;
}  // namespace events

namespace automation {

/**
 * @brief Engine-neutral execution status (the cross-engine intersection).
 *
 * Deliberately coarse; engine-specific richness stays below the seam in
 * `AutomationStatusView::engine_diagnostics`. `paused`/`cancelled` are
 * intentionally excluded in v1 (see the RFC).
 */
enum class AutomationStatus {
    Idle,       ///< No definition loaded, or loaded but not advancing.
    Running,    ///< Advancing.
    Blocked,    ///< Loaded + running but not advancing (engine-defined reason).
    Failed,     ///< The definition reached an unsuccessful terminal outcome.
    Completed,  ///< The engine declared a terminal *completed* state.
    Unknown     ///< Safety floor.
};

const char* to_string(AutomationStatus status);

/**
 * @brief Immutable identity of the automation a run executed.
 *
 * `digest` is computed from the exact loaded bytes; `digest_scope` records what
 * those bytes covered so a consumer never over-trusts the hash (see D2). For BT
 * the scope is "top_level_file" until include-closure hashing lands.
 */
struct AutomationVersion {
    std::string engine_kind;                      ///< e.g. "behavior_tree".
    std::string id;                               ///< Human label (e.g. file basename) — NOT identity.
    std::string digest;                           ///< Hex sha256 of the loaded definition bytes.
    std::string digest_scope = "top_level_file";  ///< "include_closure" | "top_level_file".
};

/**
 * @brief One coherent snapshot of engine status (read under the engine's lock).
 */
struct AutomationStatusView {
    AutomationStatus status = AutomationStatus::Idle;
    AutomationVersion version;
    uint64_t last_evaluation_at_epoch_ms = 0;  ///< Last engine evaluation (NOT observable progress).
    std::string last_error;
    nlohmann::json engine_diagnostics = nlohmann::json::object();  ///< UNSTABLE / non-contractual.
};

/**
 * @brief Opaque loaded-definition snapshot (truthful — the bytes that were parsed).
 */
struct DefinitionArtifact {
    std::string media_type;  ///< e.g. "application/xml".
    std::string digest;      ///< Matches AutomationVersion::digest.
    std::string bytes;       ///< The exact loaded definition.
};

/**
 * @brief Reference to a definition to load. For BT this is a source file path.
 */
struct AutomationDefinitionRef {
    std::string source_path;
};

/**
 * @brief Result of a load attempt (C++20 has no std::expected; tiny local type,
 *        mirroring the existing `bool + error` idiom). The `ok`/`error` pair is
 *        also the #4 admission hook shape.
 */
struct LoadOutcome {
    bool ok = false;
    std::string error;
    AutomationVersion version;
};

/**
 * @brief Engine-agnostic automation interface (internal seam).
 */
class IAutomationEngine {
public:
    virtual ~IAutomationEngine() = default;

    /// Stable engine kind, e.g. "behavior_tree".
    virtual std::string_view engine_kind() const = 0;

    /// Validate + load a definition and atomically swap it in. On failure the
    /// previously-active definition is preserved. The bool+error is the #4 hook;
    /// load() is the single #5 swap point.
    virtual LoadOutcome load(const AutomationDefinitionRef& ref) = 0;

    /// Begin advancing (BT: spawn the tick thread). Tick rate is engine config.
    virtual bool start(std::string& error) = 0;

    /// Stop advancing gracefully (BT: join the tick thread).
    virtual void stop() = 0;

    virtual bool is_running() const = 0;

    /// One coherent neutral status snapshot. MUST NOT expose BT types.
    virtual AutomationStatusView status() const = 0;

    /// The exact loaded definition snapshot (not a re-read from disk).
    virtual std::optional<DefinitionArtifact> definition() const = 0;

    /// Sink for engine-emitted events (faults, etc.). Set before start().
    virtual void set_event_sink(const std::shared_ptr<events::EventEmitter>& emitter) = 0;
};

}  // namespace automation
}  // namespace anolis
