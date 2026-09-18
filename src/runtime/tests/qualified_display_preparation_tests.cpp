#include <bloom/runtime/qualified_display_preparation.hpp>

#include <algorithm>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <cmath>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bloom::core::RationalTime;
using bloom::render::Rgba8;

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

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStackNode = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutputNode = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);
constexpr auto kColorParam = bloom::document::ParameterId::fromRaw(40);
constexpr auto kPositionParam = bloom::document::ParameterId::fromRaw(41);
constexpr auto kOpacityParam = bloom::document::ParameterId::fromRaw(42);
constexpr auto kAnchorParam = bloom::document::ParameterId::fromRaw(43);
constexpr auto kScaleParam = bloom::document::ParameterId::fromRaw(44);
constexpr auto kRotationParam = bloom::document::ParameterId::fromRaw(45);
constexpr auto kBlendModeParam = bloom::document::ParameterId::fromRaw(46);

[[nodiscard]] bloom::document::CompositionFormat format(const std::uint32_t width = 4,
                                                        const std::uint32_t height = 2) {
    const auto value = bloom::document::CompositionFormat::create(width, height);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

// A one-layer opaque red solid over a small format -- the same minimal shape
// cpu_composition_evaluator_tests.cpp's oneSolidPlan() uses, reduced to only what this file needs
// (a real, evaluator-produced ProcessFrame; ProcessFrame's constructor is private to
// CpuCompositionEvaluator, so it cannot be hand-built here).
[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
oneSolidPlan(const bloom::document::CompositionFormat compositionFormat = format(),
             const bloom::core::Color4d color = {1.0, 0.0, 0.0, 1.0}) {
    using namespace bloom::runtime;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{kSolidNode,
                      {kColorParam, color},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                       static_cast<double>(compositionFormat.width())},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                       static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(CompiledLayerOutput{
        kLayerNode, kLayer, OperationIndex::fromRaw(0),
        CompiledVec2Parameter{kPositionParam, bloom::document::Vec2d{2.0, 1.0}},
        CompiledVec2Parameter{kAnchorParam, bloom::document::kDefaultAnchor},
        CompiledVec2Parameter{kScaleParam, bloom::document::kDefaultScale},
        CompiledScalarParameter{kRotationParam, bloom::document::kDefaultRotationDegrees},
        CompiledScalarParameter{kOpacityParam, 1.0}, kBlendModeParam,
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(
        CompiledMerge{kStackNode, {{kSlot, kLayer, OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{kOutputNode, OperationIndex::fromRaw(2)});
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

[[nodiscard]] bloom::runtime::EvaluationRequest
requestFor(const bloom::runtime::CompiledCompositionPlan& plan,
           const std::size_t budget = 1U << 20U) {
    return {.time = RationalTime::fromInteger(0),
            .output = plan.output(),
            .resolution = bloom::runtime::CompositionFormatResolution{},
            .quality = bloom::runtime::EvaluationQuality::Reference,
            .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = budget};
}

[[nodiscard]] std::optional<bloom::color::PreparedCpuDisplayProcessorHandle> buildNeutralHandle() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (resolution.outcome() != bloom::color::OcioBuiltInRegistryOutcome::Ready) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto buildResult = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!buildResult) {
        return std::nullopt;
    }
    return std::move(buildResult).takeHandle();
}

// The golden test: this preparer's published pixels/window must match calling C2's own
// produceBloomNeutralDisplayFrame() directly on the identical process view, "composited per the
// display-window rule" -- see qualified_display_preparation.hpp's own "Display-window mechanism"
// documentation: CpuCompositionEvaluator always produces dataWindow == displayWindow (verified
// again below), so C2's data-window-sized product already covers the full reported window with no
// separate padding composite required to match.
void testGoldenMatchesC2DirectOutput(Expectations& expectations) {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneSolidPlan();
    const auto evaluated = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(evaluated.status() == bloom::runtime::EvaluationStatus::Evaluated &&
                            evaluated.frame() != nullptr,
                        "the fixture composition evaluates to a process frame");
    if (evaluated.frame() == nullptr) {
        return;
    }
    const auto* processDescriptor = evaluated.frame()->processImage().descriptor();
    expectations.expect(processDescriptor != nullptr &&
                            processDescriptor->dataWindow() == processDescriptor->displayWindow(),
                        "the evaluator's process frame has no data/display window padding, "
                        "matching this preparer's documented mechanism");

    auto handle = buildNeutralHandle();
    expectations.expect(handle.has_value(), "the Bloom Neutral processor handle builds");
    if (!handle.has_value()) {
        return;
    }

    const bloom::runtime::CpuQualifiedDisplayPreparer preparer(*handle);
    const bloom::runtime::QualifiedDisplayPreparationRequest request{
        .aggregatePixelStorageByteLimit = 1U << 20U,
        .viewAdjust = {},
        .displayName = {},
        .viewName = {},
        .showLook = true,
    };
    const auto result = preparer.prepare(evaluated.frame(), request, {});
    expectations.expect(result.status() ==
                                bloom::runtime::QualifiedDisplayPreparationStatus::Prepared &&
                            result.frame() != nullptr,
                        "the qualified preparer publishes a frame for the fixture composition");
    if (result.frame() == nullptr) {
        return;
    }

    const auto processView = evaluated.frame()->processImage().view();
    expectations.expect(static_cast<bool>(processView), "the process frame exposes a valid view");
    if (!processView) {
        return;
    }
    const auto direct = bloom::color::produceBloomNeutralDisplayFrame(
        *handle, *processView.value(), request.chunkPixelCount,
        request.aggregatePixelStorageByteLimit);
    expectations.expect(static_cast<bool>(direct), "the direct C2 call succeeds for the fixture");
    if (!direct) {
        return;
    }

    const auto preparerPixels = result.frame()->buffer().pixels();
    const auto directPixels = direct.value()->pixels();
    expectations.expect(
        std::vector<Rgba8>(preparerPixels.begin(), preparerPixels.end()) ==
            std::vector<Rgba8>(directPixels.begin(), directPixels.end()),
        "the preparer's pixels are byte-identical to calling C2's adapter directly");
    expectations.expect(
        result.frame()->buffer().displayWindow() == direct.value()->displayWindow() &&
            result.frame()->buffer().displayWindow() == processDescriptor->dataWindow(),
        "the preparer's reported window equals both C2's direct output window and "
        "the process frame's data window (the display-window rule)");
    expectations.expect(result.frame()->identity().processFrame == evaluated.frame()->identity(),
                        "the qualified frame's identity retains the exact source process identity");
    expectations.expect(result.frame()->processFrame() == evaluated.frame(),
                        "the qualified frame retains the same shared process frame product");
}

// Cancellation at a chunk boundary: gated via a real TaskScheduler task (the only way to obtain a
// live, cancellable CancellationToken -- see cancellation.hpp), paused at this preparer's own
// Preflight-complete progress checkpoint (immediately before it calls into C2's chunked apply),
// mirroring cpu_composition_evaluator_tests.cpp's CancellationGate idiom for the reference path.
class CancellationGate final {
  public:
    void pauseAtPreflightComplete(const bloom::runtime::QualifiedDisplayProgress& progress) {
        if (progress.stage != bloom::runtime::QualifiedDisplayProgressStage::Preflight ||
            progress.completed != 1) {
            return;
        }
        std::unique_lock lock(mutex_);
        if (entered_) {
            return;
        }
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

void testCancellationAtChunkBoundaryPublishesNothing(Expectations& expectations) {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneSolidPlan();
    const auto evaluated = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(evaluated.frame() != nullptr,
                        "the fixture evaluates for the cancellation test");
    if (evaluated.frame() == nullptr) {
        return;
    }
    auto handle = buildNeutralHandle();
    expectations.expect(handle.has_value(), "the handle builds for the cancellation test");
    if (!handle.has_value()) {
        return;
    }

    bloom::runtime::TaskSchedulerConfig config = bloom::runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    bloom::runtime::TaskScheduler scheduler(config);
    CancellationGate gate;
    std::atomic_bool preparerCancelled = false;
    const bloom::runtime::CpuQualifiedDisplayPreparer preparer(*handle);
    const bloom::runtime::QualifiedDisplayPreparationRequest request{
        .aggregatePixelStorageByteLimit = 1U << 20U,
        .viewAdjust = {},
        .displayName = {},
        .viewName = {},
        .showLook = true,
    };

    auto submission = scheduler.submit<void>(
        bloom::runtime::TaskRequest("Qualified display cancellation fixture",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(1)}),
        [frame = evaluated.frame(), &preparer, &request, &gate,
         &preparerCancelled](bloom::runtime::TaskContext& context) {
            const auto result =
                preparer.prepare(frame, request, context.cancellation(),
                                 [&gate](const bloom::runtime::QualifiedDisplayProgress& progress) {
                                     gate.pauseAtPreflightComplete(progress);
                                 });
            preparerCancelled.store(
                result.status() == bloom::runtime::QualifiedDisplayPreparationStatus::Cancelled,
                std::memory_order_release);
            return result.status() == bloom::runtime::QualifiedDisplayPreparationStatus::Cancelled
                       ? bloom::runtime::TaskResult<void>::cancelled()
                       : bloom::runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted() && gate.waitUntilEntered(),
                        "the cancellation fixture reaches its progress checkpoint");
    submission.handle.cancel();
    gate.release();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<bloom::runtime::TaskResult<void>> taskResult;
    while (!taskResult.has_value() && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult.has_value() &&
                            taskResult->state() == bloom::runtime::TaskState::Cancelled &&
                            preparerCancelled.load(std::memory_order_acquire),
                        "cancellation at the checkpoint yields the typed cancelled result and no "
                        "product");

    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

void testFailurePropagationTyped(Expectations& expectations) {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneSolidPlan();
    const auto evaluated = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(evaluated.frame() != nullptr, "the fixture evaluates for the failure test");
    if (evaluated.frame() == nullptr) {
        return;
    }
    auto handle = buildNeutralHandle();
    expectations.expect(handle.has_value(), "the handle builds for the failure test");
    if (!handle.has_value()) {
        return;
    }

    const bloom::runtime::CpuQualifiedDisplayPreparer preparer(*handle);
    const bloom::runtime::QualifiedDisplayPreparationRequest starvedRequest{
        .aggregatePixelStorageByteLimit = 1,
        .viewAdjust = {},
        .displayName = {},
        .viewName = {},
        .showLook = true,
    };
    const auto result = preparer.prepare(evaluated.frame(), starvedRequest, {});
    expectations.expect(
        result.status() == bloom::runtime::QualifiedDisplayPreparationStatus::Failed &&
            result.frame() == nullptr && !result.diagnostics().empty() &&
            result.diagnostics().front().code ==
                bloom::runtime::QualifiedDisplayDiagnosticCode::PixelStorageBudgetExceeded,
        "an impossible pixel-storage budget fails typed with no product");

    const bloom::runtime::QualifiedDisplayPreparationRequest invalidRequest{
        .aggregatePixelStorageByteLimit = 0,
        .viewAdjust = {},
        .displayName = {},
        .viewName = {},
        .showLook = true,
    };
    const auto invalidResult = preparer.prepare(evaluated.frame(), invalidRequest, {});
    expectations.expect(
        invalidResult.status() == bloom::runtime::QualifiedDisplayPreparationStatus::Failed &&
            invalidResult.frame() == nullptr && !invalidResult.diagnostics().empty() &&
            invalidResult.diagnostics().front().code ==
                bloom::runtime::QualifiedDisplayDiagnosticCode::InvalidRequest,
        "a zero pixel-storage budget is rejected as an invalid request");
}

void testViewAdjust(Expectations& expectations) {
    using namespace bloom;
    const auto plan = oneSolidPlan(format(), {0.125, 0.25, 0.5, 0.5});
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(plan, requestFor(*plan), {});
    auto handle = buildNeutralHandle();
    expectations.expect(evaluated.frame() && handle, "adjust fixture evaluates and qualifies");
    if (!evaluated.frame() || !handle)
        return;
    const runtime::CpuQualifiedDisplayPreparer qualified(*handle);
    const runtime::CpuReferenceDisplayPreparer reference;
    constexpr std::size_t budget = 1U << 20U;
    const auto original = reference.prepare(evaluated.frame(),
                                            {.aggregatePixelStorageByteLimit = budget,
                                             .viewAdjust = {},
                                             .displayName = {},
                                             .viewName = {},
                                             .showLook = true},
                                            {});
    const auto qualifiedOriginal = qualified.prepare(evaluated.frame(),
                                                     {.aggregatePixelStorageByteLimit = budget,
                                                      .viewAdjust = {},
                                                      .displayName = {},
                                                      .viewName = {},
                                                      .showLook = true},
                                                     {});
    const auto encode = [](double value) {
        return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    };
    expectations.expect(runtime::ViewAdjust{1, 1}.linearExposure(0.125) == 0.25,
                        "one EV doubles linear display light before encoding");
    for (const runtime::ViewAdjust adjust : {runtime::ViewAdjust{}, {1, 1}, {0, 2}, {-1, 0.5}}) {
        const auto a = reference.prepare(evaluated.frame(),
                                         {.aggregatePixelStorageByteLimit = budget,
                                          .viewAdjust = adjust,
                                          .displayName = {},
                                          .viewName = {},
                                          .showLook = true},
                                         {});
        const auto b = qualified.prepare(evaluated.frame(),
                                         {.aggregatePixelStorageByteLimit = budget,
                                          .viewAdjust = adjust,
                                          .displayName = {},
                                          .viewName = {},
                                          .showLook = true},
                                         {});
        expectations.expect(a.frame() && b.frame(), "both adjusted display paths prepare");
        if (!a.frame() || !b.frame())
            continue;
        const auto wrapped = runtime::PreparedPreviewFrame::createQualified(1, b.frame());
        expectations.expect(wrapped.has_value(), "adjusted qualified frame wraps for preview");
        if (!wrapped)
            continue;
        const auto view = wrapped->displayBufferView();
        expectations.expect(view.has_value(), "adjusted preview has a display buffer");
        if (!view)
            continue;
        const auto pixels = view->pixels;
        const auto expected = static_cast<std::uint8_t>(std::floor(
            std::pow(encode(0.125 * std::exp2(adjust.exposure)), 1 / adjust.gamma) * 255 + 0.5));
        expectations.expect(a.frame()->buffer().pixels()[0].red == expected &&
                                pixels[0].red == expected && pixels[0].alpha == 128,
                            "exposure precedes encoding, gamma follows it and alpha is unchanged");
        expectations.expect(a.frame()->processFrame() == evaluated.frame() &&
                                b.frame()->processFrame() == evaluated.frame(),
                            "adjustments preserve the exact reference process frame");
        if (adjust.neutral()) {
            expectations.expect(
                std::ranges::equal(original.frame()->buffer().pixels(),
                                   a.frame()->buffer().pixels()) &&
                    std::ranges::equal(qualifiedOriginal.frame()->buffer().pixels(), pixels) &&
                    !b.frame()->adjustedBuffer(),
                "neutral settings bypass adjustment and keep both original buffers bit-identical");
        }
    }
    auto roiRequest = requestFor(*plan);
    const auto region = render::ImageWindow::create(1, 0, 1, 1);
    roiRequest.roi = *region.value();
    const auto sampled = evaluator.evaluate(plan, roiRequest, {});
    expectations.expect(sampled.frame() && sampled.frame()->processImage().pixels().size() == 1,
                        "1x1 ROI retains one reference process pixel");
    if (sampled.frame()) {
        const auto a = reference.prepare(sampled.frame(),
                                         {.aggregatePixelStorageByteLimit = budget,
                                          .viewAdjust = {},
                                          .displayName = {},
                                          .viewName = {},
                                          .showLook = true},
                                         {});
        const auto b = qualified.prepare(sampled.frame(),
                                         {.aggregatePixelStorageByteLimit = budget,
                                          .viewAdjust = {},
                                          .displayName = {},
                                          .viewName = {},
                                          .showLook = true},
                                         {});
        expectations.expect(a.frame() && b.frame(),
                            "ROI display preparation succeeds in both paths");
        if (a.frame() && b.frame()) {
            const auto wrapped = runtime::PreparedPreviewFrame::createQualified(1, b.frame());
            expectations.expect(wrapped.has_value(), "ROI qualified frame wraps for preview");
            if (!wrapped)
                return;
            const auto view = wrapped->displayBufferView();
            expectations.expect(view.has_value(), "ROI preview has a display buffer");
            if (!view)
                return;
            expectations.expect(
                view->displayWindow ==
                        evaluated.frame()->processImage().descriptor()->displayWindow() &&
                    view->pixels[0] == Rgba8{} &&
                    view->pixels[1] == qualifiedOriginal.frame()->buffer().pixels()[1] &&
                    std::ranges::equal(a.frame()->buffer().pixels(), view->pixels),
                "ROI preserves the full viewport and pads uncomputed pixels in both display paths");
        }
    }
    const auto invalid = reference.prepare(evaluated.frame(),
                                           {.aggregatePixelStorageByteLimit = budget,
                                            .viewAdjust = {0, 0},
                                            .displayName = {},
                                            .viewName = {},
                                            .showLook = true},
                                           {});
    expectations.expect(!invalid.frame(), "invalid gamma is rejected before display work");
    const auto starved = qualified.prepare(evaluated.frame(),
                                           {.aggregatePixelStorageByteLimit = 200,
                                            .viewAdjust = {1, 1},
                                            .displayName = {},
                                            .viewName = {},
                                            .showLook = true},
                                           {});
    expectations.expect(!starved.frame(),
                        "adjusted qualified storage obeys the aggregate byte budget");
}

} // namespace

int main() {
    Expectations expectations;
    testViewAdjust(expectations);
    testGoldenMatchesC2DirectOutput(expectations);
    testCancellationAtChunkBoundaryPublishesNothing(expectations);
    testFailurePropagationTyped(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
