// Real native proof that the production GpuSceneExecutor runs accepted GpuAffine and GpuBlend
// commands on a Vulkan device and matches the actual CPU oracle under the documented 2e-6
// absolute-or-relative gate. Readback happens only for oracle assertions.
//
// The scene is a deliberately controlled PreparedGpuScene built by the proof-only friend
// GpuSceneFixtureBuilder (declared in prepared_gpu_scene.hpp, defined here). No production mutable
// scene-creation API is exposed and CpuGpuSceneBuilder is untouched.
//
// Without a loader the test skips cleanly; --require-device makes that a failure.

#include "gpu_blend_native_support.hpp"
#include "gpu_composite_native_support.hpp"

#include "layer_parent_transform.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_affine_blend_executor_fault_fixtures.hpp"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;

using bloom::core::BlendMode;
using bloom::render::blendLinearRec709SceneRow;
using bloom::render::GpuImage;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::blend_proof::blendPixels;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationProvider;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::GpuSceneAffineCommand;
using bloom::runtime::GpuSceneBlendCommand;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneCommandIndex;
using bloom::runtime::GpuSceneCompositionOutputCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnostic;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneFixtureBuilder;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::OperationIndex;
using bloom::runtime::ProcessFrameIdentity;
using bloom::runtime::detail::LayerMatrix;
using bloom::runtime::detail::ParentedLayerTransform;

[[nodiscard]] Rgba32fImageDescriptor
descriptorFor(const ImageWindow data, const ImageWindow display, const PixelAspectRatio aspect) {
    const auto descriptor = Rgba32fImageDescriptor::create(data, display, aspect);
    return *descriptor.value();
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

[[nodiscard]] GpuSceneCompositionOutputCommand
outputCommand(const GpuSceneCommandIndex index, const GpuSceneCommandIndex input,
              const ImageWindow data, const ImageWindow display, const PixelAspectRatio aspect,
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

[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::vector<Rgba32f>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                return false;
            }
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale)) {
                return false;
            }
        }
    }
    return true;
}

struct RunOutcome final {
    GpuSceneExecutorDiagnostic beginDiagnostic;
    GpuSceneExecutorPollResult pollResult = GpuSceneExecutorPollResult::Failure;
    std::vector<Rgba32f> pixels;
};

// begin -> poll loop -> readback. If cancelMid is set, cancel() is issued after the first Pending.
[[nodiscard]] RunOutcome
runScene(GpuSceneExecutor& executor,
         const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene,
         const std::uint64_t budget, const bool cancelMid) {
    RunOutcome outcome;
    outcome.beginDiagnostic = executor.begin(scene, budget);
    if (outcome.beginDiagnostic.code != GpuSceneExecutorDiagnosticCode::None) {
        return outcome;
    }
    bool cancelIssued = false;
    for (;;) {
        const auto poll = executor.poll();
        if (poll == GpuSceneExecutorPollResult::Pending) {
            if (cancelMid && !cancelIssued) {
                executor.cancel();
                cancelIssued = true;
            }
            continue;
        }
        outcome.pollResult = poll;
        break;
    }
    if (outcome.pollResult == GpuSceneExecutorPollResult::Ready) {
        const auto* image = executor.image();
        if (image != nullptr) {
            const auto readback = readbackResidentImage(*image, kBudget);
            if (readback.hasValue()) {
                outcome.pixels = readback.pixels;
            }
        }
        static_cast<void>(executor.takeImage());
    }
    return outcome;
}

void runAffine(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const auto sourceWindow = window(3, -2, 24, 16);
    const auto display = sourceWindow;
    const auto aspect = PixelAspectRatio::square();
    const auto sourcePixels = denseHdrPixels(24, 16);
    auto sourceImage = makeImage(sourceWindow, display, aspect, sourcePixels);
    expectations.expect(sourceImage.has_value(), "affine: source builds");
    if (!sourceImage) {
        return;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*sourceImage));

    // A sheared composed parent*child matrix (rotation + nonuniform/negative scale both levels).
    const auto parent = LayerMatrix::authored({4.0, 2.0}, {1.5, -0.5}, {1.5, -0.75}, 30.0);
    const auto child = LayerMatrix::authored({-1.5, 3.25}, {0.25, 0.5}, {2.0, 0.5}, -15.0);
    const auto composed = parent.times(child);
    const ParentedLayerTransform parented(composed, sourceWindow, 1.0, 1.0, 0.8F);
    const auto bounds = parented.supportBounds(window(-96, -96, 192, 192));
    expectations.expect(bounds.has_value(), "affine: support bounds non-empty");
    if (!bounds) {
        return;
    }
    const auto outputWindow = *bounds;

    const GpuSceneAffineCommand affine{.index = 1,
                                       .sourceOperation = OperationIndex::fromRaw(0),
                                       .input = 0,
                                       .inputKey = "affine-src",
                                       .sourceWindow = sourceWindow,
                                       .outputWindow = outputWindow,
                                       .matrix = toGpuAffineMatrix(composed, sourceWindow),
                                       .opacity = 0.8F,
                                       .pixelAspect = aspect,
                                       .semanticKey = "advisory-ignored"};

    std::vector<GpuSceneCommand> commands;
    commands.emplace_back(uploadCommand(0, source, "affine-src"));
    commands.emplace_back(affine);
    commands.emplace_back(outputCommand(2, 1, outputWindow, display, aspect, "affine-out"));
    const auto scene = GpuSceneFixtureBuilder::make(std::move(commands), 2,
                                                    descriptorFor(outputWindow, display, aspect));

    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "affine: executor creates");
    if (!executor) {
        return;
    }
    const auto budget = 64ULL * 1024ULL * 1024ULL;
    const auto first = runScene(*executor.executor, scene, budget, false);
    expectations.expect(first.pollResult == GpuSceneExecutorPollResult::Ready,
                        "affine: completes: " + first.beginDiagnostic.message);
    if (first.pollResult == GpuSceneExecutorPollResult::Ready) {
        const auto counters = executor.executor->counters();
        expectations.expect(counters.affineDispatches == 1,
                            "affine cold: exactly one affine dispatch");
        expectations.expect(counters.blendDispatches == 0, "affine cold: zero blend dispatches");

        const auto view = source->view();
        if (view) {
            const auto outWidth = outputWindow.extent().width();
            const auto outHeight = outputWindow.extent().height();
            std::vector<Rgba32f> expected(static_cast<std::size_t>(outWidth) * outHeight,
                                          Rgba32f::transparent());
            for (std::uint32_t y = 0; y < outHeight; ++y) {
                auto row = std::span<Rgba32f>(expected).subspan(
                    static_cast<std::size_t>(y) * outWidth, outWidth);
                static_cast<void>(
                    parented.row(*view.value(), outputWindow,
                                 outputWindow.originY() + static_cast<std::int64_t>(y), row));
            }
            expectations.expect(parity(first.pixels, expected),
                                "affine: GPU matches ParentedLayerTransform oracle (2e-6)");
        }
    }

    // Warm, unchanged scene tree: zero dispatch of any family.
    const auto warm = runScene(*executor.executor, scene, budget, false);
    expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready, "affine warm: ready");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.affineDispatches == 1, "affine warm: no new affine dispatch");
    expectations.expect(warmCounters.translationDispatches == 1,
                        "affine warm: no new translation dispatch");
    expectations.expect(warmCounters.uploads == 1, "affine warm: no new upload");
    expectations.expect(warmCounters.commandCacheHits > 0, "affine warm: cache hit");

    // A tiny budget below the affine output image + metadata must be refused, never silently run.
    const auto tiny = runScene(*executor.executor, scene, 64, false);
    expectations.expect(tiny.pollResult == GpuSceneExecutorPollResult::Failure,
                        "affine tiny budget: refused");
    expectations.expect(tiny.beginDiagnostic.code == GpuSceneExecutorDiagnosticCode::OverBudget ||
                            executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::OverBudget,
                        "affine tiny budget: OverBudget (image + metadata)");
}

void runBlendMode(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
                  const BlendMode mode, const std::string& name) {
    const auto sourceWindow = window(0, 0, 16, 12);
    const auto destWindow = window(0, 0, 16, 12);
    const auto aspect = PixelAspectRatio::square();
    auto sourceImage = makeImage(sourceWindow, sourceWindow, aspect, blendPixels(16, 12, true));
    auto destImage = makeImage(destWindow, destWindow, aspect, blendPixels(16, 12, false));
    expectations.expect(sourceImage.has_value() && destImage.has_value(),
                        name + ": fixtures build");
    if (!sourceImage || !destImage) {
        return;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*sourceImage));
    auto destination = std::make_shared<const Rgba32fImage>(std::move(*destImage));

    const std::string tag = "blend-" + std::to_string(static_cast<unsigned>(mode));
    const GpuSceneBlendCommand blend{.index = 2,
                                     .sourceOperation = OperationIndex::fromRaw(0),
                                     .source = 0,
                                     .destination = 1,
                                     .sourceKey = "blend-src-" + tag,
                                     .destinationKey = "blend-dst-" + tag,
                                     .mode = mode,
                                     .sourceWindow = sourceWindow,
                                     .outputWindow = destWindow,
                                     .pixelAspect = aspect,
                                     .artifactDigest = "adversarially-wrong-digest",
                                     .semanticKey = "advisory-ignored"};

    std::vector<GpuSceneCommand> commands;
    commands.emplace_back(uploadCommand(0, source, "blend-src-" + tag));
    commands.emplace_back(uploadCommand(1, destination, "blend-dst-" + tag));
    commands.emplace_back(blend);
    commands.emplace_back(outputCommand(3, 2, destWindow, destWindow, aspect, "blend-out-" + tag));
    const auto scene = GpuSceneFixtureBuilder::make(std::move(commands), 3,
                                                    descriptorFor(destWindow, destWindow, aspect));

    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), name + ": executor creates");
    if (!executor) {
        return;
    }
    const auto result = runScene(*executor.executor, scene, 64ULL * 1024ULL * 1024ULL, false);
    // On a device without shaderFloat64 the six general modes are refused honestly.
    if (result.beginDiagnostic.code != GpuSceneExecutorDiagnosticCode::None) {
        expectations.expect(result.beginDiagnostic.code ==
                                GpuSceneExecutorDiagnosticCode::Unsupported,
                            name + ": only Unsupported may reject at begin");
        return;
    }
    if (result.pollResult != GpuSceneExecutorPollResult::Ready) {
        const auto code = executor.executor->diagnostic().code;
        const bool acceptable = (code == GpuSceneExecutorDiagnosticCode::Unsupported ||
                                 code == GpuSceneExecutorDiagnosticCode::DispatchRefused);
        expectations.expect(acceptable, name + ": general mode must run or refuse honestly (f64)");
        return;
    }
    const auto counters = executor.executor->counters();
    expectations.expect(counters.blendDispatches == 1, name + ": one blend dispatch");
    expectations.expect(counters.affineDispatches == 0, name + ": zero affine dispatches");

    // Oracle: blendLinearRec709SceneRow over the destination copy.
    const auto sourceView = source->view();
    const auto destView = destination->view();
    if (!sourceView || !destView) {
        return;
    }
    const auto width = destWindow.extent().width();
    const auto height = destWindow.extent().height();
    std::vector<Rgba32f> expected =
        std::vector<Rgba32f>(destView.value()->pixels().begin(), destView.value()->pixels().end());
    bool oracleOk = true;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto offset = static_cast<std::size_t>(y) * width;
        const auto status =
            blendLinearRec709SceneRow(mode, sourceView.value()->pixels().subspan(offset, width),
                                      std::span<Rgba32f>(expected).subspan(offset, width));
        if (status.has_value()) {
            oracleOk = false;
        }
    }
    expectations.expect(oracleOk, name + ": oracle rows succeed");
    expectations.expect(parity(result.pixels, expected),
                        name + ": GPU matches blend oracle (2e-6)");
}

void runBlendMatrix(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const auto modes = std::array<BlendMode, 8>{
        BlendMode::Normal,  BlendMode::Add,    BlendMode::Multiply, BlendMode::Screen,
        BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,  BlendMode::Difference};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        runBlendMode(expectations, device, cache, modes[i],
                     "blend-mode-" + std::to_string(static_cast<unsigned>(i)));
    }
}

void runAffineInvalidationAndCancel(Expectations& expectations, GpuDevice& device,
                                    GpuSceneCache& cache) {
    const auto sourceWindow = window(5, -1, 20, 14);
    const auto display = sourceWindow;
    const auto aspect = PixelAspectRatio::square();
    auto image = makeImage(sourceWindow, display, aspect, denseHdrPixels(20, 14));
    expectations.expect(image.has_value(), "affine-inval: source builds");
    if (!image) {
        return;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
    const auto outputWindow = window(-32, -32, 64, 64);
    const auto m1 = LayerMatrix::authored({3.0, 1.0}, {0.5, 0.5}, {1.25, 0.8}, 20.0);
    const auto m2 = LayerMatrix::authored({3.0, 1.0}, {0.5, 0.5}, {1.5, 0.8}, 20.0);
    const auto m3 = LayerMatrix::authored({3.0, 1.0}, {0.5, 0.5}, {1.75, 0.8}, 20.0);
    const auto makeScene = [&](const LayerMatrix& matrix, const std::string& suffix) {
        GpuSceneAffineCommand affine{.index = 1,
                                     .sourceOperation = OperationIndex::fromRaw(0),
                                     .input = 0,
                                     .inputKey = "inv-src",
                                     .sourceWindow = sourceWindow,
                                     .outputWindow = outputWindow,
                                     .matrix = toGpuAffineMatrix(matrix, sourceWindow),
                                     .opacity = 1.0F,
                                     .pixelAspect = aspect,
                                     .semanticKey = "advisory-" + suffix};
        std::vector<GpuSceneCommand> commands;
        commands.emplace_back(uploadCommand(0, source, "inv-src"));
        commands.emplace_back(affine);
        commands.emplace_back(
            outputCommand(2, 1, outputWindow, display, aspect, "inv-out-" + suffix));
        return GpuSceneFixtureBuilder::make(std::move(commands), 2,
                                            descriptorFor(outputWindow, display, aspect));
    };
    const auto sceneA = makeScene(m1, "a");
    const auto sceneB = makeScene(m2, "b");
    const auto sceneC = makeScene(m3, "c");

    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "affine-inval: executor creates");
    if (!executor) {
        return;
    }
    const std::uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    const auto first = runScene(*executor.executor, sceneA, budget, false);
    expectations.expect(first.pollResult == GpuSceneExecutorPollResult::Ready,
                        "affine-inval: scene A ready");
    auto counters = executor.executor->counters();
    const auto uploadsAfterA = counters.uploads;
    const auto affineAfterA = counters.affineDispatches;

    const auto second = runScene(*executor.executor, sceneB, budget, false);
    expectations.expect(second.pollResult == GpuSceneExecutorPollResult::Ready,
                        "affine-inval: scene B ready");
    counters = executor.executor->counters();
    expectations.expect(counters.uploads == uploadsAfterA,
                        "affine-inval: changed matrix preserves upstream upload cache");
    expectations.expect(counters.affineDispatches == affineAfterA + 1,
                        "affine-inval: changed matrix dirties only the affine descendant");

    // sceneC re-dispatches affine; the upload is cached, so affine is the very next step and one
    // poll leaves it genuinely in flight.
    const auto beforeCancel = executor.executor->counters().affineDispatches;
    const auto begin = executor.executor->begin(sceneC, budget);
    expectations.expect(begin.code == GpuSceneExecutorDiagnosticCode::None,
                        "affine-cancel: begin accepted");
    const auto firstPoll = executor.executor->poll();
    expectations.expect(firstPoll == GpuSceneExecutorPollResult::Pending,
                        "affine-cancel: affine is in flight after one poll");
    if (firstPoll == GpuSceneExecutorPollResult::Pending) {
        expectations.expect(executor.executor->counters().affineDispatches == beforeCancel + 1,
                            "affine-cancel: positive affine dispatch before cancel");
        executor.executor->cancel();
        auto finalPoll = executor.executor->poll();
        while (finalPoll == GpuSceneExecutorPollResult::Pending) {
            finalPoll = executor.executor->poll();
        }
        expectations.expect(finalPoll == GpuSceneExecutorPollResult::Failure &&
                                executor.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::Cancelled,
                            "affine-cancel: cancelled mid-native");
        expectations.expect(executor.executor->image() == nullptr,
                            "affine-cancel: no output published");
        const auto rerunBefore = executor.executor->counters().affineDispatches;
        const auto rerun = runScene(*executor.executor, sceneC, budget, false);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready,
                            "affine-cancel: rerun ready");
        expectations.expect(executor.executor->counters().affineDispatches == rerunBefore + 1,
                            "affine-cancel: cancelled job left no cache reuse");
    } else {
        static_cast<void>(executor.executor->takeImage());
    }
}

void runBlendInvalidationAndCancel(Expectations& expectations, GpuDevice& device,
                                   GpuSceneCache& cache) {
    const auto geometry = window(0, 0, 16, 12);
    const auto aspect = PixelAspectRatio::square();
    auto sourceImage = makeImage(geometry, geometry, aspect, blendPixels(16, 12, true));
    auto destImage = makeImage(geometry, geometry, aspect, blendPixels(16, 12, false));
    expectations.expect(sourceImage.has_value() && destImage.has_value(),
                        "blend-inval: fixtures build");
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
                                         .sourceKey = "bi-src",
                                         .destinationKey = "bi-dst",
                                         .mode = mode,
                                         .sourceWindow = geometry,
                                         .outputWindow = geometry,
                                         .pixelAspect = aspect,
                                         .artifactDigest = "adversarially-wrong-digest",
                                         .semanticKey = "advisory-" + suffix};
        std::vector<GpuSceneCommand> commands;
        commands.emplace_back(uploadCommand(0, source, "bi-src"));
        commands.emplace_back(uploadCommand(1, destination, "bi-dst"));
        commands.emplace_back(blend);
        commands.emplace_back(outputCommand(3, 2, geometry, geometry, aspect, "bi-out-" + suffix));
        return GpuSceneFixtureBuilder::make(std::move(commands), 3,
                                            descriptorFor(geometry, geometry, aspect));
    };
    const auto sceneA = makeScene(BlendMode::Normal, "a");
    const auto sceneB = makeScene(BlendMode::Add, "b");
    const auto sceneC = makeScene(BlendMode::Multiply, "c");

    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "blend-inval: executor creates");
    if (!executor) {
        return;
    }
    const std::uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    const auto first = runScene(*executor.executor, sceneA, budget, false);
    expectations.expect(first.pollResult == GpuSceneExecutorPollResult::Ready,
                        "blend-inval: scene A ready");
    auto counters = executor.executor->counters();
    const auto uploadsAfterA = counters.uploads;
    const auto blendAfterA = counters.blendDispatches;

    const auto second = runScene(*executor.executor, sceneB, budget, false);
    expectations.expect(second.pollResult == GpuSceneExecutorPollResult::Ready,
                        "blend-inval: scene B ready");
    counters = executor.executor->counters();
    expectations.expect(counters.uploads == uploadsAfterA,
                        "blend-inval: changed mode preserves upstream upload cache");
    expectations.expect(counters.blendDispatches == blendAfterA + 1,
                        "blend-inval: changed mode dirties only the blend descendant");

    const auto beforeCancel = executor.executor->counters().blendDispatches;
    const auto begin = executor.executor->begin(sceneC, budget);
    expectations.expect(begin.code == GpuSceneExecutorDiagnosticCode::None,
                        "blend-cancel: begin accepted");
    const auto firstPoll = executor.executor->poll();
    expectations.expect(firstPoll == GpuSceneExecutorPollResult::Pending,
                        "blend-cancel: blend is in flight after one poll");
    if (firstPoll == GpuSceneExecutorPollResult::Pending) {
        expectations.expect(executor.executor->counters().blendDispatches == beforeCancel + 1,
                            "blend-cancel: positive blend dispatch before cancel");
        executor.executor->cancel();
        auto finalPoll = executor.executor->poll();
        while (finalPoll == GpuSceneExecutorPollResult::Pending) {
            finalPoll = executor.executor->poll();
        }
        expectations.expect(finalPoll == GpuSceneExecutorPollResult::Failure &&
                                executor.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::Cancelled,
                            "blend-cancel: cancelled mid-native");
        expectations.expect(executor.executor->image() == nullptr,
                            "blend-cancel: no output published");
        const auto rerunBefore = executor.executor->counters().blendDispatches;
        const auto rerun = runScene(*executor.executor, sceneC, budget, false);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready,
                            "blend-cancel: rerun ready");
        expectations.expect(executor.executor->counters().blendDispatches == rerunBefore + 1,
                            "blend-cancel: cancelled job left no cache reuse");
    } else {
        static_cast<void>(executor.executor->takeImage());
    }
}

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
using bloom::render::gpu_scene_executor_fault::PollFault;
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        auto cache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "cache creates");
        if (!cache) {
            return 1;
        }
        // A second device instance mints a distinct ownership epoch even on the same physical GPU;
        // the executor must refuse a cache bound to it before touching any native resource.
        auto foreign = GpuDevice::create(createOptions);
        if (foreign) {
            auto foreignCache = GpuSceneCache::create(*foreign.device, GpuSceneCacheBudgets{});
            expectations.expect(foreignCache.hasValue(), "foreign: cache creates");
            if (foreignCache) {
                auto refused = GpuSceneExecutor::create(*device.device, *foreignCache.cache);
                expectations.expect(!refused.hasValue(),
                                    "foreign: executor refuses a foreign-device cache");
                expectations.expect(refused.diagnostic.code ==
                                        GpuSceneExecutorDiagnosticCode::InvalidArgument,
                                    "foreign: refusal is InvalidArgument");
            }
        }
        runAffine(expectations, *device.device, *cache.cache);
        runBlendMatrix(expectations, *device.device, *cache.cache);
        runAffineInvalidationAndCancel(expectations, *device.device, *cache.cache);
        runBlendInvalidationAndCancel(expectations, *device.device, *cache.cache);
        bloom::runtime::tests::affine_blend_fault::runAffineFaults(expectations, *device.device,
                                                                   *cache.cache);
        bloom::runtime::tests::affine_blend_fault::runBlendFaults(expectations, *device.device,
                                                                  *cache.cache);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures()
                      << " executor affine/blend expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU scene executor affine + blend vs CPU oracle\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
