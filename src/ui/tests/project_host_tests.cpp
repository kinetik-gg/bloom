#include <bloom/ui/project_host.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/bloom_neutral_profile.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "zip_container_test_support.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QLabel>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
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

void connectCopyCapture(ProjectHost& host, OutcomeCapture& capture) {
    QObject::connect(&host, &ProjectHost::copyFinished,
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
    // The application's startup project (and File > New, the same createNewBlank()) is Bloom
    // Neutral v1, whose working space is lin_rec709_scene.
    expectations.expect(host.colorSettings() != nullptr &&
                            *host.colorSettings() ==
                                bloom::document::makeBloomNeutralColorSettingsV1(
                                    bloom::host::kBloomNeutralV1ConfigDigest),
                        "new-project construction: a new project uses Bloom Neutral v1");
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
// SAVEFIX-1's autosave, driven through the host: a dirty session writes a recovery file into the
// host's own recovery directory, a successful Save retires it, and the startup offer is decided
// through an injected seam so no dialog is ever presented here.
// ---------------------------------------------------------------------------------------------

void testRecoverySnapshotAndStartupOffer(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "recovery: temp directory is available");
        return;
    }
    const auto recoveryDirectory = directory.path() / "recovery";
    const auto targetPath = directory.path() / "project.bloom";

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    host.setRecoveryDirectory(recoveryDirectory);

    host.writeRecoverySnapshot();
    expectations.expect(!host.recoveryOffer().has_value(),
                        "recovery: a clean session leaves nothing to offer");

    expectations.expect(executeRename(host, "Recovered Project"),
                        "recovery: the rename transaction commits");
    host.writeRecoverySnapshot();
    expectations.expect(waitUntil([&] { return host.recoveryOffer().has_value(); }),
                        "recovery: the asynchronous snapshot completes");
    const auto offer = host.recoveryOffer();
    expectations.expect(offer.has_value() && std::filesystem::exists(offer->recoveryPath),
                        "recovery: a dirty session writes a recovery file worth offering");
    expectations.expect(host.isDirty(),
                        "recovery: writing one accepts no savepoint -- the session stays dirty");

    host.beginSaveAs(targetPath);
    expectations.expect(waitUntilIdle(host), "recovery: the real save reaches a terminal state");
    expectations.expect(!host.isDirty() && std::filesystem::exists(targetPath),
                        "recovery: the real save publishes and cleans the session");
    expectations.expect(offer.has_value() && waitUntil([&] {
                            return !std::filesystem::exists(offer->recoveryPath);
                        }),
                        "recovery: a successful save removes the recovery file");
    expectations.expect(!host.recoveryOffer().has_value(),
                        "recovery: and there is nothing left to offer on the next launch");

    // The startup offer seam: a declined offer opens nothing, an accepted one opens the recovery
    // file through the ordinary Open path.
    expectations.expect(executeRename(host, "Edited Again"), "recovery: a second edit commits");
    host.writeRecoverySnapshot();
    expectations.expect(waitUntil([&] { return host.recoveryOffer().has_value(); }),
                        "recovery: the second asynchronous snapshot completes");
    const auto secondOffer = host.recoveryOffer();
    expectations.expect(secondOffer.has_value(), "recovery: the second edit is described on disk");
    if (!secondOffer.has_value()) {
        return;
    }

    int asked = 0;
    host.setRecoveryDecisionProvider([&asked](const std::filesystem::path&) {
        ++asked;
        return false;
    });
    host.offerRecoveryOnStartup();
    expectations.expect(waitUntil([&] { return asked == 1; }),
                        "recovery: the startup scan reaches its decision");
    expectations.expect(asked == 1 && !host.isBusy(),
                        "recovery: a declined offer asks once and opens nothing");

    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Discard; });
    host.setRecoveryDecisionProvider([&asked](const std::filesystem::path&) {
        ++asked;
        return true;
    });
    host.offerRecoveryOnStartup();
    expectations.expect(waitUntil([&] { return asked == 2; }),
                        "recovery: the second startup scan reaches its decision");
    expectations.expect(asked == 2, "recovery: an accepted offer asks again and proceeds");
    expectations.expect(waitUntilIdle(host),
                        "recovery: the recovery open reaches a terminal state");
    expectations.expect(host.displayPath() == std::optional(secondOffer->recoveryPath),
                        "recovery: recovering opens the recovery file like any other project");
    host.writeRecoverySnapshot();
    expectations.expect(std::filesystem::exists(secondOffer->recoveryPath),
                        "recovery: a clean tick never deletes the opened recovery project");
}

// A fresh host starts with the same runtime session ID as a crashed host. Its clean timer tick,
// including one delivered by a nested recovery-decision event loop, cannot retire the earlier
// host's artifact.
void testRecoverySurvivesCleanRestartAndPendingDecision(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "recovery restart: temp directory is available");
        return;
    }
    const auto recoveryDirectory = directory.path() / "recovery";
    bloom::runtime::TaskScheduler scheduler;
    std::filesystem::path crashedPath;
    bloom::host::ProjectSessionId crashedSessionId;
    {
        ProjectHost crashed(scheduler);
        crashed.setRecoveryDirectory(recoveryDirectory);
        crashedSessionId = crashed.stateSnapshot().projectSessionId;
        expectations.expect(executeRename(crashed, "Unsaved before restart"),
                            "recovery restart: the first host becomes dirty");
        crashed.writeRecoverySnapshot();
        expectations.expect(waitUntil([&] { return crashed.recoveryOffer().has_value(); }),
                            "recovery restart: the asynchronous snapshot completes");
        const auto offer = crashed.recoveryOffer();
        expectations.expect(offer.has_value(),
                            "recovery restart: the first host publishes a recovery artifact");
        if (!offer.has_value()) {
            return;
        }
        crashedPath = offer->recoveryPath;
    }

    ProjectHost restarted(scheduler);
    restarted.setRecoveryDirectory(recoveryDirectory);
    expectations.expect(restarted.stateSnapshot().projectSessionId == crashedSessionId,
                        "recovery restart: runtime session IDs restart at the same value");
    restarted.writeRecoverySnapshot();
    expectations.expect(std::filesystem::exists(crashedPath),
                        "recovery restart: a clean host tick preserves the previous artifact");

    restarted.setRecoveryPolicy({.interval = std::chrono::seconds{1}});
    int decisions = 0;
    restarted.setRecoveryDecisionProvider([&](const std::filesystem::path& offeredPath) {
        ++decisions;
        expectations.expect(offeredPath == crashedPath,
                            "recovery restart: startup offers the prior artifact");
        QEventLoop nestedDecision;
        QTimer::singleShot(1'300, &nestedDecision, &QEventLoop::quit);
        nestedDecision.exec(); // The host's one-second recovery timer runs inside this dialog.
        expectations.expect(std::filesystem::exists(crashedPath),
                            "recovery restart: the pending decision protects the offered file");
        return false;
    });
    restarted.offerRecoveryOnStartup();
    expectations.expect(waitUntil([&] { return decisions == 1; }),
                        "recovery restart: the asynchronous scan reaches its decision");
    expectations.expect(decisions == 1 && std::filesystem::exists(crashedPath),
                        "recovery restart: declining the offer leaves the prior artifact intact");
}

void testConcurrentRecoveryOwnersAndExplicitDiscard(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "recovery owners: temp directory is available");
        return;
    }
    const auto recoveryDirectory = directory.path() / "recovery";
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost first(scheduler);
    ProjectHost second(scheduler);
    first.setRecoveryDirectory(recoveryDirectory);
    second.setRecoveryDirectory(recoveryDirectory);
    expectations.expect(first.stateSnapshot().projectSessionId ==
                            second.stateSnapshot().projectSessionId,
                        "recovery owners: concurrent hosts have colliding runtime session IDs");
    expectations.expect(executeRename(first, "First unsaved"),
                        "recovery owners: the first host becomes dirty");
    first.writeRecoverySnapshot();
    expectations.expect(waitUntil([&] { return first.recoveryOffer().has_value(); }),
                        "recovery owners: first asynchronous snapshot completes");
    const auto firstOffer = first.recoveryOffer();
    expectations.expect(firstOffer.has_value(),
                        "recovery owners: the first host publishes an artifact");
    if (!firstOffer.has_value()) {
        return;
    }
    const auto firstPath = firstOffer->recoveryPath;
    const auto artifactCount = [&] {
        std::error_code code;
        if (!std::filesystem::exists(recoveryDirectory, code) || code) {
            return std::size_t{0};
        }
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(recoveryDirectory, code)) {
            files += entry.path().extension() == ".bloom" ? 1U : 0U;
        }
        return code ? std::size_t{0} : files;
    };

    second.writeRecoverySnapshot();
    expectations.expect(std::filesystem::exists(firstPath) && artifactCount() == 1,
                        "recovery owners: a concurrent clean host cannot remove the first file");
    expectations.expect(executeRename(second, "Second unsaved"),
                        "recovery owners: the second host becomes dirty");
    second.writeRecoverySnapshot();
    expectations.expect(waitUntil([&] { return artifactCount() == 2; }),
                        "recovery owners: second asynchronous snapshot completes");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(recoveryDirectory)) {
        if (entry.path().extension() == ".bloom") {
            paths.push_back(entry.path());
        }
    }
    expectations.expect(paths.size() == 2,
                        "recovery owners: two concurrent hosts publish separate artifacts");
    if (paths.size() != 2) {
        return;
    }
    const auto secondPath = paths.front() == firstPath ? paths.back() : paths.front();
    expectations.expect(secondPath != firstPath && std::filesystem::exists(secondPath),
                        "recovery owners: the second artifact is independent");

    second.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Discard; });
    second.newProject();
    expectations.expect(waitUntil([&] { return !std::filesystem::exists(secondPath); }) &&
                            std::filesystem::exists(firstPath) && !second.isDirty(),
                        "recovery owners: explicit Discard retires only the discarded session's "
                        "artifact");

    first.beginSaveAs(directory.path() / "first.bloom");
    expectations.expect(waitUntilIdle(first), "recovery owners: the first real save completes");
    expectations.expect(waitUntil([&] { return !std::filesystem::exists(firstPath); }) &&
                            !first.isDirty(),
                        "recovery owners: saving the remaining host retires its own artifact");
}

void testDiscardKeepsRecoveryWhenOpenFails(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "failed open: temp directory is available");
        return;
    }
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    host.setRecoveryDirectory(directory.path() / "recovery");
    expectations.expect(executeRename(host, "Keep this work"),
                        "failed open: the current project becomes dirty");
    host.writeRecoverySnapshot();
    expectations.expect(waitUntil([&] { return host.recoveryOffer().has_value(); }),
                        "failed open: recovery snapshot completes");
    const auto offer = host.recoveryOffer();
    if (!offer.has_value()) {
        return;
    }
    OutcomeCapture opened;
    connectOpenCapture(host, opened);
    host.setUnsavedChangeDecisionProvider([] { return UnsavedChangeDecision::Discard; });
    host.setOpenPathProvider([&] { return directory.path() / "absent.bloom"; });
    host.requestOpen();
    expectations.expect(waitUntil([&] { return opened.count == 1; }),
                        "failed open: the worker reports the missing path");
    expectations.expect(opened.outcome == ProjectHostOperationOutcome::Failed && host.isDirty() &&
                            std::filesystem::exists(offer->recoveryPath),
                        "failed open: the dirty session and its recovery file survive Discard");
    host.newProject();
    expectations.expect(waitUntil([&] { return !std::filesystem::exists(offer->recoveryPath); }),
                        "failed open: actual replacement retires the old owned artifact");
}

void testOpenCancellationIsPromptWhileWorkerIsHeld(Expectations& expectations) {
    bloom::runtime::TaskScheduler scheduler;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    auto gate = scheduler.submit<void>(
        bloom::runtime::TaskRequest("Held project read",
                                    {.kind = bloom::runtime::TaskOwnerKind::Application,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(902)},
                                    bloom::runtime::TaskPriority::Foreground,
                                    bloom::runtime::TaskExecutor::BlockingIo),
        [&](bloom::runtime::TaskContext&) -> bloom::runtime::TaskResult<void> {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return released; });
            return bloom::runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(gate.accepted(), "cancel Open: the held worker is admitted");
    if (!gate.accepted()) {
        return;
    }
    {
        std::unique_lock lock(mutex);
        expectations.expect(
            condition.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
            "cancel Open: the worker is held before path I/O");
    }
    ProjectHost host(scheduler);
    OutcomeCapture opened;
    connectOpenCapture(host, opened);
    const auto originalSession = host.stateSnapshot().projectSessionId;
    host.beginOpen("/tmp/bloom-missing-held-open.bloom");
    expectations.expect(host.activity() == ProjectHostActivity::Opening,
                        "cancel Open: Opening activity is published before worker I/O");
    host.cancelOpen();
    expectations.expect(!host.isBusy() && opened.count == 1 &&
                            opened.outcome == ProjectHostOperationOutcome::Cancelled,
                        "cancel Open: cancellation acknowledges without waiting for the worker");
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    condition.notify_all();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }) &&
                            host.stateSnapshot().projectSessionId == originalSession,
                        "cancel Open: abandoned worker cannot install a replacement");
}

void testRecoveryCoalescesToNewestRevision(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "recovery revision: temporary directory exists");
        return;
    }
    bloom::runtime::TaskScheduler scheduler;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    auto gate = scheduler.submit<void>(
        bloom::runtime::TaskRequest("Held recovery worker",
                                    {.kind = bloom::runtime::TaskOwnerKind::Application,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(903)},
                                    bloom::runtime::TaskPriority::Foreground,
                                    bloom::runtime::TaskExecutor::BlockingIo),
        [&](bloom::runtime::TaskContext&) -> bloom::runtime::TaskResult<void> {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return released; });
            return bloom::runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(gate.accepted(), "recovery revision: held worker admits");
    if (!gate.accepted()) {
        return;
    }
    {
        std::unique_lock lock(mutex);
        expectations.expect(
            condition.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
            "recovery revision: worker is held");
    }
    ProjectHost host(scheduler);
    host.setRecoveryDirectory(directory.path() / "recovery");
    expectations.expect(executeRename(host, "First revision"),
                        "recovery revision: first edit succeeds");
    host.writeRecoverySnapshot();
    expectations.expect(executeRename(host, "Latest revision"),
                        "recovery revision: second edit succeeds while worker is held");
    host.writeRecoverySnapshot();
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    condition.notify_all();
    expectations.expect(waitUntil([&] { return host.recoveryOffer().has_value(); }),
                        "recovery revision: newest snapshot publishes after held worker releases");
    const auto offer = host.recoveryOffer();
    if (!offer.has_value()) {
        return;
    }
    ProjectHost reader(scheduler);
    reader.beginOpen(offer->recoveryPath);
    expectations.expect(waitUntilIdle(reader), "recovery revision: recovery archive reopens");
    const auto* document = reader.liveDocumentAndStack().first;
    expectations.expect(document != nullptr &&
                            document->snapshot().project().name() == "Latest revision",
                        "recovery revision: stale captured input never replaces the newest edit");
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

// ---------------------------------------------------------------------------------------------
// Save a Copy is unavailable on ordinary editable content (task SC1, issue #77): canSaveCopy() is
// false for a fresh new project, and requestSaveCopy() refuses without ever invoking the (shared)
// Save As dialog seam or touching session state.
// ---------------------------------------------------------------------------------------------

void testSaveCopyUnavailableOnEditableContent(Expectations& expectations) {
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture copyCapture;
    connectCopyCapture(host, copyCapture);

    expectations.expect(!host.canSaveCopy(),
                        "Save Copy unavailable: a fresh editable project cannot Save a Copy");

    bool dialogInvoked = false;
    host.setSaveAsPathProvider([&] {
        dialogInvoked = true;
        return std::optional<std::filesystem::path>(std::filesystem::path("/tmp/unused.bloom"));
    });

    const auto before = host.stateSnapshot();
    host.requestSaveCopy();
    expectations.expect(copyCapture.count == 1 &&
                            copyCapture.outcome == ProjectHostOperationOutcome::Refused,
                        "Save Copy unavailable: requestSaveCopy() reports a typed Refused outcome");
    expectations.expect(!dialogInvoked,
                        "Save Copy unavailable: the dialog seam is never invoked for content that "
                        "cannot Save a Copy");
    const auto after = host.stateSnapshot();
    expectations.expect(after.contentKind == before.contentKind && after.dirty == before.dirty,
                        "Save Copy unavailable: session state is completely untouched by the "
                        "refusal");
}

// ADR 0032 / issue #381: a project saved before document 1.28 whose video asset relied on the
// previous Auto colour default shows the one-time note when opened. Saving it records 1.28, so
// reopening shows no note, and an asset with an explicit input colour never triggers it.
[[nodiscard]] std::string encodeVideoProjectAtMinor(const bool explicitInputColour,
                                                    const std::uint32_t minor) {
    auto seed = bloom::document::makeNewProject("Older video", "Main",
                                                bloom::core::RationalTime::fromInteger(2));
    bloom::document::AssetRecord asset;
    asset.id = bloom::document::AssetId::fromRaw(1);
    asset.kind = bloom::document::AssetKind::Video;
    asset.locator = {"file", "project-relative", "clip.mp4", "file:///clip.mp4"};
    asset.width = 1280;
    asset.height = 720;
    asset.frames = 48;
    asset.duration = *bloom::core::RationalTime::create(2, 1);
    asset.name = "clip";
    if (explicitInputColour)
        asset.interpretation.inputColorSpaceId = "lin_rec709_scene";
    bloom::document::AssetVideoStream stream;
    stream.kind = 1;
    stream.codec = "h264";
    stream.timebase = *bloom::core::RationalTime::create(1, 24);
    stream.framePeriod = *bloom::core::RationalTime::create(1, 24);
    stream.duration = *bloom::core::RationalTime::create(2, 1);
    stream.width = 1280;
    stream.height = 720;
    // An untagged phone clip: no primaries, transfer, matrix or range.
    asset.videoStreams = {stream};
    if (!seed.project.addAsset(std::move(asset)))
        throw std::logic_error("older video asset");
    const bloom::document::Document document{std::move(seed.project)};
    const auto snapshot = document.snapshot();
    const auto settings = bloom::document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const bloom::project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch,
                                                    sortScratch};
    const auto size = bloom::project::canonicalDocumentSize(input);
    std::string text(size ? *size.value() : 0U, '\0');
    if (!size || !bloom::project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("older video document encode");
    const auto current =
        "\"minor\": " + std::to_string(bloom::project::kCanonicalDocumentSchemaVersionV1.minor) +
        "\n  },\n  \"project\"";
    const auto anchor = text.find(current);
    if (anchor == std::string::npos)
        throw std::runtime_error("older video version anchor");
    text.replace(anchor, current.find('\n'), "\"minor\": " + std::to_string(minor));
    // Before 1.29 a document has no export queue or export id allocators (ADR 0033).
    if (minor < 29) {
        for (const auto* pattern :
             {R"(,\s*"exportQueue"\s*:\s*\{\s*"jobs"\s*:\s*\[\s*\]\s*\})",
              R"(,\s*"exportJob"\s*:\s*"0")", R"(,\s*"exportOutput"\s*:\s*"0")"}) {
            const std::regex expression(pattern);
            if (!std::regex_search(text, expression))
                throw std::runtime_error("older video export queue member");
            text = std::regex_replace(text, expression, "");
        }
    }
    return text;
}

void writeArchiveAtMinor(const std::filesystem::path& path, const std::string& documentText,
                         const std::uint32_t minor) {
    const bloom::project::CanonicalManifestV1 manifest{.documentSchemaVersion = {1, minor},
                                                       .requirements = {}};
    const auto manifestSize = bloom::project::canonicalManifestSize(manifest);
    std::string manifestText(manifestSize ? *manifestSize.value() : 0U, '\0');
    if (!manifestSize || !bloom::project::encodeCanonicalManifest(manifest, manifestText))
        throw std::runtime_error("older video manifest encode");
    namespace zip = bloom::project::test;
    const auto archive = zip::buildConformingArchive(
        zip::makeStoredEntry("manifest.json", zip::toBytes(manifestText)),
        zip::makeStoredEntry("document.json", zip::toBytes(documentText)));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(archive.data()),
               static_cast<std::streamsize>(archive.size()));
}

void testOlderProjectVideoColourNotice(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "video color notice: temp directory is available");
        return;
    }
    const auto older = directory.path() / "older.bloom";
    const auto overridden = directory.path() / "overridden.bloom";
    const auto resaved = directory.path() / "resaved.bloom";
    writeArchiveAtMinor(older, encodeVideoProjectAtMinor(false, 27), 27);
    writeArchiveAtMinor(overridden, encodeVideoProjectAtMinor(true, 27), 27);

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    OutcomeCapture openCapture;
    OutcomeCapture saveCapture;
    connectOpenCapture(host, openCapture);
    connectSaveCapture(host, saveCapture);
    QString noticeAtSignal;
    QObject::connect(&host, &ProjectHost::openFinished,
                     [&] { noticeAtSignal = host.openNotice(); });

    host.beginOpen(older);
    expectations.expect(waitUntilIdle(host) && openCapture.count == 1 &&
                            openCapture.outcome == ProjectHostOperationOutcome::Published,
                        "video color notice: the 1.27 project opens");
    expectations.expect(
        !noticeAtSignal.isEmpty() && noticeAtSignal.contains("earlier version of Bloom") &&
            noticeAtSignal.contains("affects 1 video in") &&
            noticeAtSignal.contains("Input Color"),
        "video color notice: an older project with an Auto video asset shows the note, got: " +
            noticeAtSignal.toStdString());

    host.beginSaveAs(resaved);
    expectations.expect(waitUntilIdle(host) && saveCapture.count == 1 &&
                            saveCapture.outcome == ProjectHostOperationOutcome::Published,
                        "video color notice: the older project saves");
    host.beginOpen(resaved);
    expectations.expect(waitUntilIdle(host) && openCapture.count == 2 &&
                            openCapture.outcome == ProjectHostOperationOutcome::Published,
                        "video color notice: the resaved project opens");
    expectations.expect(noticeAtSignal.isEmpty() && host.openNotice().isEmpty(),
                        "video color notice: a project saved at 1.28 shows the note no more");

    host.beginOpen(overridden);
    expectations.expect(waitUntilIdle(host) && openCapture.count == 3 &&
                            openCapture.outcome == ProjectHostOperationOutcome::Published,
                        "video color notice: the overridden 1.27 project opens");
    expectations.expect(noticeAtSignal.isEmpty(),
                        "video color notice: an explicit input color is preserved and needs no "
                        "note");
    const auto snapshot = host.stateSnapshot();
    auto [document, stack] = host.liveDocumentAndStack();
    expectations.expect(
        snapshot.contentKind == ProjectSessionContentKind::DecodedDocument && document != nullptr &&
            !document->snapshot().project().assets().empty() &&
            document->snapshot().project().assets().front().interpretation.inputColorSpaceId ==
                "lin_rec709_scene",
        "video color notice: the explicit input color opens exactly as saved");
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Lane C: the DEFAULT dialogs. Save As goes through kit::chooseFile(), which adds `.bloom` to a
// name typed without it, and the unsaved-change prompt names the project and the action, with
// Save as its default.
// ---------------------------------------------------------------------------------------------

void testDefaultSaveAsAddsBloomSuffix(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "default Save As: temp directory is available");
        return;
    }
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    QWidget window;
    host.setDialogOwner(&window);
    OutcomeCapture saveCapture;
    connectSaveCapture(host, saveCapture);
    const auto typed = QString::fromStdString((directory.path() / "shot").string());
    bool dialogSeen = false;
    QTimer::singleShot(0, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QFileDialog*>(widget); dialog && dialog->isVisible()) {
                dialogSeen = dialog->parentWidget() == &window;
                dialog->selectFile(typed);
                QMetaObject::invokeMethod(dialog, "accept");
                return;
            }
    });
    host.requestSaveAs();
    expectations.expect(waitUntilIdle(host), "default Save As: the save completes");
    expectations.expect(dialogSeen, "default Save As: the file dialog is owned by the window");
    expectations.expect(host.displayPath() == std::optional(directory.path() / "shot.bloom") &&
                            std::filesystem::exists(directory.path() / "shot.bloom"),
                        "default Save As: a name typed without .bloom is saved with it");
}

void testDefaultUnsavedPromptNamesTheAction(Expectations& expectations) {
    bloom::runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    QWidget window;
    host.setDialogOwner(&window);
    expectations.expect(executeRename(host, "Dirty"), "unsaved prompt: the project is dirty");
    QString heading;
    bool saveIsDefault = false;
    QTimer::singleShot(0, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<bloom::ui::kit::KDialog*>(widget);
                dialog && dialog->isVisible() &&
                dialog->objectName() == QStringLiteral("projectUnsavedChanges")) {
                if (auto* label = dialog->findChild<QLabel*>("projectUnsavedChangesHeading"))
                    heading = label->text();
                saveIsDefault = dialog->defaultRole() == bloom::ui::kit::KDialog::Role::Accept &&
                                dialog->button(bloom::ui::kit::KDialog::Role::Accept)->text() ==
                                    QStringLiteral("Save");
                dialog->reject();
                return;
            }
    });
    host.requestOpen();
    expectations.expect(heading.contains(QStringLiteral("Untitled")) &&
                            heading.contains(QStringLiteral("before opening another project")),
                        "unsaved prompt: it names the project and the action");
    expectations.expect(saveIsDefault, "unsaved prompt: Save is the visible default");
    expectations.expect(host.isDirty() && !host.isBusy(), "unsaved prompt: Cancel keeps the edit");
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Qt's widget file dialog saves its state into the user's Qt settings: not from a test.
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    Expectations expectations;
    testNewProjectConstruction(expectations);
    testDirtyAfterExecutedTransaction(expectations);
    testFullSaveThenOpenCycle(expectations);
    testRecoverySnapshotAndStartupOffer(expectations);
    testRecoverySurvivesCleanRestartAndPendingDecision(expectations);
    testConcurrentRecoveryOwnersAndExplicitDiscard(expectations);
    testDiscardKeepsRecoveryWhenOpenFails(expectations);
    testOpenCancellationIsPromptWhileWorkerIsHeld(expectations);
    testRecoveryCoalescesToNewestRevision(expectations);
    testBusyRefusalWhileInFlight(expectations);
    testTypedFailureSurfacedOnUnwritablePath(expectations);
    testUnsavedChangeDecisionSeamForNew(expectations);
    testUnsavedChangeDecisionSeamForOpen(expectations);
    testPathlessSaveRoutesToSaveAsDecision(expectations);
    testSaveCopyUnavailableOnEditableContent(expectations);
    testDefaultSaveAsAddsBloomSuffix(expectations);
    testDefaultUnsavedPromptNamesTheAction(expectations);
    testOlderProjectVideoColourNotice(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
