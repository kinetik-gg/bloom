#include "composition_export_dialog.hpp"
#include <QDialog>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>

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
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
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
#include <vector>

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
using bloom::ui::FrameExportRangeRequest;
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

    explicit Fixture(const document::CompositionFormat format = smallFormat(),
                     const core::RationalTime duration = core::RationalTime::fromInteger(10))
        : newProject(document::makeNewProject("Export Test", "Main", duration, format)),
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

    const runtime::CpuCompositionEvaluator previewEvaluator;
    const runtime::CpuReferenceDisplayPreparer previewPreparer;
    runtime::QualifiedDisplayProcessorProvider previewProvider;
    bloom::ui::CompositionPreviewController preview(
        fixture.session, fixture.scheduler, fixture.bridge,
        bloom::ui::makeCompositionPreviewPipeline(fixture.compiler, previewEvaluator,
                                                  previewPreparer, previewProvider),
        {.resolutionPolicy = runtime::PreviewResolutionPolicy::Quarter,
         .displayName = {},
         .viewName = {},
         .showLook = true});
    expectations.expect(
        waitUntil([&] { return preview.state().activity == bloom::ui::PreviewActivity::Ready; }),
        "Quarter preview is ready before exporting at Full");
    const auto previewBuffer = preview.state().frame == nullptr
                                   ? std::nullopt
                                   : preview.state().frame->displayBufferView();
    expectations.expect(previewBuffer.has_value() &&
                            previewBuffer->displayWindow.extent().width() == 1,
                        "the viewer's actual preview buffer is reduced to Quarter");

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

// -------------------------------------------------------------------------------------------
// Preset selection by destination extension (issue #111, design decision 3): the chosen extension
// -- not the dialog's selected filter entry -- selects the preset, so a hand-typed path behaves
// identically to a filtered one.
// -------------------------------------------------------------------------------------------

void testDestinationExtensionSelectsPreset(Expectations& expectations) {
    const auto preset = [](const char* path) {
        return FrameExportController::presetForDestination(std::filesystem::path(path));
    };
    expectations.expect(preset("/tmp/frame.png") == output::OutputPresetV1::PngRgba8SrgbV1,
                        "extension routing: .png selects the PNG preset");
    expectations.expect(preset("/tmp/frame.PNG") == output::OutputPresetV1::PngRgba8SrgbV1,
                        "extension routing: extension matching is ASCII case-insensitive");
    expectations.expect(preset("/tmp/frame.exr") ==
                            output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                        "extension routing: .exr keeps the flat OpenEXR preset");
    expectations.expect(preset("/tmp/frame") ==
                            output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                        "extension routing: no extension keeps the flat OpenEXR preset");
    expectations.expect(preset("/tmp/frame.tif") == output::OutputPresetV1::TiffRgba16SrgbV1 &&
                            preset("/tmp/frame.tiff") == output::OutputPresetV1::TiffRgba16SrgbV1 &&
                            preset("/tmp/frame.TIFF") == output::OutputPresetV1::TiffRgba16SrgbV1,
                        "extension routing: TIFF extensions select the typed TIFF preset");
    expectations.expect(preset("/tmp/frame.bmp") ==
                            output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                        "extension routing: an unrecognized extension keeps the flat OpenEXR "
                        "preset, never a silently different one");
}

// -------------------------------------------------------------------------------------------
// Full PNG drive through the injected destination seam: a .png destination routes to the PNG
// preset, the approval prompt reports PNG-appropriate facts, and the published file really is a
// PNG.
// -------------------------------------------------------------------------------------------

void testPngDestinationRoutesToPngPresetAndPublishes(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "png drive: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.25, 0.5, 0.75, 1.0}),
        "png drive: the solid layer is added");

    const auto target = fixture.directory.path() / "published.png";
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
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString&) {
                         ++finishedCount;
                         outcome = resultOutcome;
                     });

    fixture.controller().requestExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "png drive: the export reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Published,
                        "png drive: the export publishes");
    expectations.expect(std::filesystem::exists(target),
                        "png drive: the target file really exists");

    const auto* prompt =
        require(capturedPrompt, expectations, "png drive: the approval prompt was presented");
    if (prompt != nullptr) {
        expectations.expect(prompt->preset == output::OutputPresetV1::PngRgba8SrgbV1,
                            "png drive: the prompt reports the typed PNG preset the .png "
                            "destination selected");
        expectations.expect(prompt->presetName == QStringLiteral("PngRgba8SrgbV1"),
                            "png drive: the prompt names the exact serialized PNG preset id");
        expectations.expect(prompt->width == 4 && prompt->height == 4,
                            "png drive: the prompt names the composition's own resolution");
        // docs/architecture/frame-output.md's nominal PNG derivation: pixels, precision, color, and
        // alpha association are Approximated and external dependencies is ExternalReference -- five
        // non-exact facets; the remaining six are Exact.
        expectations.expect(prompt->facets.exactFacetCount == 6 &&
                                prompt->facets.nonExactFacetCount == 5,
                            "png drive: a nominal PNG export reports six Exact and five non-Exact "
                            "facets");
        expectations.expect(
            prompt->facets.nonExactFacetNames.contains(QStringLiteral("Color")) &&
                prompt->facets.nonExactFacetNames.contains(QStringLiteral("External Dependencies")),
            "png drive: the non-exact facet list names the PNG-specific color and "
            "external-dependency conversions");
        expectations.expect(prompt->digestShortForm.size() == 16,
                            "png drive: the digest short form is 16 hex characters");
    }

    std::ifstream published(target, std::ios::binary);
    std::array<char, 8> signature{};
    published.read(signature.data(), 8);
    static constexpr std::array<unsigned char, 8> kPngSignature{137, 80, 78, 71, 13, 10, 26, 10};
    bool signatureMatches = published.gcount() == 8;
    for (std::size_t index = 0; index < signature.size() && signatureMatches; ++index) {
        signatureMatches =
            static_cast<unsigned char>(signature.at(index)) == kPngSignature.at(index);
    }
    expectations.expect(signatureMatches,
                        "png drive: the published file carries the exact PNG signature");
}

// -------------------------------------------------------------------------------------------
// Task S3: a text layer really reaches an exported PNG's pixels. This is the end of the slice's
// chain -- document schema, compiled plan, CPU glyph rasterization, display mapping, PNG encode --
// and it is asserted by decoding the published file rather than by trusting the writer.
// -------------------------------------------------------------------------------------------

void testPngExportContainsRasterizedText(Expectations& expectations) {
    // Big enough for a readable glyph; U+2588 FULL BLOCK at 24 px per em so the assertion can name
    // exact interior pixels instead of hunting for antialiased edges. The rasterized block's own
    // geometry (origin and full-coverage interior) is pinned in src/render's own tests; here it
    // only has to be comfortably inside this frame.
    const auto textFormat = document::CompositionFormat::create(64, 48);
    if (!textFormat.has_value()) {
        expectations.expect(false, "text export: the fixture format is valid");
        return;
    }
    Fixture fixture(*textFormat);
    if (!fixture.setUp(expectations, "text export: fixture is available")) {
        return;
    }
    // The glyph box is centred at the layer's default composition-centre anchor.
    expectations.expect(fixture.session.addTextLayer(QStringLiteral("Title"),
                                                     QString::fromUtf8("\xe2\x96\x88"), 24.0,
                                                     core::Color4d{1.0, 1.0, 1.0, 1.0}),
                        "text export: the text layer is added");

    const auto target = fixture.directory.path() / "text.png";
    fixture.controller().setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });
    fixture.controller().setApprovalDecisionProvider(
        [](const FrameExportApprovalPrompt&) { return FrameExportApprovalDecision::Export; });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString&) {
                         ++finishedCount;
                         outcome = resultOutcome;
                     });
    fixture.controller().requestExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "text export: the export reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Published,
                        "text export: the export publishes");
    if (outcome != FrameExportOutcome::Published) {
        return;
    }

    // Decoded by Qt, not by Bloom's own writer, so this asserts what a reader actually sees.
    QImage decoded;
    expectations.expect(decoded.load(QString::fromStdString(target.string()), "PNG"),
                        "text export: the published PNG decodes");
    if (decoded.isNull()) {
        return;
    }
    expectations.expect(decoded.width() == 64 && decoded.height() == 48,
                        "text export: the PNG carries the composition's own resolution");
    const QImage rgba = decoded.convertToFormat(QImage::Format_RGBA8888);
    const QColor ink = rgba.pixelColor(32, 24);
    expectations.expect(ink.alpha() == 255 && ink.red() == 255 && ink.green() == 255 &&
                            ink.blue() == 255,
                        "text export: a pixel under the glyph's fully covered interior is opaque "
                        "white, which is the exported text");
    const QColor background = rgba.pixelColor(60, 44);
    expectations.expect(background.alpha() == 0,
                        "text export: a pixel the glyph does not reach stays fully transparent, so "
                        "the glyph is really glyph-shaped rather than a filled frame");

    std::size_t inkPixels = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        for (int x = 0; x < rgba.width(); ++x) {
            inkPixels += rgba.pixelColor(x, y).alpha() == 0 ? 0U : 1U;
        }
    }
    expectations.expect(inkPixels > 0 && inkPixels < static_cast<std::size_t>(rgba.width()) *
                                                         static_cast<std::size_t>(rgba.height()),
                        "text export: the exported frame is partly covered -- neither empty nor "
                        "entirely filled");
}

// -------------------------------------------------------------------------------------------
// Both presets from one controller, back to back: the EXR path is unchanged and the PNG path
// coexists with it under the same one-export-at-a-time bound.
// -------------------------------------------------------------------------------------------

void testBothPresetsExportBackToBack(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "both presets: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.6, 1.0}),
        "both presets: the solid layer is added");

    QStringList presetNames;
    fixture.controller().setApprovalDecisionProvider(
        [&presetNames](const FrameExportApprovalPrompt& prompt) {
            presetNames << prompt.presetName;
            return FrameExportApprovalDecision::Export;
        });

    int finishedCount = 0;
    std::vector<FrameExportOutcome> outcomes;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString&) {
                         ++finishedCount;
                         outcomes.push_back(resultOutcome);
                     });

    const auto exrTarget = fixture.directory.path() / "both.exr";
    const auto pngTarget = fixture.directory.path() / "both.png";
    fixture.controller().beginExport(exrTarget);
    expectations.expect(!fixture.controller().canExport(),
                        "both presets: the one-export-at-a-time bound holds while the first export "
                        "is in flight");
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "both presets: the EXR export reaches a terminal outcome");
    fixture.controller().beginExport(pngTarget);
    expectations.expect(waitUntil([&] { return finishedCount == 2; }),
                        "both presets: the PNG export reaches a terminal outcome");

    expectations.expect(outcomes.size() == 2 && outcomes[0] == FrameExportOutcome::Published &&
                            outcomes[1] == FrameExportOutcome::Published,
                        "both presets: both exports publish");
    expectations.expect(std::filesystem::exists(exrTarget) && std::filesystem::exists(pngTarget),
                        "both presets: both target files exist");
    expectations.expect(presetNames == QStringList{QStringLiteral("FlatExrRgba32fLinRec709SceneV1"),
                                                   QStringLiteral("PngRgba8SrgbV1")},
                        "both presets: each approval prompt named the preset its own destination "
                        "extension selected");
}

// --- Task S5, item 3a/3c: the frame-range export
// --------------------------------------------------
//
// The proof the whole slice exists for: an animated composition exported as a SEQUENCE must carry a
// different, predictable picture in every frame's own file -- which is only true if every frame was
// evaluated at its own exact time rather than at the session's, and if the animation reached the
// pixels at all. The solid's COLOUR is what animates here, because colour animation is exactly what
// task S5's item 1 added: before it, no command could put a colour on a curve.
void testFrameRangeExportsEveryFrameAtItsOwnTime(Expectations& expectations) {
    // Four frames at 24 fps, 2x2 pixels: frames 0 through 3, each at exact time i/24.
    const auto duration = core::RationalTime::create(4, 24);
    const auto format = document::CompositionFormat::create(2, 2);
    if (!duration.has_value() || !format.has_value()) {
        expectations.expect(false, "frame range: the four-frame fixture constructs");
        return;
    }
    Fixture fixture(*format, *duration);
    if (!fixture.setUp(expectations, "frame range: fixture is available")) {
        return;
    }

    const runtime::CpuCompositionEvaluator previewEvaluator;
    const runtime::CpuReferenceDisplayPreparer previewPreparer;
    runtime::QualifiedDisplayProcessorProvider previewProvider;
    bloom::ui::CompositionPreviewController preview(
        fixture.session, fixture.scheduler, fixture.bridge,
        bloom::ui::makeCompositionPreviewPipeline(fixture.compiler, previewEvaluator,
                                                  previewPreparer, previewProvider),
        {.resolutionPolicy = runtime::PreviewResolutionPolicy::Quarter,
         .displayName = {},
         .viewName = {},
         .showLook = true});
    expectations.expect(
        waitUntil([&] { return preview.state().activity == bloom::ui::PreviewActivity::Ready; }),
        "Quarter preview is ready before exporting at Full");
    const auto previewBuffer = preview.state().frame == nullptr
                                   ? std::nullopt
                                   : preview.state().frame->displayBufferView();
    expectations.expect(previewBuffer.has_value() &&
                            previewBuffer->displayWindow.extent().width() == 1,
                        "the viewer's actual preview buffer is reduced to Quarter");

    // Black at frame 0, white at frame 3, animated in between.
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Ramp"), core::Color4d{0.0, 0.0, 0.0, 1.0}),
        "frame range: the ramp layer is added");
    expectations.expect(fixture.session.toggleKeyframe(document::kSolidColorParameterRole),
                        "frame range: the solid colour accepts the keyframe gesture");
    const auto lastTime = core::RationalTime::create(3, 24);
    if (!lastTime.has_value()) {
        expectations.expect(false, "frame range: frame 3's exact time constructs");
        return;
    }
    expectations.expect(fixture.session.setCurrentTime(*lastTime),
                        "frame range: the session steps to frame 3");
    expectations.expect(
        fixture.session.setSelectedSolidColor(core::Color4d{1.0, 1.0, 1.0, 1.0}),
        "frame range: editing the animated colour at frame 3 inserts its second key");

    // The session stays on frame 3 for the whole export: if any frame were evaluated at the SESSION
    // time rather than at its own, every file would come out identical, and the assertions below
    // would fail.
    const auto base = fixture.directory.path() / "ramp.png";
    fixture.controller().setRangeProvider([base]() -> std::optional<FrameExportRangeRequest> {
        return FrameExportRangeRequest{.destination = base, .firstFrame = 0, .lastFrame = 3};
    });
    int approvalPrompts = 0;
    fixture.controller().setApprovalDecisionProvider(
        [&approvalPrompts](const FrameExportApprovalPrompt&) {
            ++approvalPrompts;
            return FrameExportApprovalDecision::Export;
        });

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Refused;
    QString diagnosticMessage;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString& text) {
                         ++finishedCount;
                         outcome = resultOutcome;
                         diagnosticMessage = text;
                     });
    fixture.controller().requestRangeExport();
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "frame range: the range reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Published,
                        "frame range: every frame publishes");
    if (outcome != FrameExportOutcome::Published) {
        std::cerr << "  diagnostic: " << diagnosticMessage.toStdString() << '\n';
        return;
    }
    expectations.expect(approvalPrompts == 1,
                        "frame range: the artist approves ONCE for the whole range, not once per "
                        "frame -- every frame still runs its own approval with its own digest");

    // Zero-padded names, four digits, in the destination's own directory.
    std::array<QImage, 4> frames;
    for (std::uint64_t index = 0; index < 4; ++index) {
        const auto path = FrameExportController::sequenceFramePath(base, index, 3);
        expectations.expect(path.filename().string() == "ramp." +
                                                            std::string(index == 0   ? "0000"
                                                                        : index == 1 ? "0001"
                                                                        : index == 2 ? "0002"
                                                                                     : "0003") +
                                                            ".png",
                            "frame range: each frame's name is the stem, a dot, a four-digit "
                            "zero-padded index, then the extension");
        expectations.expect(std::filesystem::exists(path),
                            "frame range: every frame in the range was written");
        expectations.expect(frames[index].load(QString::fromStdString(path.string()), "PNG"),
                            "frame range: every written frame decodes as PNG");
    }
    if (std::ranges::any_of(frames, [](const QImage& image) { return image.isNull(); })) {
        return;
    }

    // THE per-frame assertion. Frame 0 is the first key exactly, frame 3 the second exactly, and
    // the two interior frames are strictly between them in strictly increasing order -- which can
    // only be true if each was evaluated at its own exact time.
    std::array<int, 4> luminance{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const QImage rgba = frames[index].convertToFormat(QImage::Format_RGBA8888);
        expectations.expect(rgba.width() == 2 && rgba.height() == 2,
                            "frame range: each frame carries the composition's own resolution");
        const QColor pixel = rgba.pixelColor(0, 0);
        expectations.expect(pixel.alpha() == 255,
                            "frame range: the animated solid is opaque in every frame");
        expectations.expect(pixel.red() == pixel.green() && pixel.green() == pixel.blue(),
                            "frame range: a neutral ramp stays neutral in every frame");
        luminance[index] = pixel.red();
    }
    expectations.expect(luminance[0] == 0, "frame range: frame 0 is exactly the first key's black");
    expectations.expect(luminance[3] == 255,
                        "frame range: frame 3 is exactly the second key's white");
    expectations.expect(luminance[0] < luminance[1] && luminance[1] < luminance[2] &&
                            luminance[2] < luminance[3],
                        "frame range: the four frames carry four strictly increasing values, so "
                        "every frame really was evaluated at its own exact time");
}

// Task S5, item 3a: a range outside the composition is refused rather than silently clamped, and a
// cancel at the approval prompt publishes nothing.
void testFrameRangeRefusalAndCancellation(Expectations& expectations) {
    const auto duration = core::RationalTime::create(4, 24);
    if (!duration.has_value()) {
        expectations.expect(false, "frame range refusal: the fixture duration constructs");
        return;
    }
    Fixture fixture(smallFormat(), *duration);
    if (!fixture.setUp(expectations, "frame range refusal: fixture is available")) {
        return;
    }
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.5, 0.5, 0.5, 1.0}),
        "frame range refusal: a layer is added");

    int finishedCount = 0;
    FrameExportOutcome outcome = FrameExportOutcome::Published;
    QString message;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](const FrameExportOutcome resultOutcome, const QString& text) {
                         ++finishedCount;
                         outcome = resultOutcome;
                         message = text;
                     });

    const auto base = fixture.directory.path() / "out.png";
    fixture.controller().beginRangeExport(
        FrameExportRangeRequest{.destination = base, .firstFrame = 0, .lastFrame = 99});
    expectations.expect(
        finishedCount == 1 && outcome == FrameExportOutcome::Refused,
        "frame range refusal: a range past the composition's last frame is refused");
    expectations.expect(message.contains(QStringLiteral("outside")),
                        "frame range refusal: and the message says the range is out of bounds");
    expectations.expect(
        !std::filesystem::exists(FrameExportController::sequenceFramePath(base, 0, 99)),
        "frame range refusal: nothing was written");

    // Declining the single approval prompt cancels the whole range before any frame publishes.
    finishedCount = 0;
    fixture.controller().setApprovalDecisionProvider(
        [](const FrameExportApprovalPrompt&) { return FrameExportApprovalDecision::Cancel; });
    fixture.controller().beginRangeExport(
        FrameExportRangeRequest{.destination = base, .firstFrame = 0, .lastFrame = 3});
    expectations.expect(waitUntil([&] { return finishedCount == 1; }),
                        "frame range refusal: the declined range reaches a terminal outcome");
    expectations.expect(outcome == FrameExportOutcome::Cancelled,
                        "frame range refusal: declining the one approval cancels the whole range");
    expectations.expect(!fixture.controller().isExportingRange() &&
                            fixture.controller().canExport(),
                        "frame range refusal: and the controller returns to idle");
    expectations.expect(fixture.controller().chargedResourceBytes() == 0,
                        "frame range refusal: a cancelled range releases every reservation");
    expectations.expect(
        !std::filesystem::exists(FrameExportController::sequenceFramePath(base, 0, 3)),
        "frame range refusal: no frame was written");
}

void testSequenceWriterParity(Expectations& expectations) {
    const auto duration = core::RationalTime::create(2, 24);
    if (!duration)
        return;
    Fixture fixture(smallFormat(), *duration);
    if (!fixture.setUp(expectations, "sequence parity fixture"))
        return;
    expectations.expect(fixture.session.addSolidLayer(QStringLiteral("Constant"),
                                                      core::Color4d{0.2, 0.3, 0.4, 1.0}),
                        "sequence parity solid");
    fixture.controller().setApprovalDecisionProvider(
        [](const FrameExportApprovalPrompt&) { return FrameExportApprovalDecision::Export; });
    bool finished = false;
    FrameExportOutcome outcome = FrameExportOutcome::Failed;
    QString diagnostic;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](FrameExportOutcome result, const QString& message) {
                         finished = true;
                         outcome = result;
                         diagnostic = message;
                     });
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    for (const char* extension : {".png", ".exr", ".tiff"}) {
        if (std::string_view(extension) == ".tiff" &&
            !output::outputPresetAvailabilityV1(output::OutputPresetV1::TiffRgba16SrgbV1).available)
            continue;
        const auto single = fixture.directory.path() / (std::string("single") + extension);
        finished = false;
        fixture.controller().beginExport(single);
        expectations.expect(waitUntil([&] { return finished; }), "single export completes");
        expectations.expect(outcome == FrameExportOutcome::Published, "single export publishes");
        if (outcome != FrameExportOutcome::Published) {
            std::cerr << diagnostic.toStdString() << '\n';
            continue;
        }
        const auto bytes = read(single);
        expectations.expect(!bytes.empty(), "single output bytes");
        const auto base = fixture.directory.path() / (std::string("sequence") + extension);
        finished = false;
        fixture.controller().beginRangeExport({base, 0, 1});
        expectations.expect(waitUntil([&] { return finished; }), "sequence completes");
        expectations.expect(outcome == FrameExportOutcome::Published, "sequence publishes");
        if (outcome != FrameExportOutcome::Published)
            std::cerr << diagnostic.toStdString() << '\n';
        for (std::uint64_t index = 0; index < 2; ++index)
            expectations.expect(read(FrameExportController::sequenceFramePath(base, index, 1)) ==
                                    bytes,
                                "PNG/EXR/TIFF sequence bytes match existing single-frame writer");
    }
}

void testCompositionExportUi(Expectations& expectations) {
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        expectations.expect(dialog != nullptr, "composition dialog opens");
        if (!dialog)
            return;
        auto* preset = dialog->findChild<bloom::ui::kit::KDropdown*>("compositionExportPreset");
        auto* profile = dialog->findChild<bloom::ui::kit::KDropdown*>("compositionExportProfile");
        auto* note = dialog->findChild<bloom::ui::kit::KLabel*>("compositionExportNote");
        expectations.expect(
            preset && preset->count() == 9 &&
                preset->isItemEnabled(5) ==
                    output::outputPresetAvailabilityV1(output::OutputPresetV1::TiffRgba16SrgbV1)
                        .available,
            "two deliverables precede the seven raw presets");
        if (output::outputPresetAvailabilityV1(output::OutputPresetV1::ProResMovV1).available) {
            expectations.expect(profile && profile->count() == 6 &&
                                    profile->currentData().toString() == QStringLiteral("hq"),
                                "six ProRes profiles default to HQ");
            expectations.expect(
                note &&
                    note->text() == QString::fromUtf8(bloom::media::provider::kProResExportNote),
                "dialog carries exact ProRes implementation note");
        }
        dialog->reject();
    });
    expectations.expect(!bloom::ui::compositionExportDialog(47, 48000),
                        "dialog cancellation yields no request");
    if (!output::outputPresetAvailabilityV1(output::OutputPresetV1::ProResMovV1).available)
        return;
    const auto format = document::CompositionFormat::create(256, 128);
    const auto duration = core::RationalTime::create(2, 24);
    if (!format || !duration)
        return;
    Fixture fixture(*format, *duration);
    if (!fixture.setUp(expectations, "movie UI fixture"))
        return;
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Picture"), core::Color4d{0.2, 0.2, 0.2, 1}),
        "movie picture");
    int approvals = 0;
    fixture.controller().setApprovalDecisionProvider([&](const FrameExportApprovalPrompt& prompt) {
        ++approvals;
        expectations.expect(prompt.implementationNote.startsWith(
                                QString::fromUtf8(bloom::media::provider::kProResExportNote)),
                            "approval carries exact ProRes note");
        return FrameExportApprovalDecision::Export;
    });
    bool finished = false;
    FrameExportOutcome outcome = FrameExportOutcome::Failed;
    QString diagnostic;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](FrameExportOutcome result, const QString& message) {
                         finished = true;
                         outcome = result;
                         diagnostic = message;
                     });
    bloom::ui::CompositionExportRequest request;
    request.range = {fixture.directory.path() / "movie.mov", 0, 1};
    request.audio = false;
    fixture.controller().beginCompositionExport(request);
    expectations.expect(waitUntil([&] { return finished; }), "movie controller completes");
    expectations.expect(outcome == FrameExportOutcome::Published && approvals == 1,
                        "movie publishes after one user approval");
    if (outcome != FrameExportOutcome::Published)
        std::cerr << diagnostic.toStdString() << '\n';
}

void testDeliverableDialog(Expectations& expectations) {
    QSettings settings;
    settings.remove("export/projects/color5-a");
    settings.remove("export/projects/color5-b");
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto* preset = dialog->findChild<bloom::ui::kit::KDropdown*>("compositionExportPreset");
        auto* start = dialog->findChild<QSpinBox*>("compositionExportStartFrame");
        auto* padding = dialog->findChild<QSpinBox*>("compositionExportPadding");
        auto* compression =
            dialog->findChild<bloom::ui::kit::KDropdown*>("compositionExportCompression");
        auto* space = dialog->findChild<bloom::ui::kit::KLineEdit*>("compositionExportOutputSpace");
        auto* look = dialog->findChild<bloom::ui::kit::KCheckBox*>("compositionExportLook");
        if (!preset || !start || !padding || !compression || !space || !look) {
            expectations.expect(false, "deliverable controls exist");
            dialog->reject();
            return;
        }
        preset->setCurrentIndex(0);
        expectations.expect(start->value() == 1001 && padding->value() == 4 &&
                                compression->currentData().toInt() ==
                                    static_cast<int>(output::FlatExrCompressionV1::Piz) &&
                                space->text() == "ACES2065-1" && !look->isChecked(),
                            "handoff fills AP0, PIZ, 1001, four digits, look off");
        start->setValue(1010);
        padding->setValue(5);
        dialog->accept();
    });
    const auto request = bloom::ui::compositionExportDialog(47, 48000, "color5-a");
    expectations.expect(
        request && request->deliverable == bloom::ui::DeliverablePreset::VfxHandoff &&
            request->range.naming.startFrame == 1010 && request->range.naming.framePadding == 5 &&
            request->range.bypassLookNodes,
        "filled fields remain editable");
    for (const auto& key : {QString("color5-a"), QString("color5-b")}) {
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* preset = dialog->findChild<bloom::ui::kit::KDropdown*>("compositionExportPreset");
            expectations.expect(preset && preset->currentIndex() == (key == "color5-a" ? 0 : 2),
                                "last deliverable isolated per project key");
            if (preset && preset->isItemEnabled(1)) {
                preset->setCurrentIndex(1);
                auto* look = dialog->findChild<bloom::ui::kit::KCheckBox*>("compositionExportLook");
                expectations.expect(look && look->isChecked(), "review turns look on");
            }
            dialog->reject();
        });
        expectations.expect(!bloom::ui::compositionExportDialog(47, 48000, key),
                            "persistence probe cancelled");
    }
    settings.remove("export/projects/color5-a");
}

void testHandoffApproval(Expectations& expectations) {
    Fixture fixture;
    if (!fixture.setUp(expectations, "handoff fixture"))
        return;
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision) {
        expectations.expect(false, "ACES config available");
        return;
    }
    auto settings = fixture.session.colorSettings();
    settings.processColorSpaceId = "ACEScg";
    settings.ocioConfig.locator =
        document::BuiltInOcioConfigLocator{std::string(bloom::color::kAcesCgV1ConfigUri)};
    settings.ocioConfig.expectedRevision.digest = *revision;
    fixture.session.setColorSettings(std::move(settings));
    expectations.expect(fixture.session.addSolidLayer("Shot", {0.18, 0.2, 0.3, 1}),
                        "handoff solid");
    int approvals = 0;
    fixture.controller().setApprovalDecisionProvider([&](const FrameExportApprovalPrompt& prompt) {
        ++approvals;
        expectations.expect(prompt.implementationNote == "Look: OFF (handoff)",
                            "handoff approval text pinned");
        expectations.expect(prompt.profile == "ACES2065-1; PIZ" &&
                                prompt.destination.filename() == "shot.1001.exr",
                            "handoff approval declares output and naming");
        return FrameExportApprovalDecision::Export;
    });
    bool finished = false;
    FrameExportOutcome outcome = FrameExportOutcome::Failed;
    QObject::connect(&fixture.controller(), &FrameExportController::exportFinished,
                     [&](FrameExportOutcome value, const QString& message) {
                         finished = true;
                         outcome = value;
                         if (value != FrameExportOutcome::Published)
                             std::cerr << message.toStdString() << '\n';
                     });
    bloom::ui::CompositionExportRequest request;
    request.preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    request.deliverable = bloom::ui::DeliverablePreset::VfxHandoff;
    request.range = {.destination = fixture.directory.path() / "shot.exr",
                     .firstFrame = 0,
                     .lastFrame = 1,
                     .naming = {.startFrame = 1001},
                     .exr = {.outputColorSpaceId = "ACES2065-1",
                             .compression = output::FlatExrCompressionV1::Piz},
                     .bypassLookNodes = true};
    fixture.controller().beginCompositionExport(std::move(request));
    expectations.expect(waitUntil([&] { return finished; }) &&
                            outcome == FrameExportOutcome::Published && approvals == 1,
                        "handoff sequence publishes with one approval");
    expectations.expect(std::filesystem::exists(fixture.directory.path() / "shot.1002.exr"),
                        "handoff second label published");
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
    testDestinationExtensionSelectsPreset(expectations);
    testPngDestinationRoutesToPngPresetAndPublishes(expectations);
    testPngExportContainsRasterizedText(expectations);
    testBothPresetsExportBackToBack(expectations);
    testSequenceWriterParity(expectations);
    testCompositionExportUi(expectations);
    testDeliverableDialog(expectations);
    testHandoffApproval(expectations);
    testFrameRangeExportsEveryFrameAtItsOwnTime(expectations);
    testFrameRangeRefusalAndCancellation(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
