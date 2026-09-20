#ifndef BLOOM_RUNTIME_TESTS_GPU_AFFINE_BLEND_EXECUTOR_FAULT_FIXTURES_HPP
#define BLOOM_RUNTIME_TESTS_GPU_AFFINE_BLEND_EXECUTOR_FAULT_FIXTURES_HPP

// Test-only affine/blend fault fixtures for the production GpuSceneExecutor. Self-contained and
// included at reference scope (NOT inside the test's anonymous namespace); everything lives in
// namespace bloom::runtime::tests::affine_blend_fault with internal linkage helpers. All bodies are
// compiled only under BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION; the default build sees no-op
// functions, so the production and non-fault test paths are unchanged.

#include "gpu_blend_native_support.hpp"
#include "gpu_composite_native_support.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

// Proof-only fixture seam: can call the private PreparedGpuScene constructor. Defined only in the
// test translation unit, so production code cannot construct a mutable scene this way.
struct GpuSceneFixtureBuilder final {
    [[nodiscard]] static std::shared_ptr<const PreparedGpuScene>
    make(std::vector<GpuSceneCommand> commands, const GpuSceneCommandIndex output,
         const render::Rgba32fImageDescriptor outputDescriptor) {
        ProcessFrameIdentity identity{
            .plan = nullptr,
            .time = {},
            .output = OperationIndex::fromRaw(0),
            .resolution = {},
            .quality = EvaluationQuality::Reference,
            .colorIntent = EvaluationColorIntent::LinearRec709Scene,
            .provider = EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = 0,
            .animationSamplingSemanticsVersion = 0,
            .imagePrimitiveSemanticsVersion = 0,
            .roi = std::nullopt,
            .bypassLookNodes = false,
        };
        return std::shared_ptr<const PreparedGpuScene>(new PreparedGpuScene(
            std::move(commands), {}, output, std::move(identity), {}, outputDescriptor, {}));
    }
};

} // namespace bloom::runtime


namespace bloom::runtime::tests::affine_blend_fault {

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
namespace {

using bloom::core::BlendMode;
using bloom::render::blend_proof::blendPixels;
using bloom::render::composite_proof::denseHdrPixels;
using bloom::render::composite_proof::Expectations;
using bloom::render::composite_proof::GpuDevice;
using bloom::render::composite_proof::kBudget;
using bloom::render::composite_proof::makeImage;
using bloom::render::composite_proof::PixelAspectRatio;
using bloom::render::composite_proof::Rgba32f;
using bloom::render::composite_proof::Rgba32fImage;
using bloom::render::composite_proof::window;
using bloom::render::ImageWindow;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuSceneAffineCommand;
using bloom::runtime::GpuSceneBlendCommand;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneCommandIndex;
using bloom::runtime::GpuSceneCompositionOutputCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneFixtureBuilder;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::OperationIndex;
using bloom::runtime::detail::LayerMatrix;
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
using bloom::render::gpu_scene_executor_fault::PollFault;
#endif
using bloom::runtime::detail::ParentedLayerTransform;

[[nodiscard]] Rgba32fImageDescriptor descriptorFor(const ImageWindow data,
                                                   const ImageWindow display,
                                                   const PixelAspectRatio aspect) {
    const auto created = Rgba32fImageDescriptor::create(data, display, aspect);
    return *created.value();
}

[[nodiscard]] GpuSceneUploadCommand uploadCommand(const GpuSceneCommandIndex index,
                                                  const std::shared_ptr<const Rgba32fImage>& image,
                                                  std::string key) {
    return GpuSceneUploadCommand{.index = index,
                                 .sourceOperation = OperationIndex::fromRaw(0),
                                 .image = image,
                                 .descriptor = *image->descriptor(),
                                 .semanticKey = std::move(key)};
}

[[nodiscard]] GpuSceneCompositionOutputCommand outputCommand(const GpuSceneCommandIndex index,
                                                             const GpuSceneCommandIndex input,
                                                             const ImageWindow data,
                                                             const ImageWindow display,
                                                             const PixelAspectRatio aspect,
                                                             std::string key) {
    return GpuSceneCompositionOutputCommand{.index = index,
                                             .sourceOperation = OperationIndex::fromRaw(0),
                                             .input = input,
                                             .dataWindow = data,
                                             .displayWindow = display,
                                             .pixelAspect = aspect,
                                             .semanticKey = std::move(key)};
}

[[nodiscard]] bloom::render::GpuAffineMatrix toGpuAffineMatrix(const LayerMatrix& matrix,
                                                               const ImageWindow sourceWindow) {
    const double ox = static_cast<double>(sourceWindow.originX()) + 0.5;
    const double oy = static_cast<double>(sourceWindow.originY()) + 0.5;
    return bloom::render::GpuAffineMatrix{.a = matrix.a,
                                          .b = matrix.b,
                                          .tx = matrix.a * ox + matrix.b * oy + matrix.x - 0.5,
                                          .c = matrix.c,
                                          .d = matrix.d,
                                          .ty = matrix.c * ox + matrix.d * oy + matrix.y - 0.5};
}

struct RunOutcome final {
    GpuSceneExecutorPollResult pollResult = GpuSceneExecutorPollResult::Failure;
};

[[nodiscard]] RunOutcome
runScene(GpuSceneExecutor& executor, const std::shared_ptr<const PreparedGpuScene>& scene,
         const std::uint64_t budget) {
    RunOutcome outcome;
    if (executor.begin(scene, budget).code != GpuSceneExecutorDiagnosticCode::None) {
        return outcome;
    }
    for (;;) {
        const auto poll = executor.poll();
        if (poll != GpuSceneExecutorPollResult::Pending) {
            outcome.pollResult = poll;
            break;
        }
    }
    if (outcome.pollResult == GpuSceneExecutorPollResult::Ready) {
        static_cast<void>(executor.takeImage());
    }
    return outcome;
}

// Dispatches the next native op and returns once it is in flight (Pending), or the terminal result.
[[nodiscard]] GpuSceneExecutorPollResult dispatchOnce(GpuSceneExecutor& executor) {
    return executor.poll();
}

} // namespace
#endif // BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION

inline void runAffineFaults(
    [[maybe_unused]] bloom::render::composite_proof::Expectations& expectations,
    [[maybe_unused]] bloom::render::composite_proof::GpuDevice& device,
    [[maybe_unused]] bloom::runtime::GpuSceneCache& cache) {
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    const auto sourceWindow = window(2, 3, 18, 12);
    const auto aspect = PixelAspectRatio::square();
    auto image = makeImage(sourceWindow, sourceWindow, aspect, denseHdrPixels(18, 12));
    expectations.expect(image.has_value(), "affine-fault: source");
    if (!image) {
        return;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
    const auto outputWindow = window(-16, -16, 48, 48);
    const auto makeScene = [&](const LayerMatrix& matrix, const std::string& suffix) {
        const GpuSceneAffineCommand affine{.index = 1,
                                           .sourceOperation = OperationIndex::fromRaw(0),
                                           .input = 0,
                                           .inputKey = "fu-src",
                                           .sourceWindow = sourceWindow,
                                           .outputWindow = outputWindow,
                                           .matrix = toGpuAffineMatrix(matrix, sourceWindow),
                                           .opacity = 1.0F,
                                           .pixelAspect = aspect,
                                           .semanticKey = "advisory-" + suffix};
        std::vector<GpuSceneCommand> commands;
        commands.emplace_back(uploadCommand(0, source, "fu-src"));
        commands.emplace_back(affine);
        commands.emplace_back(
            outputCommand(2, 1, outputWindow, sourceWindow, aspect, "fu-out-" + suffix));
        return GpuSceneFixtureBuilder::make(std::move(commands), 2,
                                            descriptorFor(outputWindow, sourceWindow, aspect));
    };
    const auto authored = [](const double sx, const double sy) {
        return LayerMatrix::authored({2.0, 1.0}, {0.5, 0.5}, {sx, sy}, 15.0);
    };
    const auto sceneWarm = makeScene(authored(1.1, 0.9), "warm");
    const auto sceneStall = makeScene(authored(1.2, 0.9), "stall");
    const auto sceneUnknown = makeScene(authored(1.3, 0.9), "unknown");
    const auto sceneLost = makeScene(authored(1.4, 0.9), "lost");
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "affine-fault: executor");
    if (!executor) {
        return;
    }
    const std::uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    expectations.expect(runScene(*executor.executor, sceneWarm, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "affine-fault: warm ready");

    // StallPending: real submitted affine job stalls -> mid-native cancel, no publication, no cache.
    bloom::render::gpu_scene_executor_fault::set(PollFault::StallPending);
    expectations.expect(executor.executor->begin(sceneStall, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "affine-fault: stall begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "affine-fault: stall dispatched pending");
    expectations.expect(executor.executor->counters().affineDispatches == 2,
                        "affine-fault: stall positive dispatch");
    executor.executor->cancel();
    bloom::render::gpu_scene_executor_fault::clear();
    auto stallFinal = executor.executor->poll();
    while (stallFinal == GpuSceneExecutorPollResult::Pending) {
        stallFinal = executor.executor->poll();
    }
    expectations.expect(stallFinal == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                        "affine-fault: stall cancelled mid-native");
    expectations.expect(executor.executor->image() == nullptr, "affine-fault: stall no output");
    const auto afterStall = executor.executor->counters().affineDispatches;
    expectations.expect(runScene(*executor.executor, sceneStall, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "affine-fault: stall rerun ready");
    expectations.expect(executor.executor->counters().affineDispatches == afterStall + 1,
                        "affine-fault: stall no cached failed output");

    // UnknownFence: unproven retirement -> owner drain required, then proven retirement, no reuse.
    bloom::render::gpu_scene_executor_fault::set(PollFault::UnknownFence);
    expectations.expect(executor.executor->begin(sceneUnknown, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "affine-fault: unknown begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "affine-fault: unknown dispatched pending");
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Failure,
                        "affine-fault: unknown fails");
    expectations.expect(executor.executor->diagnostic().code ==
                            GpuSceneExecutorDiagnosticCode::NativeUnproven,
                        "affine-fault: unknown NativeUnproven");
    expectations.expect(executor.executor->ownerDrainRequired(), "affine-fault: unknown drain gate");
    expectations.expect(executor.executor->begin(sceneUnknown, budget).code ==
                            GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                        "affine-fault: unknown begin refused while draining");
    bloom::render::gpu_scene_executor_fault::clear();
    for (int i = 0; i < 100000 && executor.executor->ownerDrainRequired(); ++i) {
        static_cast<void>(executor.executor->poll());
    }
    expectations.expect(!executor.executor->ownerDrainRequired(),
                        "affine-fault: unknown drain proves retirement");
    const auto afterUnknown = executor.executor->counters().affineDispatches;
    expectations.expect(runScene(*executor.executor, sceneUnknown, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "affine-fault: unknown rerun ready");
    expectations.expect(executor.executor->counters().affineDispatches == afterUnknown + 1,
                        "affine-fault: unknown no cached failed output");

    // DeviceLost: injected loss after proven retirement; the executor is terminal.
    bloom::render::gpu_scene_executor_fault::set(PollFault::DeviceLost);
    expectations.expect(executor.executor->begin(sceneLost, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "affine-fault: lost begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "affine-fault: lost dispatched pending");
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Failure,
                        "affine-fault: lost fails");
    expectations.expect(executor.executor->diagnostic().code ==
                            GpuSceneExecutorDiagnosticCode::DeviceLost,
                        "affine-fault: DeviceLost");
    expectations.expect(executor.executor->deviceLost(), "affine-fault: terminal after loss");
    expectations.expect(executor.executor->image() == nullptr, "affine-fault: lost no output");
    expectations.expect(executor.executor->begin(sceneLost, budget).code ==
                            GpuSceneExecutorDiagnosticCode::DeviceLost,
                        "affine-fault: begin refused after loss");
#endif
}

inline void runBlendFaults(
    [[maybe_unused]] bloom::render::composite_proof::Expectations& expectations,
    [[maybe_unused]] bloom::render::composite_proof::GpuDevice& device,
    [[maybe_unused]] bloom::runtime::GpuSceneCache& cache) {
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    const auto geometry = window(0, 0, 16, 12);
    const auto aspect = PixelAspectRatio::square();
    auto sourceImage = makeImage(geometry, geometry, aspect, blendPixels(16, 12, true));
    auto destImage = makeImage(geometry, geometry, aspect, blendPixels(16, 12, false));
    expectations.expect(sourceImage.has_value() && destImage.has_value(), "blend-fault: fixtures");
    if (!sourceImage || !destImage) {
        return;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*sourceImage));
    auto destination = std::make_shared<const Rgba32fImage>(std::move(*destImage));
    const auto makeScene = [&](const BlendMode mode, const std::string& suffix) {
        const GpuSceneBlendCommand blend{.index = 2,
                                         .sourceOperation = OperationIndex::fromRaw(0),
                                         .source = 0,
                                         .destination = 1,
                                         .sourceKey = "bf-src",
                                         .destinationKey = "bf-dst",
                                         .mode = mode,
                                         .sourceWindow = geometry,
                                         .outputWindow = geometry,
                                         .pixelAspect = aspect,
                                         .artifactDigest = "adversarially-wrong-digest",
                                         .semanticKey = "advisory-" + suffix};
        std::vector<GpuSceneCommand> commands;
        commands.emplace_back(uploadCommand(0, source, "bf-src"));
        commands.emplace_back(uploadCommand(1, destination, "bf-dst"));
        commands.emplace_back(blend);
        commands.emplace_back(outputCommand(3, 2, geometry, geometry, aspect, "bf-out-" + suffix));
        return GpuSceneFixtureBuilder::make(std::move(commands), 3,
                                            descriptorFor(geometry, geometry, aspect));
    };
    const auto sceneWarm = makeScene(BlendMode::Normal, "warm");
    const auto sceneStall = makeScene(BlendMode::Add, "stall");
    const auto sceneUnknown = makeScene(BlendMode::Multiply, "unknown");
    const auto sceneLost = makeScene(BlendMode::Screen, "lost");
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "blend-fault: executor");
    if (!executor) {
        return;
    }
    const std::uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    expectations.expect(runScene(*executor.executor, sceneWarm, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "blend-fault: warm ready");

    bloom::render::gpu_scene_executor_fault::set(PollFault::StallPending);
    expectations.expect(executor.executor->begin(sceneStall, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "blend-fault: stall begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "blend-fault: stall dispatched pending");
    expectations.expect(executor.executor->counters().blendDispatches == 2,
                        "blend-fault: stall positive dispatch");
    executor.executor->cancel();
    bloom::render::gpu_scene_executor_fault::clear();
    auto stallFinal = executor.executor->poll();
    while (stallFinal == GpuSceneExecutorPollResult::Pending) {
        stallFinal = executor.executor->poll();
    }
    expectations.expect(stallFinal == GpuSceneExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                        "blend-fault: stall cancelled mid-native");
    expectations.expect(executor.executor->image() == nullptr, "blend-fault: stall no output");
    const auto afterStall = executor.executor->counters().blendDispatches;
    expectations.expect(runScene(*executor.executor, sceneStall, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "blend-fault: stall rerun ready");
    expectations.expect(executor.executor->counters().blendDispatches == afterStall + 1,
                        "blend-fault: stall no cached failed output");

    bloom::render::gpu_scene_executor_fault::set(PollFault::UnknownFence);
    expectations.expect(executor.executor->begin(sceneUnknown, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "blend-fault: unknown begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "blend-fault: unknown dispatched pending");
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Failure,
                        "blend-fault: unknown fails");
    expectations.expect(executor.executor->diagnostic().code ==
                            GpuSceneExecutorDiagnosticCode::NativeUnproven,
                        "blend-fault: unknown NativeUnproven");
    expectations.expect(executor.executor->ownerDrainRequired(), "blend-fault: unknown drain gate");
    expectations.expect(executor.executor->begin(sceneUnknown, budget).code ==
                            GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                        "blend-fault: unknown begin refused while draining");
    bloom::render::gpu_scene_executor_fault::clear();
    for (int i = 0; i < 100000 && executor.executor->ownerDrainRequired(); ++i) {
        static_cast<void>(executor.executor->poll());
    }
    expectations.expect(!executor.executor->ownerDrainRequired(),
                        "blend-fault: unknown drain proves retirement");
    const auto afterUnknown = executor.executor->counters().blendDispatches;
    expectations.expect(runScene(*executor.executor, sceneUnknown, budget).pollResult ==
                            GpuSceneExecutorPollResult::Ready,
                        "blend-fault: unknown rerun ready");
    expectations.expect(executor.executor->counters().blendDispatches == afterUnknown + 1,
                        "blend-fault: unknown no cached failed output");

    bloom::render::gpu_scene_executor_fault::set(PollFault::DeviceLost);
    expectations.expect(executor.executor->begin(sceneLost, budget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "blend-fault: lost begin");
    expectations.expect(dispatchOnce(*executor.executor) == GpuSceneExecutorPollResult::Pending,
                        "blend-fault: lost dispatched pending");
    expectations.expect(executor.executor->poll() == GpuSceneExecutorPollResult::Failure,
                        "blend-fault: lost fails");
    expectations.expect(executor.executor->diagnostic().code ==
                            GpuSceneExecutorDiagnosticCode::DeviceLost,
                        "blend-fault: DeviceLost");
    expectations.expect(executor.executor->deviceLost(), "blend-fault: terminal after loss");
    expectations.expect(executor.executor->image() == nullptr, "blend-fault: lost no output");
    expectations.expect(executor.executor->begin(sceneLost, budget).code ==
                            GpuSceneExecutorDiagnosticCode::DeviceLost,
                        "blend-fault: begin refused after loss");
#endif
}

} // namespace bloom::runtime::tests::affine_blend_fault

#endif // BLOOM_RUNTIME_TESTS_GPU_AFFINE_BLEND_EXECUTOR_FAULT_FIXTURES_HPP
