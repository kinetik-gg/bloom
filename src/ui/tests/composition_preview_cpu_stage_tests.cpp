// CPU-stage seam tests: the compile/evaluate/select half and the display-application half of the
// composition preview pipeline, exercised directly through their own factories. The wrapper that
// installs them as makeCompositionPreviewPipeline() is a thin composition of exactly these two, so
// these are the meaningful unit-level checks; the existing pipeline suite covers the end-to-end
// behavior (including unsupported compilation).
//
// The four states proven here need no invented document fixtures: default-qualified (provider
// Ready), Pending reference, provider Failed, and a live parameter override. Every case also pins
// that the fallback reuses the exact ProcessFrame the stage evaluated, never recompiling or
// re-evaluating.

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QApplication>
#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using StageValue = bloom::runtime::PreviewCpuStageOutcomeHandle;
using FallbackValue = bloom::ui::PreviewPreparationResultHandle;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
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
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return bloom::document::makeNewProject("CPU Stage", "Main",
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

// Runs any preparation-shaped callable on a real scheduler task and returns its terminal result.
template <typename Value, typename Callable>
[[nodiscard]] std::optional<bloom::runtime::TaskResult<Value>>
runOnWorker(bloom::runtime::TaskScheduler& scheduler, Callable callable) {
    bloom::runtime::TaskRequest request("CPU stage task",
                                        {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                         .id = bloom::runtime::TaskOwnerId::fromRaw(1)},
                                        bloom::runtime::TaskPriority::Visible);
    auto submission = scheduler.submit<Value>(
        std::move(request), [callable = std::move(callable)](bloom::runtime::TaskContext& context) {
            return callable(context);
        });
    if (!submission.accepted()) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = submission.handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

struct Fixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider provider;
    bloom::runtime::TaskScheduler scheduler;

    Fixture() : compiler(definitions), scheduler(schedulerConfig()) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
    }

    [[nodiscard]] bloom::runtime::PreviewCpuStageFunction stage() const {
        return bloom::ui::makeCompositionPreviewCpuStage(compiler, evaluator, provider);
    }
    [[nodiscard]] bloom::runtime::PreviewCpuDisplayFallback fallback() const {
        return bloom::ui::makeCompositionPreviewCpuDisplayFallback(displayPreparer);
    }
};

// Runs the stage for `identity` and returns the evaluated stage, or null after recording a failure.
[[nodiscard]] std::shared_ptr<const bloom::runtime::PreviewCpuStage>
evaluateStage(Fixture& fixture, const bloom::runtime::PreviewRequestIdentity& identity,
              const bloom::document::Snapshot& snapshot,
              const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides,
              Expectations& expectations, const std::string& label) {
    auto function = fixture.stage();
    auto result = runOnWorker<StageValue>(
        fixture.scheduler,
        [&function, &snapshot, &identity, &overrides](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit,
                            overrides, context);
        });
    expectations.expect(result.has_value() &&
                            result->state() == bloom::runtime::TaskState::Succeeded,
                        label + ": the stage completes successfully");
    if (!result.has_value() || result->state() != bloom::runtime::TaskState::Succeeded) {
        return nullptr;
    }
    const auto& outcome = result->value();
    expectations.expect(outcome.has_value() && *outcome != nullptr,
                        label + ": the stage returns an outcome");
    if (!outcome.has_value() || *outcome == nullptr ||
        (*outcome)->status != bloom::runtime::PreviewCpuStageStatus::Evaluated) {
        return nullptr;
    }
    return (*outcome)->stage;
}

// Applies the display fallback to an evaluated stage and returns the prepared frame.
[[nodiscard]] bloom::ui::PreparedPreviewFrameHandle
applyFallback(Fixture& fixture, const std::shared_ptr<const bloom::runtime::PreviewCpuStage>& stage,
              Expectations& expectations, const std::string& label) {
    expectations.expect(stage != nullptr, label + ": a stage was evaluated");
    if (stage == nullptr) {
        return nullptr;
    }
    auto function = fixture.fallback();
    auto result = runOnWorker<FallbackValue>(
        fixture.scheduler, [&function, &stage](bloom::runtime::TaskContext& context) {
            return function(*stage, context);
        });
    expectations.expect(result.has_value() &&
                            result->state() == bloom::runtime::TaskState::Succeeded,
                        label + ": the fallback completes successfully");
    if (!result.has_value() || result->state() != bloom::runtime::TaskState::Succeeded) {
        return nullptr;
    }
    const auto& value = result->value();
    expectations.expect(value.has_value() && *value != nullptr, label + ": the fallback prepared");
    if (!value.has_value() || *value == nullptr) {
        return nullptr;
    }
    expectations.expect((*value)->status() == bloom::runtime::PreviewPreparationStatus::Prepared,
                        label + ": the fallback reports Prepared");
    const auto frame = (*value)->frame();
    // The fallback must reuse the exact ProcessFrame the stage evaluated: same pointer, no
    // recompile and no re-evaluation.
    expectations.expect(frame != nullptr && frame->hasProcessFrame() &&
                            frame->processFrame() == stage->processFrame(),
                        label + ": the fallback retains the stage's exact ProcessFrame");
    return frame;
}

void testPendingReferenceUsesReferencePath(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fixture creates a solid layer");
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    // The provider is never published: the honest Pending startup window.
    const auto stage = evaluateStage(fixture, identity, snapshot, {}, expectations, "Pending");
    expectations.expect(stage != nullptr && !stage->ocioQualified(),
                        "Pending selects no qualified processor");
    const auto frame = applyFallback(fixture, stage, expectations, "Pending");
    expectations.expect(frame != nullptr && !frame->isOcioQualified(),
                        "Pending prepares an unqualified reference frame");
}

void testDefaultQualifiedUsesQualifiedPath(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.6, 0.3, 0.1, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
    expectations.expect(fixture.provider.readiness() ==
                            bloom::runtime::QualifiedDisplayProcessorReadiness::Ready,
                        "the qualified processor is Ready");
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    const auto stage = evaluateStage(fixture, identity, snapshot, {}, expectations, "Qualified");
    expectations.expect(stage != nullptr && stage->ocioQualified(),
                        "Ready selects the qualified processor");
    const auto frame = applyFallback(fixture, stage, expectations, "Qualified");
    expectations.expect(frame != nullptr && frame->isOcioQualified(),
                        "Ready prepares a qualified frame");
}

void testFailedProviderFailsClosed(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.1, 0.9, 0.4, 1.0}),
        "the fixture creates a solid layer");
    fixture.provider.publish(bloom::runtime::QualifiedDisplayProcessorBuildResult::failed(
        {.code = "bloom.test.cpu-stage.forced-failure",
         .severity = bloom::runtime::DiagnosticSeverity::Error,
         .summary = "The Bloom Neutral display configuration could not be resolved",
         .detail = {},
         .suggestedAction = {}}));
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);
    auto function = fixture.stage();
    auto result = runOnWorker<StageValue>(
        fixture.scheduler, [&function, &snapshot, &identity](bloom::runtime::TaskContext& context) {
            return function(snapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit, {},
                            context);
        });
    expectations.expect(result.has_value() && result->state() == bloom::runtime::TaskState::Failed,
                        "a permanently failed provider fails the stage closed");
    expectations.expect(result.has_value() && !result->diagnostics().empty(),
                        "the failure carries the provider's diagnostic");
}

void testOverrideChangesPixels(Expectations& expectations) {
    Fixture fixture;
    auto project = makeProject();
    const auto compositionId = project.initialCompositionId;
    bloom::document::Document document(std::move(project.project));
    bloom::commands::CommandStack commands(document);
    bloom::ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), bloom::core::Color4d{0.2, 0.2, 0.2, 1.0}),
        "the fixture creates a solid layer");
    const auto snapshot = session.snapshot();
    const auto identity = identityFor(snapshot, compositionId);

    bloom::document::ParameterId colorParameter;
    const auto* liveComposition = snapshot.project().findComposition(compositionId);
    expectations.expect(liveComposition != nullptr, "the live composition resolves");
    if (liveComposition != nullptr) {
        for (const auto& record : liveComposition->parameters().records()) {
            if (record.schemaKey == bloom::document::kSolidColorParameterSchemaKey) {
                colorParameter = record.id;
            }
        }
    }
    expectations.expect(colorParameter.isValid(), "the solid exposes its color parameter");

    const auto baselineStage = evaluateStage(fixture, identity, snapshot, {}, expectations, "Base");
    const auto baseline = applyFallback(fixture, baselineStage, expectations, "Base");
    const std::vector<bloom::runtime::SnapshotParameterOverride> overrides{
        {snapshot.revision(), colorParameter, bloom::core::Color4d{0.9, 0.1, 0.1, 1.0}}};
    const auto overriddenStage =
        evaluateStage(fixture, identity, snapshot, overrides, expectations, "Override");
    const auto overridden = applyFallback(fixture, overriddenStage, expectations, "Override");

    expectations.expect(baseline != nullptr && overridden != nullptr,
                        "both the baseline and the override prepared frames");
    if (baseline == nullptr || overridden == nullptr) {
        return;
    }
    const auto baselineView = baseline->displayBufferView();
    const auto overriddenView = overridden->displayBufferView();
    expectations.expect(baselineView.has_value() && overriddenView.has_value(),
                        "both frames expose a display buffer");
    if (!baselineView.has_value() || !overriddenView.has_value()) {
        return;
    }
    expectations.expect(baselineView->pixels.size() == overriddenView->pixels.size() &&
                            !baselineView->pixels.empty() &&
                            std::memcmp(baselineView->pixels.data(), overriddenView->pixels.data(),
                                        baselineView->pixels.size_bytes()) != 0,
                        "the override changes the prepared pixels");
    expectations.expect(overriddenStage->processFrame() != baselineStage->processFrame(),
                        "the override evaluates its own ProcessFrame");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testPendingReferenceUsesReferencePath(expectations);
    testDefaultQualifiedUsesQualifiedPath(expectations);
    testFailedProviderFailsClosed(expectations);
    testOverrideChangesPixels(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " CPU-stage expectation(s) failed\n";
        return 1;
    }
    std::cout << "CPU-stage: all expectations passed\n";
    return 0;
}
