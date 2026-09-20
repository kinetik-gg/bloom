// Production general-display stage acceptance: the ACTUAL application/session stage factory
// (makeSessionRefreshingGpuSceneStage) fed by the ONE shared runtime GpuOcioContextResolver built
// from the packaged tools staged beside this executable. There are no manually injected tool paths
// and no injected preparer: the resolver qualifies the real packaged tools off the UI thread and
// supplies both the builder's effect/media/ACES working-space context and the same shared preparer
// to the general display program service.
//
// Coverage:
//   * a Bloom Neutral project (solid + text) prepares a GPU scene and a Neutral-bound display
//     program;
//   * a project switched to the ACES 1.3 CG config with the ACEScg working space (solid + text)
//     prepares a GPU scene and an ACES-bound display program;
//   * the config/working switch leaves no stale colour: the two programs do not match each other's
//     binding and have distinct command identities;
//   * a warm identical request adds no new compile (the shared preparer's cache is warm);
//   * a missing-tools resolver yields no fabricated program (typed CPU fallback);
//   * cancellation is typed.
//
// Without the pinned tools the test is a clean skip. Media leaves and CST effects are covered by
// the runtime preparation/native suites (which exercise the same builder + shared resolver
// context).

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
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
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    return {.cpuWorkerCount = 2,
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
identityFor(const bloom::document::Snapshot& snapshot, const bloom::document::CompositionId id,
            const bloom::runtime::EvaluationColorIntent intent) {
    return {.projectId = snapshot.project().id(),
            .compositionId = id,
            .sourceRevision = snapshot.revision(),
            .requestGeneration = 1,
            .time = bloom::core::RationalTime::fromInteger(0),
            .output = bloom::runtime::PreviewOutput::Composition,
            .resolution = bloom::runtime::CompositionFormatResolution{},
            .quality = bloom::runtime::EvaluationQuality::Reference,
            .colorIntent = intent,
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
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::CompiledPlanCacheHandle planCache = std::make_shared<bloom::ui::CompiledPlanCache>();

    Fixture() : compiler(definitions), scheduler(schedulerConfig()) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
    }
};

// Runs the ACTUAL session-refreshing stage (with the shared resolver) on a scheduler worker and
// returns the outcome handle.
[[nodiscard]] std::shared_ptr<const bloom::runtime::PreviewGpuSceneStageOutcome>
runStage(Fixture& fixture, std::shared_ptr<bloom::runtime::GpuOcioContextResolver> resolver,
         const bloom::runtime::PreviewRequestIdentity& identity,
         const bloom::document::Snapshot& snapshot, Expectations& expectations,
         const std::string& label) {
    auto function = bloom::ui::makeSessionRefreshingGpuSceneStage(
        fixture.compiler, fixture.evaluator, fixture.provider, nullptr, nullptr, fixture.planCache,
        std::move(resolver));
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
    const auto deadline = std::chrono::steady_clock::now() + 60s;
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

#ifdef BLOOM_GPU_TOOLS_AVAILABLE
#if BLOOM_GPU_TOOLS_AVAILABLE
[[nodiscard]] std::optional<bloom::core::Sha256Digest>
parsePinnedDigest(const std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + bloom::core::kSha256HexCharacters ||
        text.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return bloom::core::Sha256Digest::fromLowercaseHex(text.substr(prefix.size()));
}

[[nodiscard]] std::shared_ptr<bloom::runtime::GpuOcioContextResolver> packagedResolver() {
    bloom::runtime::GpuOcioContextRequest request;
    request.applicationExecutable = std::filesystem::path{BLOOM_GPU_GENERAL_TEST_EXECUTABLE};
    request.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    request.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    request.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    request.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
    request.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#ifdef BLOOM_GPU_TOOLS_BUNDLE_RELATIVE
    request.toolPackage.bundleRelative = true;
#endif
#ifdef BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256
    request.toolPackage.glslangStagedDigest =
        parsePinnedDigest(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256);
#endif
#ifdef BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256
    request.toolPackage.spirvValStagedDigest =
        parsePinnedDigest(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256);
#endif
    return std::make_shared<bloom::runtime::GpuOcioContextResolver>(std::move(request));
}

[[nodiscard]] bloom::document::ColorSettings
acesColorSettings(const bloom::document::ColorSettings& base) {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::abort();
    }
    auto settings = base;
    settings.processColorSpaceId = "ACEScg";
    settings.ocioConfig.locator =
        bloom::document::BuiltInOcioConfigLocator{std::string(bloom::color::kAcesCgV1ConfigUri)};
    settings.ocioConfig.expectedRevision.digest = *revision;
    return settings;
}
#endif
#endif

} // namespace

int runTests(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    Fixture fixture;

    // --- Bloom Neutral project: solid + text -> Neutral-bound general program --------------------
    auto neutralProject = makeProject();
    const auto neutralComposition = neutralProject.initialCompositionId;
    bloom::document::Document neutralDocument(std::move(neutralProject.project));
    bloom::commands::CommandStack neutralCommands(neutralDocument);
    bloom::ui::CompositionSession neutralSession(neutralDocument, neutralCommands,
                                                 neutralComposition);
    expectations.expect(neutralSession.addSolidLayer(QStringLiteral("Solid"),
                                                     bloom::core::Color4d{0.2, 0.5, 0.7, 1.0}),
                        "neutral: solid layer added");
    expectations.expect(
        neutralSession.addTextLayer(QStringLiteral("Title"), QStringLiteral("Bloom")),
        "neutral: text layer added");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto neutralSnapshot = neutralSession.snapshot();
    const auto neutralIdentity =
        identityFor(neutralSnapshot, neutralComposition, neutralSession.colorIntent());

    auto resolver = packagedResolver();
    const auto neutralOutcome =
        runStage(fixture, resolver, neutralIdentity, neutralSnapshot, expectations, "neutral");
    expectations.expect(neutralOutcome != nullptr &&
                            neutralOutcome->status == PreviewGpuSceneStageStatus::Prepared,
                        "neutral: solid+text prepares a GPU scene");
    const bool neutralHasProgram = neutralOutcome != nullptr && neutralOutcome->stage != nullptr &&
                                   neutralOutcome->stage->hasGeneralDisplayProgram();
    expectations.expect(neutralHasProgram, "neutral: the stage carries a general display program");
    if (!neutralHasProgram) {
        if (neutralOutcome != nullptr && neutralOutcome->stage != nullptr) {
            for (const auto& diagnostic : neutralOutcome->stage->diagnostics()) {
                std::cerr << "  neutral diagnostic " << diagnostic.code << ": "
                          << diagnostic.summary << '\n';
            }
        }
        std::cerr << expectations.failures() << " general stage expectation(s) failed\n";
        return 1;
    }
    const auto neutralProgram = *neutralOutcome->stage->displayProgram();
    const auto neutralBinding = bloom::runtime::gpuDisplayColorBindingForIntent(
        neutralIdentity.colorIntent, neutralIdentity.displayName, neutralIdentity.viewName);
    expectations.expect(
        bloom::runtime::gpuDisplayProgramMatchesRequest(neutralProgram, neutralBinding),
        "neutral: the program matches the Neutral project binding");
    expectations.expect(neutralProgram.binding.expectedRevision ==
                            bloom::color::kBloomNeutralV1ConfigDigest,
                        "neutral: the program records the Neutral content revision");

    // --- ACES project: switch config + working space, solid + text ------------------------------
    auto acesProject = makeProject();
    const auto acesComposition = acesProject.initialCompositionId;
    bloom::document::Document acesDocument(std::move(acesProject.project));
    bloom::commands::CommandStack acesCommands(acesDocument);
    bloom::ui::CompositionSession acesSession(acesDocument, acesCommands, acesComposition);
    acesSession.setColorSettings(acesColorSettings(acesSession.colorSettings()));
    expectations.expect(acesSession.addSolidLayer(QStringLiteral("Solid"),
                                                  bloom::core::Color4d{0.2, 0.5, 0.7, 1.0}),
                        "aces: solid layer added");
    expectations.expect(acesSession.addTextLayer(QStringLiteral("Title"), QStringLiteral("Bloom")),
                        "aces: text layer added");
    const auto acesSnapshot = acesSession.snapshot();
    const auto acesIdentity = identityFor(acesSnapshot, acesComposition, acesSession.colorIntent());
    expectations.expect(acesIdentity.colorIntent.workingColorSpaceId == "ACEScg" &&
                            acesIdentity.colorIntent.ocioConfigUri ==
                                bloom::color::kAcesCgV1ConfigUri,
                        "aces: the session reports the ACES config + working space");

    const auto acesOutcome =
        runStage(fixture, resolver, acesIdentity, acesSnapshot, expectations, "aces");
    expectations.expect(acesOutcome != nullptr &&
                            acesOutcome->status == PreviewGpuSceneStageStatus::Prepared,
                        "aces: solid+text prepares a GPU scene in the ACES working space");
    const bool acesHasProgram = acesOutcome != nullptr && acesOutcome->stage != nullptr &&
                                acesOutcome->stage->hasGeneralDisplayProgram();
    expectations.expect(acesHasProgram,
                        "aces: the stage carries an ACES-bound general display program");
    if (acesHasProgram) {
        const auto acesProgram = *acesOutcome->stage->displayProgram();
        const auto acesBinding = bloom::runtime::gpuDisplayColorBindingForIntent(
            acesIdentity.colorIntent, acesIdentity.displayName, acesIdentity.viewName);
        expectations.expect(
            bloom::runtime::gpuDisplayProgramMatchesRequest(acesProgram, acesBinding),
            "aces: the program matches the ACES project binding");
        expectations.expect(acesProgram.command->identity() != neutralProgram.command->identity(),
                            "switch: the config/working switch changes the command identity");
        expectations.expect(
            !bloom::runtime::gpuDisplayProgramMatchesRequest(neutralProgram, acesBinding),
            "switch: the Neutral program is refused for the ACES binding (no stale colour)");
        expectations.expect(
            !bloom::runtime::gpuDisplayProgramMatchesRequest(acesProgram, neutralBinding),
            "switch: the ACES program is refused for the Neutral binding");

        // --- Warm identical request: no new compile, shared cache hit ---------------------------
        const auto beforeWarm = resolver->preparer()->counters();
        const auto warmOutcome =
            runStage(fixture, resolver, acesIdentity, acesSnapshot, expectations, "aces-warm");
        const auto afterWarm = resolver->preparer()->counters();
        expectations.expect(warmOutcome != nullptr && warmOutcome->stage != nullptr &&
                                warmOutcome->stage->hasGeneralDisplayProgram() &&
                                warmOutcome->stage->displayProgram()->command->identity() ==
                                    acesProgram.command->identity(),
                            "warm: the identical request returns the same command");
        expectations.expect(afterWarm.compiles == beforeWarm.compiles,
                            "warm: the identical request adds no new compile");
        expectations.expect(afterWarm.cacheHits > beforeWarm.cacheHits,
                            "warm: the shared preparer served the command from its cache");
    }

    // --- Cancellation is typed (a fresh resolver, before any tool work) ----------------------
    bloom::runtime::GpuOcioContextResolver freshResolver;
    const auto cancelled = freshResolver.resolve([] { return true; });
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.error == bloom::runtime::GpuOcioContextError::Cancelled,
                        "cancel: a cancelled shared resolve is a typed refusal");

    // --- Missing tools: no fabricated program (typed CPU fallback) ---------------------------
    auto missingResolver = std::make_shared<bloom::runtime::GpuOcioContextResolver>();
    const auto missingOutcome = runStage(fixture, missingResolver, acesIdentity, acesSnapshot,
                                         expectations, "missing-tools");
    expectations.expect(missingOutcome != nullptr,
                        "missing-tools: the stage still returns a terminal outcome");
    expectations.expect(missingOutcome != nullptr && missingOutcome->stage != nullptr &&
                            !missingOutcome->stage->hasGeneralDisplayProgram(),
                        "missing-tools: no general display program is fabricated");
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
