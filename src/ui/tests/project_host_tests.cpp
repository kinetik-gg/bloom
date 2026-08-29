#include <bloom/ui/project_host.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// ProjectHost unit tests (task U1, issue #72): drives bloom::ui::ProjectHost's public surface --
// New/Open/Save/Save-As over a REAL bloom::runtime::TaskScheduler with the injectable decision/
// dialog seams standing in for real QMessageBox/QFileDialog prompts, exactly as an offscreen test
// must. Follows src/host/tests/session_async_io_tests.cpp's fixture idiom (TempDirectory,
// Expectations) and src/ui/tests/composition_preview_controller_tests.cpp's pumped-event-loop
// waitUntil() idiom.

namespace {

using bloom::commands::SetProjectName;
using bloom::commands::Transaction;
using bloom::host::ProjectSessionContentKind;
using bloom::ui::ProjectHost;
using bloom::ui::ProjectHostActivity;
using bloom::ui::ProjectHostOperationOutcome;
using bloom::ui::UnsavedChangeDecision;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-project-host-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] bool waitUntilIdle(const ProjectHost& host) {
    return waitUntil([&host] { return !host.isBusy(); });
}

// Executes a rename transaction directly through the host's live command stack (the same path
// CompositionSession/editors already use -- see this task's command-routing finding: dirty-state
// tracking works via ProjectSession::stateSnapshot() reading the live document/command-stack
// directly, regardless of which caller executed the transaction).
[[nodiscard]] bool executeRename(ProjectHost& host, const std::string& name) {
    auto [document, commandStack] = host.liveDocumentAndStack();
    if (document == nullptr || commandStack == nullptr) {
        return false;
    }
    Transaction transaction("Rename", commandStack->trackedRevision());
    transaction.emplace<SetProjectName>(name);
    return commandStack->execute(std::move(transaction)).changed();
}

struct OutcomeCapture final {
    int count = 0;
    ProjectHostOperationOutcome outcome = ProjectHostOperationOutcome::Refused;
    QString message;
};

void connectSaveCapture(ProjectHost& host, OutcomeCapture& capture) {
    QObject::connect(&host, &ProjectHost::saveFinished,
                     [&capture](const ProjectHostOperationOutcome outcome, const QString& message) {
                         ++capture.count;
                         capture.outcome = outcome;
                         capture.message = message;
                     });
}

void connectOpenCapture(ProjectHost& host, OutcomeCapture& capture) {
    QObject::connect(&host, &ProjectHost::openFinished,
                     [&capture](const ProjectHostOperationOutcome outcome, const QString& message) {
                         ++capture.count;
                         capture.outcome = outcome;
                         capture.message = message;
                     });
}

// ---------------------------------------------------------------------------------------------
// New-project construction: valid session, clean, not dirty.
// ---------------------------------------------------------------------------------------------

void testNewProjectConstruction(Expectations& expectations) {
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);

    const auto snapshot = host.stateSnapshot();
    expectations.expect(snapshot.valid, "new-project construction: the initial session is valid");
    expectations.expect(snapshot.contentKind == ProjectSessionContentKind::DecodedDocument,
                        "new-project construction: the initial content is a decoded document");
    expectations.expect(!snapshot.displayPath.has_value(),
                        "new-project construction: an untouched new project has no display path");
    expectations.expect(!host.isDirty(),
                        "new-project construction: an untouched new project is not dirty");
    expectations.expect(host.canSave(),
                        "new-project construction: an editable, non-busy project can save");
    expectations.expect(!host.isBusy(), "new-project construction: the host starts idle");
}

// ---------------------------------------------------------------------------------------------
// Dirty after an executed transaction.
// ---------------------------------------------------------------------------------------------

void testDirtyAfterExecutedTransaction(Expectations& expectations) {
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);

    expectations.expect(executeRename(host, "Renamed Project"),
                        "dirty after edit: the rename transaction commits");
    expectations.expect(host.isDirty(), "dirty after edit: the host reports dirty after an edit");
}

// ---------------------------------------------------------------------------------------------
// Full save -> open cycle to a temp directory through the host's async flows.
// ---------------------------------------------------------------------------------------------

void testFullSaveThenOpenCycle(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "save/open cycle: temp directory is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture saveCapture;
    OutcomeCapture openCapture;
    connectSaveCapture(host, saveCapture);
    connectOpenCapture(host, openCapture);

    expectations.expect(executeRename(host, "Round Trip Project"),
                        "save/open cycle: the rename transaction commits");
    expectations.expect(host.isDirty(), "save/open cycle: the project is dirty before saving");

    host.beginSaveAs(targetPath);
    expectations.expect(host.isBusy(), "save/open cycle: beginSaveAs starts an async save");
    expectations.expect(waitUntilIdle(host), "save/open cycle: the save reaches a terminal state");
    expectations.expect(saveCapture.count == 1 &&
                            saveCapture.outcome == ProjectHostOperationOutcome::Published,
                        "save/open cycle: the save publishes");
    expectations.expect(!host.isDirty(), "save/open cycle: the session is clean after saving");
    expectations.expect(host.displayPath() == std::optional(targetPath),
                        "save/open cycle: the display path is the saved target");
    expectations.expect(std::filesystem::exists(targetPath),
                        "save/open cycle: the file exists on disk");

    int sessionReplacedCount = 0;
    QObject::connect(&host, &ProjectHost::sessionReplaced, [&] { ++sessionReplacedCount; });

    host.beginOpen(targetPath);
    expectations.expect(host.isBusy(), "save/open cycle: beginOpen starts an async open");
    expectations.expect(waitUntilIdle(host), "save/open cycle: the open reaches a terminal state");
    expectations.expect(openCapture.count == 1 &&
                            openCapture.outcome == ProjectHostOperationOutcome::Published,
                        "save/open cycle: the open installs");
    expectations.expect(sessionReplacedCount == 1,
                        "save/open cycle: sessionReplaced fires exactly once for this open");
    expectations.expect(!host.isDirty(), "save/open cycle: the reopened session is clean");
    expectations.expect(host.displayPath() == std::optional(targetPath),
                        "save/open cycle: the reopened display path matches the opened file");

    const auto reopenedSnapshot = host.stateSnapshot();
    expectations.expect(reopenedSnapshot.contentKind == ProjectSessionContentKind::DecodedDocument,
                        "save/open cycle: the reopened content is a decoded document");
}

// ---------------------------------------------------------------------------------------------
// Busy refusal: a second begin* call while one is already in flight is refused synchronously with
// a typed Refused outcome, and never disturbs the in-flight operation.
// ---------------------------------------------------------------------------------------------

void testBusyRefusalWhileInFlight(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "busy refusal: temp directory is available");
        return;
    }
    const auto firstPath = directory.path() / "first.bloom";
    const auto secondPath = directory.path() / "second.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture saveCapture;
    connectSaveCapture(host, saveCapture);

    host.beginSaveAs(firstPath);
    expectations.expect(host.isBusy(), "busy refusal: the first save is in flight");
    expectations.expect(host.activity() == ProjectHostActivity::Saving,
                        "busy refusal: the host activity names Saving");

    // No event-loop pump between these two calls: the second request is synchronously refused
    // before the first save can possibly have completed.
    host.beginSaveAs(secondPath);
    expectations.expect(saveCapture.count == 1 &&
                            saveCapture.outcome == ProjectHostOperationOutcome::Refused,
                        "busy refusal: the second concurrent save is refused with a typed busy "
                        "outcome");
    expectations.expect(!std::filesystem::exists(secondPath),
                        "busy refusal: the refused second save never wrote a file");

    expectations.expect(waitUntilIdle(host), "busy refusal: the first save still completes");
    expectations.expect(saveCapture.count == 2 &&
                            saveCapture.outcome == ProjectHostOperationOutcome::Published,
                        "busy refusal: the first save publishes normally afterward");
    expectations.expect(std::filesystem::exists(firstPath),
                        "busy refusal: the first save's file exists");
}

// ---------------------------------------------------------------------------------------------
// Typed failure surfaced: saving to an unwritable (missing parent directory) path never reports
// success.
// ---------------------------------------------------------------------------------------------

void testTypedFailureSurfacedOnUnwritablePath(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "typed failure: temp directory is available");
        return;
    }
    const auto badPath = directory.path() / "missing-subdirectory" / "project.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture saveCapture;
    connectSaveCapture(host, saveCapture);

    host.beginSaveAs(badPath);
    expectations.expect(waitUntilIdle(host), "typed failure: the save reaches a terminal state");
    expectations.expect(saveCapture.count == 1, "typed failure: exactly one outcome is reported");
    expectations.expect(saveCapture.outcome != ProjectHostOperationOutcome::Published,
                        "typed failure: an unwritable target is never reported as Published");
    expectations.expect(!saveCapture.message.isEmpty(),
                        "typed failure: a display-ready message is provided");
    expectations.expect(!std::filesystem::exists(badPath),
                        "typed failure: no file was written to the unwritable target");
}

// ---------------------------------------------------------------------------------------------
// Unsaved-change decision seam for New: Cancel keeps the dirty project; Discard replaces it
// immediately; Save saves first, then replaces it.
// ---------------------------------------------------------------------------------------------

void testUnsavedChangeDecisionSeamForNew(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "unsaved flow (New): temp directory is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    int sessionReplacedCount = 0;
    QObject::connect(&host, &ProjectHost::sessionReplaced, [&] { ++sessionReplacedCount; });

    // Cancel: the dirty project is untouched.
    expectations.expect(executeRename(host, "Cancel Me"),
                        "unsaved flow (New): the rename transaction commits");
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Cancel; });
    host.newProject();
    expectations.expect(sessionReplacedCount == 0,
                        "unsaved flow (New): Cancel never replaces the session");
    expectations.expect(host.isDirty(), "unsaved flow (New): Cancel leaves the project dirty");

    // Discard: the session is replaced immediately, synchronously.
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Discard; });
    host.newProject();
    expectations.expect(sessionReplacedCount == 1,
                        "unsaved flow (New): Discard replaces the session exactly once");
    expectations.expect(!host.isDirty(), "unsaved flow (New): the replaced session is clean");

    // Save: dirty again, decision Save routes through the Save As dialog seam (pathless), then
    // proceeds once the save publishes.
    expectations.expect(executeRename(host, "Save Me First"),
                        "unsaved flow (New): the second rename transaction commits");
    bool saveDialogInvoked = false;
    host.setSaveAsPathProvider([&] {
        saveDialogInvoked = true;
        return std::optional(targetPath);
    });
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Save; });
    host.newProject();
    expectations.expect(waitUntil([&] { return sessionReplacedCount == 2; }),
                        "unsaved flow (New): Save eventually replaces the session");
    expectations.expect(saveDialogInvoked,
                        "unsaved flow (New): the pathless Save routed through the Save As seam");
    expectations.expect(!host.isDirty(), "unsaved flow (New): the final replaced session is clean");
    expectations.expect(std::filesystem::exists(targetPath),
                        "unsaved flow (New): the Save choice's file was actually written");
}

// ---------------------------------------------------------------------------------------------
// Unsaved-change decision seam for Open: same three answers, gating requestOpen().
// ---------------------------------------------------------------------------------------------

void testUnsavedChangeDecisionSeamForOpen(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "unsaved flow (Open): temp directory is available");
        return;
    }
    const auto openTargetPath = directory.path() / "to-open.bloom";

    // Build a real file to open by saving a throwaway host's content to it first.
    {
        bloom::runtime::TaskScheduler seedScheduler;
        ProjectHost seedHost(seedScheduler);
        expectations.expect(executeRename(seedHost, "Openable Project"),
                            "unsaved flow (Open): the seed rename transaction commits");
        seedHost.beginSaveAs(openTargetPath);
        expectations.expect(waitUntilIdle(seedHost),
                            "unsaved flow (Open): the seed save reaches a terminal state");
        expectations.expect(std::filesystem::exists(openTargetPath),
                            "unsaved flow (Open): the seed file was written");
    }

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    int sessionReplacedCount = 0;
    QObject::connect(&host, &ProjectHost::sessionReplaced, [&] { ++sessionReplacedCount; });
    host.setOpenPathProvider([&] { return std::optional(openTargetPath); });

    // Cancel: nothing happens.
    expectations.expect(executeRename(host, "Cancel Open"),
                        "unsaved flow (Open): the rename transaction commits");
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Cancel; });
    host.requestOpen();
    expectations.expect(sessionReplacedCount == 0,
                        "unsaved flow (Open): Cancel never opens anything");
    expectations.expect(host.isDirty(), "unsaved flow (Open): Cancel leaves the project dirty");

    // Discard: the open-path seam is invoked and the open proceeds.
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Discard; });
    host.requestOpen();
    expectations.expect(waitUntil([&] { return sessionReplacedCount == 1; }),
                        "unsaved flow (Open): Discard opens the chosen file");
    expectations.expect(!host.isDirty(), "unsaved flow (Open): the opened session is clean");

    // Save: dirty the reopened project, choose Save, then Open proceeds once the save publishes.
    expectations.expect(executeRename(host, "Dirty Again"),
                        "unsaved flow (Open): the re-dirtying transaction commits");
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Save; });
    host.requestOpen();
    expectations.expect(waitUntil([&] { return sessionReplacedCount == 2; }),
                        "unsaved flow (Open): Save eventually opens the chosen file");
    expectations.expect(!host.isDirty(), "unsaved flow (Open): the final opened session is clean");
}

// ---------------------------------------------------------------------------------------------
// Pathless Save routes to the Save As decision (the seam observes it).
// ---------------------------------------------------------------------------------------------

void testPathlessSaveRoutesToSaveAsDecision(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "pathless save: temp directory is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture saveCapture;
    connectSaveCapture(host, saveCapture);

    bool saveAsSeamInvoked = false;
    host.setSaveAsPathProvider([&] {
        saveAsSeamInvoked = true;
        return std::optional(targetPath);
    });

    expectations.expect(!host.stateSnapshot().displayPath.has_value(),
                        "pathless save: the fresh project has no display path");
    host.beginSave();
    expectations.expect(waitUntilIdle(host), "pathless save: the routed save completes");
    expectations.expect(saveAsSeamInvoked,
                        "pathless save: a pathless Save invokes the Save As dialog seam");
    expectations.expect(saveCapture.count == 1 &&
                            saveCapture.outcome == ProjectHostOperationOutcome::Published,
                        "pathless save: the routed save publishes");
    expectations.expect(host.displayPath() == std::optional(targetPath),
                        "pathless save: the display path is now the chosen Save As target");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testNewProjectConstruction(expectations);
    testDirtyAfterExecutedTransaction(expectations);
    testFullSaveThenOpenCycle(expectations);
    testBusyRefusalWhileInFlight(expectations);
    testTypedFailureSurfacedOnUnwritablePath(expectations);
    testUnsavedChangeDecisionSeamForNew(expectations);
    testUnsavedChangeDecisionSeamForOpen(expectations);
    testPathlessSaveRoutesToSaveAsDecision(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
