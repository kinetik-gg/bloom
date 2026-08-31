#include <bloom/ui/frame_export_controller.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

// Task F3 (issue #103): drives bloom::ui::FrameExportController's public surface -- "File -> Export
// Frame..." -- end to end over a REAL bloom::runtime::TaskScheduler, mirroring
// src/ui/tests/project_host_tests.cpp's fixture/pumped-event-loop idiom and src/host/tests/
// frame_export_publication_tests.cpp's fixture-attempt-building AND require()-accessor idioms
// (which this file cannot directly reuse -- src modules may not reach across a sibling module's
// tests/ directory, exactly as src/ui/tests/main_window_readonly_placeholder_tests.cpp's own
// comment documents for its own duplicate of a src/host/tests/ helper).

namespace {

namespace commands = bloom::commands;
namespace core = bloom::core;
namespace document = bloom::document;
namespace host = bloom::host;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;

using bloom::ui::CompositionSession;
using bloom::ui::FrameExportApprovalDecision;
using bloom::ui::FrameExportApprovalPrompt;
using bloom::ui::FrameExportController;
using bloom::ui::FrameExportOutcome;
using bloom::ui::TaskUiBridge;

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

// The one place this file dereferences a std::optional (or optional-shaped result type) without a
// directly-adjacent has_value()/operator bool() check: it performs that check right here and
// returns a raw pointer, so every call site below works with a plain (possibly-null) pointer
// instead -- mirrors src/host/tests/frame_export_publication_tests.cpp's own require() exactly
// (clang-tidy's bugprone-unchecked-optional-access only tracks std::optional itself across
// statements, so converting to a pointer once, in one place, is the established way to avoid
// repeating an unprovable-at-a-distance check at dozens of call sites).
template <typename Optional>
[[nodiscard]] auto require(Optional& value, Expectations& expectations,
                           const std::string_view message) -> decltype(&*value) {
    expectations.expect(static_cast<bool>(value), message);
    if (!value) {
        return nullptr;
    }
    return &*value; // NOLINT(bugprone-unchecked-optional-access) -- guarded immediately above.
}

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-frame-export-controller-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

// A small, cheap-to-evaluate composition format (mirrors src/ui/tests/composition_preview_
// controller_tests.cpp's own smallFormat()).
[[nodiscard]] document::CompositionFormat smallFormat() {
    const auto format = document::CompositionFormat::create(4, 4);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

// A composition format whose width alone exceeds the version-1 export hard limit (32768 px) while
// its total pixel count stays tiny (docs/architecture/frame-output.md's "export hard-limit result":
// "exceeded iff any source or target pixel width or height ... exceeds 32768"). Empirically (see
// this task's implementor report's "defects found" -- not this test's fault, and nothing here
// works around it): this trips ProcessFrameSemanticIdentityV1Preparer's OWN identical
// exceedsOutputLimits() preflight (src/output/process_frame_semantic_identity.cpp,
// output_limits.hpp's kOutputAnalysisMaximumDimensionV1/kOutputAnalysisMaximumPixelCountV1) at the
// Identifying stage, BEFORE the analyzer ever runs -- so it fails the whole attempt
// (OutputAnalysisAttemptFailureV1) rather than reaching a valid-but-non-approvable
// OutputAnalysisReportV1. The analyzer's own documented resource.limit-exceeded facet path is real
// (exercised by src/output/tests/output_analysis_analyzer_tests.cpp's hand-constructed inputs) but
// is unreachable through the real production pipeline for this reason -- see
// testAttemptFailureViaOverLimitCompositionSurfacesDiagnostics() below, which exercises the outcome
// this composition ACTUALLY produces (a typed Failed attempt, not NotApprovable).
[[nodiscard]] document::CompositionFormat overLimitFormat() {
    const auto format = document::CompositionFormat::create(33000, 2);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

// Owns the fixture set every test below needs. platform::StagedArtifactCoordinator/
// host::PublicationCoordinator are only constructible through their own ::create() factories and
// bloom::ui::FrameExportController is neither copyable nor movable, so all three are held in-place
// inside a std::optional and populated by setUp() -- the accessor methods below are the ONE place
// each member's optional is dereferenced (guarded by setUp()'s own already-checked success),
// mirroring src/host/tests/frame_export_publication_tests.cpp's ExportFixture exactly.
struct Fixture final {
    document::NewProject newProject;
    document::Document document;
    commands::CommandStack commandStack;
    CompositionSession session;
    runtime::NodeDefinitionRegistry nodeDefinitions;
    runtime::SnapshotCompiler compiler;
    runtime::TaskScheduler scheduler;
    TaskUiBridge bridge;
    TempDirectory directory;

    explicit Fixture(const document::CompositionFormat format = smallFormat())
        : newProject(document::makeNewProject("Export Test", "Main",
                                              core::RationalTime::fromInteger(10), format)),
          document(std::move(newProject.project)), commandStack(document),
          session(document, commandStack, newProject.initialCompositionId),
          // SnapshotCompiler has no default constructor -- only `explicit SnapshotCompiler(const
          // NodeDefinitionRegistry&)` -- but it only STORES the reference at construction; the
          // referenced registry does not need to be populated/frozen yet (that happens in the body
          // below, before any test ever calls compiler.compile()). Declaration order places
          // `nodeDefinitions` before `compiler`, so the reference is already valid here.
          compiler(nodeDefinitions), bridge(scheduler, nullptr, std::chrono::milliseconds{1}) {
        if (!runtime::registerBuiltInNodeDefinitions(nodeDefinitions)) {
            std::abort();
        }
        nodeDefinitions.freeze();
    }

    // Two-phase construction: the controller needs `directory` (a member, constructed above) and
    // the coordinators to outlive it, so all three are built here rather than in the initializer
    // list.
    [[nodiscard]] bool setUp(Expectations& expectations, const std::string_view context) {
        if (!directory.isValid()) {
            expectations.expect(false, context);
            return false;
        }
        auto artifactsResult = platform::StagedArtifactCoordinator::create({});
        auto coordinatorResult = host::PublicationCoordinator::create();
        const bool ok = artifactsResult.succeeded() && coordinatorResult.has_value();
        expectations.expect(ok, context);
        if (!ok) {
            return false;
        }
        artifacts_.emplace(std::move(artifactsResult).takeCoordinator());
        coordinator_.emplace(
            std::move(*coordinatorResult)); // NOLINT(bugprone-unchecked-optional-access)
        controller_.emplace(session, scheduler, bridge, compiler, *coordinator_, *artifacts_,
                            directory.path() / "scratch");
        return true;
    }

    [[nodiscard]] platform::StagedArtifactCoordinator& artifacts() noexcept {
        return *artifacts_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }
    [[nodiscard]] host::PublicationCoordinator& coordinator() noexcept {
        return *coordinator_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by
                              // setUp().
    }
    [[nodiscard]] FrameExportController& controller() noexcept {
        return *controller_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by
                             // setUp().
    }

  private:
    std::optional<platform::StagedArtifactCoordinator> artifacts_;
    std::optional<host::PublicationCoordinator> coordinator_;
    std::optional<FrameExportController> controller_;
};

// Compiles and analyzes the SAME snapshot/time an already-published export used, independently of
// FrameExportController, purely to obtain a processIdentity()/report() pair to feed the F1 reopen
// verifier ("the published file passes the F1 verifier independently"). Evaluation is deterministic
// (docs/architecture/frame-output.md: "the digest is stable across two independent runs over the
// identical fixture"), so this reproduces the same process identity/report the controller's own
// (by-then-discarded) attempt used.
[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1>
independentlyReanalyze(Expectations& expectations, Fixture& fixture,
                       const std::filesystem::path& verifyTarget) {
    const auto compileResult = fixture.compiler.compile(
        {.snapshot = fixture.session.snapshot(), .compositionId = fixture.session.compositionId()},
        {});
    if (compileResult.status != runtime::SnapshotCompileStatus::Compiled ||
        compileResult.plan == nullptr) {
        expectations.expect(false, "independent reanalysis: the composition compiles");
        return nullptr;
    }

    // Non-static: ExportResourceLedgerV1::reserve() hands the built reservation a shared_ptr to the
    // ledger's own internal state, not a raw reference to this wrapper object, so the returned
    // attempt's reservation stays valid correctly independent of this local going out of scope.
    output::ExportResourceLedgerV1 verifyLedger;
    host::OutputAnalysisAttemptRequestV1 request{
        .plan = compileResult.plan,
        .evaluation = {.time = fixture.session.currentTime(),
                       .output = compileResult.plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = bloom::ui::kDefaultPreviewPixelStorageByteLimit},
        .targetPath = verifyTarget,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(99)}};

    auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler, fixture.artifacts(),
                                                    verifyLedger, std::move(request));
    if (!begin) {
        expectations.expect(false, "independent reanalysis: begin submits the Resolving task");
        return nullptr;
    }
    auto runner = std::move(begin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    (void)waitUntil([&] {
        outcome = runner.tryComplete();
        return outcome.has_value();
    });
    const auto* outcomeValue = require(
        outcome, expectations, "independent reanalysis: the attempt reaches a terminal outcome");
    if (outcomeValue == nullptr) {
        return nullptr;
    }
    expectations.expect(static_cast<bool>(*outcomeValue),
                        "independent reanalysis: the attempt completes successfully");
    return *outcomeValue ? outcomeValue->attempt() : nullptr;
}

// -------------------------------------------------------------------------------------------
// Menu action gating: no composition vs. a real composition (design decision 1).
// -------------------------------------------------------------------------------------------

void testCanExportGatesOnComposition(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "gating: fixture is available")) {
        return;
    }
    expectations.expect(fixture.controller().canExport(),
                        "gating: a fresh composition with an idle controller can export");

    // Rebind to an invalid composition id, matching what ProjectHost::lowestCompositionId() returns
    // for content with no live composition (e.g. preserved-read-only) -- session.composition()
    // becomes null.
    fixture.session.rebind(fixture.document, fixture.commandStack, document::CompositionId{});
    expectations.expect(fixture.session.composition() == nullptr,
                        "gating: rebinding to an invalid id leaves no live composition");
    expectations.expect(!fixture.controller().canExport(),
                        "gating: canExport() is false with no composition");

    fixture.session.rebind(fixture.document, fixture.commandStack,
                           fixture.newProject.initialCompositionId);
    expectations.expect(fixture.controller().canExport(),
                        "gating: canExport() is true again once a composition is live");
}

// -------------------------------------------------------------------------------------------
// Full successful drive: destination -> attempt -> approval prompt data -> approve -> job ->
// Published, with the file independently reopen-verified.
// -------------------------------------------------------------------------------------------

void testFullDriveApprovedAndPublished(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "full drive: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.25, 0.5, 0.75, 1.0}),
        "full drive: the solid layer is added");

    const auto target = fixture.directory.path() / "published.exr";
    fixture.controller().setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });

    std::optional<FrameExportApprovalPrompt> capturedPrompt;
    fixture.controller().setApprovalDecisionProvider(
        [&capturedPrompt](const FrameExportApprovalPrompt& prompt) {
            capturedPrompt = prompt;
            return FrameExportApprovalDecision::Export;
        });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QString message;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString& resultMessage) {
                         ++finishedCount;
                         outcome = resultOutcome;
                         message = resultMessage;
                     });

    fixture.controller().requestExport();
    expectations.expect(fixture.controller().isBusy(), "full drive: the export starts busy");
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "full drive: the export reaches a terminal outcome");
    expectations.expect(!fixture.controller().isBusy(),
                        "full drive: the action re-enables after the terminal outcome");
    expectations.expect(fixture.controller().canExport(),
                        "full drive: canExport() is true again after the terminal outcome");

    expectations.expect(outcome == FrameExportOutcome::Published,
                        "full drive: the export publishes");
    expectations.expect(!message.isEmpty(), "full drive: a display-ready message is provided");
    expectations.expect(std::filesystem::exists(target),
                        "full drive: the target file really exists");

    // Approval dialog data assertions (design decision 3): destination, resolution, preset name,
    // facet summary, digest short form.
    const auto* prompt =
        require(capturedPrompt, expectations, "full drive: the approval prompt was presented");
    if (prompt != nullptr) {
        expectations.expect(prompt->destination == target,
                            "full drive: the prompt names the chosen destination");
        expectations.expect(prompt->width == 4 && prompt->height == 4,
                            "full drive: the prompt names the composition's own resolution");
        expectations.expect(prompt->presetName == QStringLiteral("FlatExrRgba32fLinRec709SceneV1"),
                            "full drive: the prompt names the exact serialized preset id");
        expectations.expect(prompt->facets.exactFacetCount == 11 &&
                                prompt->facets.nonExactFacetCount == 0,
                            "full drive: a nominal EXR export reports all eleven facets Exact");
        expectations.expect(prompt->digestShortForm.size() == 16,
                            "full drive: the digest short form is 16 hex characters");
    }

    // "the file REALLY existing and passing the F1 verifier independently".
    auto independentAttempt =
        independentlyReanalyze(expectations, fixture, fixture.directory.path() / "verify-only.exr");
    if (independentAttempt != nullptr) {
        const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
        const auto verifyResult = verifier.verify(target, independentAttempt->processIdentity(),
                                                  independentAttempt->report(), {});
        expectations.expect(verifyResult.status() == output::FlatExrVerifyStatusV1::Verified,
                            "full drive: the published file independently reopen-verifies");
    }
}

// -------------------------------------------------------------------------------------------
// Attempt failure via an impossible limit: an over-limit composition format surfaces typed
// diagnostics instead of an approval prompt, and never writes a file. See overLimitFormat()'s own
// comment for why this exercises the attempt-FAILURE path (ResourceLimitExceeded at the Identifying
// stage) rather than a non-approvable-but-successfully-built report: empirically, the identity
// preparer's own resource-limit preflight always intercepts an over-limit process frame before the
// analyzer ever runs, so a genuinely non-approvable OutputAnalysisReportV1 is unreachable through
// the real production pipeline for EXR v1 (documented as a finding in this task's report, not
// worked around here per the task's scope guard).
// -------------------------------------------------------------------------------------------

void testAttemptFailureViaOverLimitCompositionSurfacesDiagnostics(Expectations& expectations) {
    Fixture fixture(overLimitFormat());
    if (!fixture.setUp(expectations, "impossible limit: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.1, 0.2, 0.3, 1.0}),
        "impossible limit: the solid layer is added");

    const auto target = fixture.directory.path() / "impossible-limit.exr";
    fixture.controller().setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });
    bool approvalPromptShown = false;
    fixture.controller().setApprovalDecisionProvider(
        [&approvalPromptShown](const FrameExportApprovalPrompt&) {
            approvalPromptShown = true;
            return FrameExportApprovalDecision::Export;
        });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QString message;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString& resultMessage) {
                         ++finishedCount;
                         outcome = resultOutcome;
                         message = resultMessage;
                     });

    fixture.controller().requestExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "impossible limit: the export reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Failed,
                        "impossible limit: the outcome is typed Failed (ResourceLimitExceeded at "
                        "Identifying), not silently Published or Refused");
    expectations.expect(!message.isEmpty(),
                        "impossible limit: a typed diagnostic message is given");
    expectations.expect(!approvalPromptShown,
                        "impossible limit: the approval dialog is never presented");
    expectations.expect(!std::filesystem::exists(target),
                        "impossible limit: no file is ever written");
    expectations.expect(!fixture.controller().isBusy(),
                        "impossible limit: the action re-enables afterward");
}

// -------------------------------------------------------------------------------------------
// Cancel at approval discards cleanly: no file, and the ledger charges nothing once the declined
// attempt's shared_ptr is released.
// -------------------------------------------------------------------------------------------

void testCancelAtApprovalDiscardsCleanly(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "cancel: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.4, 0.4, 0.4, 1.0}),
        "cancel: the solid layer is added");

    const auto target = fixture.directory.path() / "cancelled.exr";
    fixture.controller().setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });
    fixture.controller().setApprovalDecisionProvider(
        [](const FrameExportApprovalPrompt&) { return FrameExportApprovalDecision::Cancel; });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString&) {
                         ++finishedCount;
                         outcome = resultOutcome;
                     });

    fixture.controller().requestExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "cancel: the export reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Cancelled,
                        "cancel: the outcome is typed Cancelled");
    expectations.expect(!std::filesystem::exists(target), "cancel: no file was ever written");
    // A brief pump lets the last shared_ptr<const OutputAnalysisAttemptV1> reference (dropped when
    // presentApproval()/handleAttemptResult() return) actually unwind before checking the ledger.
    expectations.expect(
        waitUntil([&] { return fixture.controller().chargedResourceBytes() == 0; }),
        "cancel: the discarded attempt's reservation releases (zero charged bytes)");
    expectations.expect(!fixture.controller().isBusy(), "cancel: the action re-enables afterward");
    expectations.expect(fixture.controller().canExport(), "cancel: canExport() is true again");
}

// -------------------------------------------------------------------------------------------
// Failure outcome surfaced: an external modification between approval and publish is a typed
// conflict, not a crash, and leaves the externally-written target untouched.
// -------------------------------------------------------------------------------------------

void testExternalModificationConflictSurfacedAsFailure(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "external conflict: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.6, 0.1, 0.2, 1.0}),
        "external conflict: the solid layer is added");

    const auto target = fixture.directory.path() / "conflict.exr";
    fixture.controller().setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });
    // The attempt's own Resolving stage observed an absent target (it ran before this callback);
    // writing the conflicting file from inside the (synchronous) approval callback -- immediately
    // before the job is ever submitted -- reproduces "external modification between approval and
    // publish" deterministically instead of racing a background worker thread.
    fixture.controller().setApprovalDecisionProvider([&target](const FrameExportApprovalPrompt&) {
        std::ofstream external(target, std::ios::binary);
        external << "not a bloom export";
        return FrameExportApprovalDecision::Export;
    });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QString message;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString& resultMessage) {
                         ++finishedCount;
                         outcome = resultOutcome;
                         message = resultMessage;
                     });

    fixture.controller().requestExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "external conflict: the export reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Failed,
                        "external conflict: the outcome is typed Failed, not silently Published");
    expectations.expect(!message.isEmpty(), "external conflict: a typed message is given");
    expectations.expect(!fixture.controller().isBusy(),
                        "external conflict: the action re-enables afterward");

    std::ifstream reopened(target);
    std::string contents((std::istreambuf_iterator<char>(reopened)),
                         std::istreambuf_iterator<char>());
    expectations.expect(contents == "not a bloom export",
                        "external conflict: the externally-written target is left exactly as it "
                        "was");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testCanExportGatesOnComposition(expectations);
    testFullDriveApprovedAndPublished(expectations);
    testAttemptFailureViaOverLimitCompositionSurfacesDiagnostics(expectations);
    testCancelAtApprovalDiscardsCleanly(expectations);
    testExternalModificationConflictSurfacedAsFailure(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
