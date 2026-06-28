#include "runs/run_journal.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using anolis::runs::CloseReason;
using anolis::runs::RunEvent;
using anolis::runs::RunEventCategory;
using anolis::runs::RunJournal;
using anolis::runs::RunOpenSpec;
using anolis::runs::RunState;

std::filesystem::path unique_dir() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() / ("anolis-runs-test-" + std::to_string(nonce));
    std::filesystem::create_directories(p);
    return p;
}

RunOpenSpec spec_with_scope() {
    RunOpenSpec spec;
    spec.experiment_label = "campaign-A";
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
    EXPECT_EQ(got->experiment_label.value_or(""), "campaign-A");
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
