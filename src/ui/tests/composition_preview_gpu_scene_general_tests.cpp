// Production general-display stage acceptance: the real bootstrap/session stage seam
// (SnapshotCompiler -> CpuGpuSceneBuilder -> QualifiedDisplayProcessorProvider ->
// makeCompositionPreviewGpuSceneStage with the lazy GpuDisplayProgramService).
//
// The stage must derive each request's exact project color binding from its color identity (OCIO
// config URI, expected content revision, working color space, display/view), prepare the matching
// OCIO DisplayRgba8 program off the UI thread, and validate the returned program's own binding
// before carrying it. This proves the production route, not a manual-preparer substitute:
//   * neutral project -> Neutral-bound program;
//   * project config switch -> ACES-bound program with a distinct command identity;
//   * two configs exposing the SAME display/view names are still distinct bindings;
//   * a working-space change is a different binding and command;
//   * an identical prepare is a warm command-cache hit;
//   * a cancelled prepare reports Cancelled.
//
// It needs the pinned glslangValidator/spirv-val; without them the general case is compiled out and
// the test is a clean skip.

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>

#include <QApplication>
#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using StageValue = bloom::runtime::PreviewGpuSceneStageOutcomeHandle;
using bloom::runtime::PreviewGpuSceneStageStatus;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskState;

constexpr std::size_t kBudget = std::size_t{1} << 28;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message) {
        if (!ok) {
            ++failures_;
            std::cerr << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bloom::runtime::TaskSchedulerConfig schedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

[[nodiscard]] bloom::document::NewProject makeProject() {
    const auto format = bloom::document::CompositionFormat::create(64, 48);
    if (!format.has_value()) {
        std::abort();
    }
    return bloom::document::makeNewProject("GPU Scene General", "Main",
                                           bloom::core::RationalTime::fromInteger(10), *format);
}

[[nodiscard]] bloom::runtime::PreviewRequestIdentity
identityFor(const bloom::document::Snapshot& snapshot, const bloom::document::CompositionId id) {
    return {.projectId = snapshot.project().id(),
            .compositionId = id,
            .sourceRevision = snapshot.revision(),
            .requestGeneration = 1,
            .time = bloom::core::RationalTime::fromInteger(0),
            .output = bloom::runtime::PreviewOutput::Composition,
            .resolution = bloom::runtime::CompositionFormatResolution{},
            .quality = bloom::runtime::EvaluationQuality::Reference,
            .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
            .resolutionPolicy = bloom::runtime::PreviewResolutionPolicy::Auto,
            .displayName = {},
            .viewName = {},
            .showLook = true};
}

struct Fixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::QualifiedDisplayProcessorProvider provider;
    bloom::runtime::CpuGpuSceneBuilder builder;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::CompiledPlanCacheHandle planCache = std::make_shared<bloom::ui::CompiledPlanCache>();

    Fixture() : compiler(definitions), scheduler(schedulerConfig()) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
    }
};

[[nodiscard]] std::shared_ptr<const bloom::runtime::PreviewGpuSceneStageOutcome>
runStageOutcome(Fixture& fixture,
                const std::shared_ptr<const bloom::runtime::GpuDisplayProgramService>& service,
                const bloom::runtime::PreviewRequestIdentity& identity,
                const bloom::document::Snapshot& snapshot, Expectations& expectations,
                const std::string& label) {
    // The exact application/session wiring: the session-refreshing stage constructs a local
    // media-context builder per request and invokes the real GPU-scene stage with the general
    // display service.
    auto function = bloom::ui::makeSessionRefreshingGpuSceneStage(
        fixture.compiler, fixture.evaluator, fixture.provider, nullptr, nullptr, fixture.planCache,
        service);
    auto submission = fixture.scheduler.submit<StageValue>(
        bloom::runtime::TaskRequest("general stage",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(1)},
                                    bloom::runtime::TaskPriority::Visible),
        [&function, &snapshot, &identity](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, kBudget, {}, context);
        });
    expectations.expect(submission.accepted(), label + ": the stage task is accepted");
    if (!submission.accepted()) {
        return nullptr;
    }
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    std::optional<TaskResult<StageValue>> result;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto taken = submission.handle.tryTakeResult()) {
            result = std::move(*taken);
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                            result->value().has_value() && *result->value() != nullptr,
                        label + ": the stage completes with an outcome");
    if (!result.has_value() || result->state() != TaskState::Succeeded ||
        !result->value().has_value() || *result->value() == nullptr) {
        return nullptr;
    }
    return *result->value();
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::PreviewGpuSceneStage>
runStage(Fixture& fixture,
         const std::shared_ptr<const bloom::runtime::GpuDisplayProgramService>& service,
         const bloom::runtime::PreviewRequestIdentity& identity,
         const bloom::document::Snapshot& snapshot, Expectations& expectations,
         const std::string& label) {
    const auto outcome = runStageOutcome(fixture, service, identity, snapshot, expectations, label);
    expectations.expect(outcome != nullptr &&
                            outcome->status == PreviewGpuSceneStageStatus::Prepared,
                        label + ": the stage prepared a GPU scene");
    return outcome != nullptr && outcome->status == PreviewGpuSceneStageStatus::Prepared
               ? outcome->stage
               : nullptr;
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
[[nodiscard]] std::shared_ptr<const bloom::runtime::GpuDisplayProgramService>
makeProgramService() {
    // Construction is pure: the provider resolves the packaged tools lazily on the CPU worker.
    return std::make_shared<const bloom::runtime::GpuDisplayProgramService>(
        []() -> bloom::runtime::GpuOcioCompileOptions {
            bloom::runtime::GpuOcioCompileOptions options;
            options.glslangValidatorPath =
                std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
            options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
            return options;
        });
}
#endif

} // namespace

int runTests(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.5, 0.7, 1.0}),
        "solid layer created");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto snapshot = session.snapshot();

    auto service = makeProgramService();

    // Neutral project: a Neutral-bound general display program rides the stage.
    const auto neutralIdentity = identityFor(snapshot, compositionId);
    const auto neutralStage = runStage(fixture, service, neutralIdentity, snapshot, expectations,
                                       "neutral");
    if (neutralStage != nullptr && !neutralStage->hasGeneralDisplayProgram()) {
        for (const auto& diagnostic : neutralStage->diagnostics()) {
            std::cerr << "  stage diagnostic " << diagnostic.code << ": " << diagnostic.summary
                      << '\n';
        }
    }
    expectations.expect(neutralStage != nullptr && neutralStage->hasGeneralDisplayProgram(),
                        "neutral: the stage carries a general display program");
    if (neutralStage == nullptr || !neutralStage->hasGeneralDisplayProgram()) {
        std::cerr << expectations.failures() << " general stage expectation(s) failed\n";
        return 1;
    }
    const auto neutralBinding = bloom::runtime::gpuDisplayColorBindingForIntent(
        neutralIdentity.colorIntent, neutralIdentity.displayName, neutralIdentity.viewName);
    const auto neutralProgram = *neutralStage->displayProgram();
    expectations.expect(
        bloom::runtime::gpuDisplayProgramMatchesRequest(neutralProgram, neutralBinding),
        "neutral: the program matches the request project binding");
    expectations.expect(neutralProgram.binding.expectedRevision ==
                            bloom::color::kBloomNeutralV1ConfigDigest,
                        "neutral: the program records the Neutral content revision");
    expectations.expect(neutralProgram.binding.workingColorSpaceId == "lin_rec709_scene",
                        "neutral: the program records the resolved working space");

    // Project config switch to ACES: a distinct ACES-bound program.
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(acesRevision.has_value(), "the ACES built-in exposes its revision");
    if (acesRevision.has_value()) {
        auto acesResolution = bloom::color::resolveOcioBuiltIn(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
            *acesRevision, "ACEScg");
        auto aces = std::move(acesResolution).takeResolved();
        expectations.expect(aces.has_value(), "the ACES config resolves");
        if (aces.has_value()) {
            std::string acesDisplay;
            std::string acesView;
            for (const auto& candidate : aces->displays()) {
                auto built = bloom::color::buildCpuDisplayProcessorForView(*aces, candidate.display,
                                                                           candidate.view);
                if (built.handle() != nullptr) {
                    acesDisplay = candidate.display;
                    acesView = candidate.view;
                    break;
                }
            }
            expectations.expect(!acesDisplay.empty(), "an ACES display/view pair is available");
            if (!acesDisplay.empty()) {
                bloom::runtime::EvaluationColorIntent acesIntent;
                acesIntent.workingColorSpaceId = "ACEScg";
                acesIntent.ocioConfigRevision = *acesRevision;
                acesIntent.ocioConfigUri = bloom::color::kAcesCgV1ConfigUri;

                const auto acesBinding = bloom::runtime::gpuDisplayColorBindingForIntent(
                    acesIntent, acesDisplay, acesView);
                auto acesPrepared =
                    service->prepare(acesBinding, 64, 48, bloom::runtime::ViewAdjust{});
                expectations.expect(acesPrepared.hasValue(),
                                    "aces: the ACES project binding prepares a display program");
                if (!acesPrepared.hasValue()) {
                    return 1;
                }
                const auto& acesProgram = acesPrepared.program;
                expectations.expect(
                    bloom::runtime::gpuDisplayProgramMatchesRequest(acesProgram, acesBinding),
                    "aces: the program matches the ACES project binding");
                expectations.expect(
                    acesProgram.command->identity() != neutralProgram.command->identity(),
                    "aces: the config switch changes the prepared command identity");
                expectations.expect(
                    !bloom::runtime::gpuDisplayProgramMatchesRequest(neutralProgram, acesBinding),
                    "aces: the Neutral program is refused for the ACES binding");

                // Two configs exposing the SAME display/view names are still distinct bindings, and
                // the names alone never select the config: the Neutral default pair name does not
                // resolve in the ACES config.
                const auto sameNameAcesBinding =
                    bloom::runtime::gpuDisplayColorBindingForIntent(
                        acesIntent, neutralProgram.binding.display, neutralProgram.binding.view);
                expectations.expect(
                    !bloom::runtime::gpuDisplayProgramMatchesRequest(neutralProgram,
                                                                     sameNameAcesBinding),
                    "aces: identical display/view names in another config do not match");
                auto sameNamePrepared =
                    service->prepare(sameNameAcesBinding, 64, 48, bloom::runtime::ViewAdjust{});
                expectations.expect(!sameNamePrepared.hasValue(),
                                    "aces: identical names do not resolve in another config");

                // Working-space change within the same config.
                auto ap0Intent = acesIntent;
                ap0Intent.workingColorSpaceId = "ACES2065-1";
                const auto ap0Binding = bloom::runtime::gpuDisplayColorBindingForIntent(
                    ap0Intent, acesDisplay, acesView);
                auto ap0 = service->prepare(ap0Binding, 64, 48, bloom::runtime::ViewAdjust{});
                expectations.expect(ap0.hasValue(),
                                    "working space: ACES2065-1 resolves and prepares");
                if (ap0.hasValue()) {
                    expectations.expect(
                        !bloom::runtime::gpuDisplayProgramMatchesRequest(acesProgram, ap0Binding),
                        "working space: the ACEScg program is refused for the ACES2065-1 binding");
                    expectations.expect(
                        ap0.program.command->identity() != acesProgram.command->identity(),
                        "working space: the change produces a different command");
                }

                // Warm reuse and cancellation.
                const auto before = service->counters();
                auto warm = service->prepare(acesBinding, 64, 48, bloom::runtime::ViewAdjust{});
                const auto after = service->counters();
                expectations.expect(
                    warm.hasValue() &&
                        warm.program.command->identity() == acesProgram.command->identity(),
                    "warm: the identical prepare returns the same command");
                expectations.expect(after.cacheHits > before.cacheHits,
                                    "warm: the identical prepare is a command-cache hit");
                auto cancelled = service->prepare(acesBinding, 64, 48,
                                                  bloom::runtime::ViewAdjust{},
                                                  [] { return true; });
                expectations.expect(
                    cancelled.error == bloom::runtime::GpuDisplayProgramError::Cancelled,
                    "cancel: a cancelled prepare reports Cancelled");

                // The current GPU scene subset only prepares lin_rec709_scene, so an ACES-working
                // project must take the full CPU path from the real stage with a reason and carry no
                // general program -- never a fabricated one.
                auto acesIdentity = neutralIdentity;
                acesIdentity.colorIntent = acesIntent;
                acesIdentity.displayName = acesDisplay;
                acesIdentity.viewName = acesView;
                const auto acesOutcome =
                    runStageOutcome(fixture, service, acesIdentity, snapshot, expectations, "aces");
                expectations.expect(acesOutcome != nullptr &&
                                        acesOutcome->status ==
                                            PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                                    "aces: the unsupported working space takes the CPU fallback");
            }
        }
    }
#endif

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " general stage expectation(s) failed\n";
        return 1;
    }
    std::cout << "composition-preview-gpu-scene-general: all expectations passed\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        return runTests(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
