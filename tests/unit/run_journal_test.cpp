#include "runs/run_journal.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using anolis::runs::CloseReason;
using anolis::runs::label_from_open_request;
using anolis::runs::run_from_json;
using anolis::runs::RunEvent;
using anolis::runs::RunEventCategory;
using anolis::runs::RunJournal;
using anolis::runs::RunOpenSpec;
using anolis::runs::RunState;
using anolis::runs::to_json;

std::filesystem::path unique_dir() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() / ("anolis-runs-test-" + std::to_string(nonce));
    std::filesystem::create_directories(p);
    return p;
}

RunOpenSpec spec_with_scope() {
    RunOpenSpec spec;
    spec.label = "campaign-A";
    spec.params = {{"setpoint", 25.0}};
    spec.tag_scope.provider_ids = {"bread0"};
    spec.tag_scope.device_ids = {"rlht0"};
    return spec;
}

}  // namespace

TEST(RunJournalTest, OpenCloseRoundTrip) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 2000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto opened = journal.open(spec_with_scope(), std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(opened.run.state, RunState::Open);
    EXPECT_FALSE(opened.run.runtime_version.empty());  // ANOLIS_VERSION recorded
    EXPECT_FALSE(opened.run.run_id.empty());
    EXPECT_EQ(opened.run.run_id.rfind("rt1-", 0), 0U);  // runtime-name prefixed
    EXPECT_EQ(opened.run.polling_interval_ms, 2000);
    EXPECT_EQ(journal.open_run_id().value_or(""), opened.run.run_id);

    auto got = journal.get(opened.run.run_id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->label.value_or(""), "campaign-A");
    EXPECT_EQ(got->tag_scope.provider_ids.size(), 1U);

    auto closed = journal.close(opened.run.run_id, CloseReason::OperatorStop);
    ASSERT_TRUE(closed.ok) << closed.error;
    ASSERT_TRUE(closed.run.has_value());
    EXPECT_EQ(closed.run->state, RunState::Closed);
    EXPECT_EQ(closed.run->close_reason.value(), CloseReason::OperatorStop);
    EXPECT_TRUE(closed.run->ended_at_epoch_ms.has_value());
    EXPECT_FALSE(journal.open_run_id().has_value());

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AtMostOneOpenRun) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto first = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(first.ok);
    auto second = journal.open(RunOpenSpec{}, std::nullopt);
    EXPECT_FALSE(second.ok);
    EXPECT_NE(second.error.find("already open"), std::string::npos);

    journal.close(first.run.run_id, CloseReason::OperatorStop);
    auto third = journal.open(RunOpenSpec{}, std::nullopt);  // allowed once the first is closed
    EXPECT_TRUE(third.ok);

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AbandonedRecoveryOnRestart) {
    auto dir = unique_dir();
    std::string open_id;
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        auto opened = journal.open(RunOpenSpec{}, std::nullopt);
        ASSERT_TRUE(opened.ok);
        open_id = opened.run.run_id;
        // simulate a crash: drop the journal without closing.
    }
    {
        RunJournal restarted(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(restarted.initialize(err)) << err;
        EXPECT_FALSE(restarted.open_run_id().has_value());  // no run left open
        auto got = restarted.get(open_id);
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(got->state, RunState::Closed);
        EXPECT_EQ(got->close_reason.value(), CloseReason::Abandoned);
    }
    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, CloseIsIdempotentAndUnknownFails) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok);
    auto c1 = journal.close(opened.run.run_id, CloseReason::Completed);
    auto c2 = journal.close(opened.run.run_id, CloseReason::OperatorStop);  // already closed
    EXPECT_TRUE(c1.ok);
    EXPECT_TRUE(c2.ok);
    EXPECT_EQ(c2.run->close_reason.value(), CloseReason::Completed);  // first close wins
    auto unknown = journal.close("rt1-NONEXISTENT", CloseReason::OperatorStop);
    EXPECT_FALSE(unknown.ok);
    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, TornLineToleratedOnRecovery) {
    auto dir = unique_dir();
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        auto opened = journal.open(RunOpenSpec{}, std::nullopt);
        journal.close(opened.run.run_id, CloseReason::OperatorStop);
    }
    // Append a malformed (torn) final line.
    {
        std::ofstream out(dir / "runs" / "index.jsonl", std::ios::app);
        out << "{ this is not valid json\n";
    }
    RunJournal restarted(dir, "rt1", 1000);
    std::string err;
    EXPECT_TRUE(restarted.initialize(err)) << err;  // recovers despite the torn line
    EXPECT_EQ(restarted.list(10, 0).size(), 1U);    // the one valid run survived
    std::filesystem::remove_all(dir);
}

namespace {

// A line cut off before its newline, as a process killed mid-write leaves it.
void append_torn(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::app | std::ios::binary);
    out << text;  // no trailing newline
}

size_t count_category(const std::vector<RunEvent>& events, RunEventCategory category) {
    size_t n = 0;
    for (const auto& e : events) {
        if (e.category == category) ++n;
    }
    return n;
}

}  // namespace

// #639: the next record must not land on a torn final line. Case A: a torn
// close record, then two restarts. The run is recovered as abandoned once.
TEST(RunJournalTest, ATornLastLineDoesNotSwallowTheRecoveryRecord) {
    auto dir = unique_dir();
    std::string id;
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        id = journal.open(RunOpenSpec{}, std::nullopt).run.run_id;
    }
    append_torn(dir / "runs" / "index.jsonl", "{\"schema_version\":1,\"run_id\":\"" + id + "\",\"state\":\"clo");
    for (int restart = 0; restart < 2; ++restart) {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
    }
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto run = journal.get(id);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->state, RunState::Closed);
    EXPECT_EQ(count_category(journal.list_events(id, 100, 0), RunEventCategory::RunClosed), 1u)
        << "the run was recovered as abandoned on more than one restart";
    std::filesystem::remove_all(dir);
}

// Case B: a torn tail, then a new run left open. It survives the restart and
// is recovered as abandoned.
TEST(RunJournalTest, ATornLastLineDoesNotSwallowTheNextRunsOpenRecord) {
    auto dir = unique_dir();
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        auto first = journal.open(RunOpenSpec{}, std::nullopt);
        journal.close(first.run.run_id, CloseReason::OperatorStop);
    }
    append_torn(dir / "runs" / "index.jsonl", "{\"schema_version\":1,\"run_id\":\"rt1-TORN");
    std::string second;
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        second = journal.open(RunOpenSpec{}, std::nullopt).run.run_id;
    }
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto run = journal.get(second);
    ASSERT_TRUE(run.has_value()) << "the run opened after the torn line was lost";
    EXPECT_EQ(run->close_reason, CloseReason::Abandoned);
    std::filesystem::remove_all(dir);
}

// Case C: a torn line in a run's events file. The next event is listed.
TEST(RunJournalTest, ATornEventLineDoesNotSwallowTheNextEvent) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    append_torn(dir / "runs" / (opened.run.run_id + ".events.jsonl"), "{\"category\":\"annot");
    auto appended =
        journal.append_event(opened.run.run_id, RunEventCategory::Annotation, "note", nlohmann::json::object(), 0);
    ASSERT_TRUE(appended.ok) << appended.error;
    EXPECT_EQ(count_category(journal.list_events(opened.run.run_id, 100, 0), RunEventCategory::Annotation), 1u)
        << "the event appended after the torn line was not listed";
    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, ManualOnlyRunHasNullVersionVsRecorded) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto manual = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(manual.ok);
    EXPECT_FALSE(manual.run.automation_version.has_value());
    journal.close(manual.run.run_id, CloseReason::OperatorStop);

    anolis::runs::AutomationVersionRecord ver{"behavior_tree", "tree", "abc123", "top_level_file"};
    auto automated = journal.open(RunOpenSpec{}, ver);
    ASSERT_TRUE(automated.ok);
    ASSERT_TRUE(automated.run.automation_version.has_value());
    EXPECT_EQ(automated.run.automation_version->digest, "abc123");

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, ListIsNewestFirstWithPagination) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        auto o = journal.open(RunOpenSpec{}, std::nullopt);
        ASSERT_TRUE(o.ok);
        ids.push_back(o.run.run_id);
        journal.close(o.run.run_id, CloseReason::OperatorStop);
    }
    auto page = journal.list(2, 0);
    ASSERT_EQ(page.size(), 2U);
    EXPECT_EQ(page[0].run_id, ids[4]);  // newest first
    EXPECT_EQ(page[1].run_id, ids[3]);
    auto page2 = journal.list(2, 2);
    ASSERT_EQ(page2.size(), 2U);
    EXPECT_EQ(page2[0].run_id, ids[2]);
    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, EventStreamLifecycleAndMonotonicSequence) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok);
    const std::string id = opened.run.run_id;

    // run_opened is the first event (sequence 1).
    auto after_open = journal.list_events(id, 100, 0);
    ASSERT_EQ(after_open.size(), 1U);
    EXPECT_EQ(after_open[0].category, RunEventCategory::RunOpened);
    EXPECT_EQ(after_open[0].sequence, 1U);

    auto marker = journal.append_event(id, RunEventCategory::Annotation, "sample", {{"volume_ml", 5}}, 0);
    ASSERT_TRUE(marker.ok) << marker.error;
    EXPECT_EQ(marker.event.sequence, 2U);
    EXPECT_EQ(marker.event.type, "sample");
    EXPECT_NE(marker.event.recorded_at_epoch_ms, 0U);

    auto closed = journal.close(id, CloseReason::Completed);
    ASSERT_TRUE(closed.ok);

    auto all = journal.list_events(id, 100, 0);
    ASSERT_EQ(all.size(), 3U);  // run_opened, annotation, run_closed — oldest-first
    EXPECT_EQ(all[2].category, RunEventCategory::RunClosed);
    EXPECT_EQ(all[2].type, "completed");  // close reason rides as type
    EXPECT_EQ(all[0].sequence, 1U);
    EXPECT_EQ(all[1].sequence, 2U);
    EXPECT_EQ(all[2].sequence, 3U);

    // Pagination (oldest-first).
    auto page = journal.list_events(id, 1, 1);
    ASSERT_EQ(page.size(), 1U);
    EXPECT_EQ(page[0].sequence, 2U);

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AppendEventRejectsClosedAndUnknownRun) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto unknown = journal.append_event("rt1-NONE", RunEventCategory::Annotation, "x", {}, 0);
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(unknown.error.find("unknown run"), std::string::npos);

    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok);
    journal.close(opened.run.run_id, CloseReason::OperatorStop);
    auto on_closed = journal.append_event(opened.run.run_id, RunEventCategory::Annotation, "x", {}, 0);
    EXPECT_FALSE(on_closed.ok);  // closed runs are immutable
    EXPECT_NE(on_closed.error.find("closed"), std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, EnqueueFaultIsPersistedAsyncToOpenRun) {
    auto dir = unique_dir();
    RunJournal journal(dir, "rt1", 1000);
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok);
    const std::string id = opened.run.run_id;

    journal.enqueue_fault("tick", "provider call timed out", 0);

    // The background writer persists it off-thread; poll for it (bounded wait).
    bool found = false;
    for (int i = 0; i < 200 && !found; ++i) {
        for (const auto& ev : journal.list_events(id, 100, 0)) {
            if (ev.category == RunEventCategory::AutomationFault) {
                EXPECT_EQ(ev.type, "tick");
                EXPECT_EQ(ev.payload.value("message", ""), "provider call timed out");
                found = true;
            }
        }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(found) << "async fault was not persisted within the timeout";

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AbandonedRecoveryWritesRunClosedEvent) {
    auto dir = unique_dir();
    std::string id;
    {
        RunJournal journal(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(journal.initialize(err)) << err;
        auto opened = journal.open(RunOpenSpec{}, std::nullopt);
        ASSERT_TRUE(opened.ok);
        id = opened.run.run_id;  // crash: drop without closing
    }
    {
        RunJournal restarted(dir, "rt1", 1000);
        std::string err;
        ASSERT_TRUE(restarted.initialize(err)) << err;
        auto events = restarted.list_events(id, 100, 0);
        ASSERT_EQ(events.size(), 2U);  // run_opened + recovery run_closed
        EXPECT_EQ(events[1].category, RunEventCategory::RunClosed);
        EXPECT_EQ(events[1].type, "abandoned");
        EXPECT_EQ(events[1].sequence, 2U);  // sequence continued across restart
        EXPECT_TRUE(events[1].payload.value("recovery", false));
    }
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Durability (#551): each append is synced to storage before the call returns,
// through the injectable sync hook.
// ---------------------------------------------------------------------------

namespace {

// Records every sync the journal asks for, and fails the ones `fail` names.
struct SyncRecorder {
    struct Call {
        std::filesystem::path path;
        bool directory = false;
        std::thread::id thread;
    };

    std::mutex mutex;
    std::vector<Call> calls;
    std::function<bool(const std::filesystem::path&, bool)> fail;

    RunJournal::SyncFn hook() {
        return [this](const std::filesystem::path& path, bool directory) {
            std::lock_guard<std::mutex> lock(mutex);
            calls.push_back({path, directory, std::this_thread::get_id()});
            return !(fail && fail(path, directory));
        };
    }

    bool saw(const std::filesystem::path& path, bool directory) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& call : calls) {
            if (call.path == path && call.directory == directory) return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        calls.clear();
    }
};

std::filesystem::path events_file(const std::filesystem::path& dir, const std::string& run_id) {
    return dir / "runs" / (run_id + ".events.jsonl");
}

}  // namespace

TEST(RunJournalTest, OpenSyncsTheIndexTheEventsFileAndTheDirectory) {
    auto dir = unique_dir();
    SyncRecorder sync;
    RunJournal journal(dir, "rt1", 1000, sync.hook());
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;

    // index.jsonl and the events file are both created by this open, so each
    // file's sync is followed by one of the runs directory.
    std::vector<std::pair<std::filesystem::path, bool>> calls;
    for (const auto& call : sync.calls) calls.emplace_back(call.path, call.directory);
    const std::vector<std::pair<std::filesystem::path, bool>> expected = {
        {dir / "runs" / "index.jsonl", false},
        {dir / "runs", true},
        {events_file(dir, opened.run.run_id), false},
        {dir / "runs", true},
    };
    EXPECT_EQ(calls, expected);

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, CloseSyncsTheIndex) {
    auto dir = unique_dir();
    SyncRecorder sync;
    RunJournal journal(dir, "rt1", 1000, sync.hook());
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;
    sync.clear();

    auto closed = journal.close(opened.run.run_id, CloseReason::Completed);
    ASSERT_TRUE(closed.ok) << closed.error;

    EXPECT_TRUE(sync.saw(dir / "runs" / "index.jsonl", false)) << "the close record was not synced";
    EXPECT_TRUE(sync.saw(events_file(dir, opened.run.run_id), false)) << "run_closed was not synced";

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AnOpenWhoseRecordCannotBeSyncedFails) {
    auto dir = unique_dir();
    SyncRecorder sync;
    const auto index = dir / "runs" / "index.jsonl";
    sync.fail = [&](const std::filesystem::path& path, bool directory) { return !directory && path == index; };
    RunJournal journal(dir, "rt1", 1000, sync.hook());
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;

    auto opened = journal.open(RunOpenSpec{}, std::nullopt);

    EXPECT_FALSE(opened.ok) << "open reported success for a record that was not synced";
    EXPECT_FALSE(journal.open_run_id().has_value());

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AnEventThatCannotBeSyncedIsNotReportedAppended) {
    auto dir = unique_dir();
    SyncRecorder sync;
    RunJournal journal(dir, "rt1", 1000, sync.hook());
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;
    const auto events = events_file(dir, opened.run.run_id);
    sync.fail = [&](const std::filesystem::path& path, bool directory) { return !directory && path == events; };

    auto appended =
        journal.append_event(opened.run.run_id, RunEventCategory::Annotation, "note", nlohmann::json::object(), 0);

    EXPECT_FALSE(appended.ok) << "append_event reported success for an event that was not synced";
    EXPECT_TRUE(sync.saw(events, false));

    std::filesystem::remove_all(dir);
}

TEST(RunJournalTest, AQueuedFaultIsSyncedOffTheCallersThread) {
    auto dir = unique_dir();
    SyncRecorder sync;
    RunJournal journal(dir, "rt1", 1000, sync.hook());
    std::string err;
    ASSERT_TRUE(journal.initialize(err)) << err;
    auto opened = journal.open(RunOpenSpec{}, std::nullopt);
    ASSERT_TRUE(opened.ok) << opened.error;
    sync.clear();
    const auto events = events_file(dir, opened.run.run_id);

    journal.enqueue_fault("bt/node", "boom", 0);  // as the automation tick thread does
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sync.saw(events, false) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(sync.saw(events, false)) << "the queued fault was not synced";
    std::lock_guard<std::mutex> lock(sync.mutex);
    for (const auto& call : sync.calls) {
        EXPECT_NE(call.thread, std::this_thread::get_id()) << "a sync ran on the enqueuing thread";
    }

    std::filesystem::remove_all(dir);
}

// #696: `label` names the run; `experiment_label` is its deprecated alias for
// one minor release, written beside it and read when `label` is absent.
TEST(RunLabelTest, ARecordCarriesTheLabelUnderBothNames) {
    anolis::runs::Run run;
    run.run_id = "rt1-1";
    run.label = "Batch 7";
    const auto j = to_json(run);
    EXPECT_EQ(j["label"], "Batch 7");
    EXPECT_EQ(j["experiment_label"], "Batch 7");

    run.label.reset();
    const auto unlabelled = to_json(run);
    EXPECT_TRUE(unlabelled["label"].is_null());
    EXPECT_TRUE(unlabelled["experiment_label"].is_null());
}

TEST(RunLabelTest, AnOlderRecordWithOnlyExperimentLabelReadsAsTheLabel) {
    const auto old = run_from_json({{"run_id", "rt1-1"}, {"experiment_label", "campaign-A"}});
    ASSERT_TRUE(old.has_value());
    EXPECT_EQ(old->label.value_or(""), "campaign-A");

    const auto both = run_from_json({{"run_id", "rt1-2"}, {"label", "new"}, {"experiment_label", "old"}});
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(both->label.value_or(""), "new");
}

TEST(RunLabelTest, AnOpenRequestTakesLabelOrTheDeprecatedAlias) {
    std::optional<std::string> label;
    std::string error;

    ASSERT_TRUE(label_from_open_request({{"label", "Batch 7"}}, label, error)) << error;
    EXPECT_EQ(label.value_or(""), "Batch 7");

    label.reset();
    ASSERT_TRUE(label_from_open_request({{"experiment_label", "campaign-A"}}, label, error)) << error;
    EXPECT_EQ(label.value_or(""), "campaign-A");

    label.reset();
    ASSERT_TRUE(label_from_open_request({{"label", "same"}, {"experiment_label", "same"}}, label, error)) << error;
    EXPECT_EQ(label.value_or(""), "same");

    label.reset();
    ASSERT_TRUE(label_from_open_request(nlohmann::json::object(), label, error)) << error;
    EXPECT_FALSE(label.has_value());
}

TEST(RunLabelTest, AnOpenRequestWithTwoDifferentLabelsIsRefused) {
    std::optional<std::string> label;
    std::string error;
    EXPECT_FALSE(label_from_open_request({{"label", "new"}, {"experiment_label", "old"}}, label, error));
    EXPECT_NE(error.find("experiment_label"), std::string::npos) << error;
    EXPECT_FALSE(label.has_value());
}
