#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_LEAF_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_LEAF_EMISSION_HPP

// Private to src/runtime. The self-contained leaf command emissions moved out of the builder
// orchestrator to keep cpu_gpu_scene_preparation.cpp within its source-size budget: the SolidV1
// solid command and the terminal Composition Output. Both are templated on the builder's
// emit/charge closures so no std::function or shared state is introduced; the builder still owns
// the command vector, the per-operation maps, the running budget, and the coverage cache.

#include "gpu_scene_effect_emission.hpp"
#include "checker_image.hpp"
#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitSolidCommand(
    const CompiledSolid& solid, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const std::size_t index,
    const OperationIndex operationIndex, const render::ImageWindow fullWindow,
    const render::ImageWindow fullDisplayWindow, const core::PixelAspectRatio fullPixelAspect,
    const double hScale, const double vScale, Emit&& emit, Charge&& charge,
    std::vector<GpuSceneCommandIndex>& commandForOperation, std::vector<std::string>& keyOf,
    std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
    std::vector<EvaluatedOperationBounds>& bounds,
    std::vector<std::optional<GpuSceneVectorChain>>& vectors) {
    const auto color = resolveParameter(solid.color, plan, resolved);
    const auto width = resolveParameter(solid.width, plan, resolved);
    const auto height = resolveParameter(solid.height, plan, resolved);
    if (!color || !width || !height || width->value < 1.0 || height->value < 1.0) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Solid parameters are not evaluable");
    }
    const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color->value);
    if (!pixel) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Solid colour is not evaluable");
    }
    const double authorWidth = static_cast<double>(plan.format().width());
    const double authorHeight = static_cast<double>(plan.format().height());
    double exactWidth = static_cast<double>(fullWindow.extent().width());
    double exactHeight = static_cast<double>(fullWindow.extent().height());
    if (width->value != authorWidth) {
        exactWidth = width->value * hScale;
    }
    if (height->value != authorHeight) {
        exactHeight = height->value * vScale;
    }
    const auto w = std::ceil(exactWidth);
    const auto h = std::ceil(exactHeight);
    if (!std::isfinite(w) || !std::isfinite(h) || w > 16777216.0 || h > 16777216.0) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Solid dimensions exceed the supported extent");
    }
    const auto window = render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(w),
                                                    static_cast<std::uint64_t>(h));
    if (!window) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Solid bounds are invalid");
    }
    const auto descriptor =
        render::Rgba32fImageDescriptor::create(*window.value(), fullDisplayWindow, fullPixelAspect);
    if (!descriptor) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Solid descriptor is invalid");
    }
    const auto solidWindow = descriptor.value()->dataWindow();
    const auto solidDisplay = descriptor.value()->displayWindow();
    const auto solidAspect = descriptor.value()->pixelAspect();
    auto& entry = bounds[index];
    entry.local = boundsForWindow(solidWindow, hScale, vScale);
    entry.local.right = width->value;
    entry.local.bottom = height->value;
    entry.output = entry.local;
    outputWindowOf[index] = solidWindow;

    OperationKey key;
    key.add(std::string{"gpu-solid-v1"});
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
    addWindowToKey(key, solidWindow);
    addWindowToKey(key, solidDisplay);
    addPixelAspectToKey(key, solidAspect);
    key.add(std::string{kGpuSolidSpirvSha256});
    keyOf[index] = key.digest();

    if (const auto error = charge(solidWindow.extent().width(), solidWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneSolidCommand command{.sourceOperation = operationIndex,
                                 .pixel = *pixel.value(),
                                 .dataWindow = solidWindow,
                                 .displayWindow = solidDisplay,
                                 .pixelAspect = solidAspect,
                                 .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(command));
    vectors[index] = GpuSceneVectorChain{index, {}, 1.0};
    return std::nullopt;
}

template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitCheckerCommand(
    const CompiledChecker& checker, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const std::size_t index,
    const OperationIndex operationIndex, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const double hScale, const double vScale,
    Emit&& emit, Charge&& charge, std::vector<GpuSceneCommandIndex>& commandForOperation,
    std::vector<std::string>& keyOf,
    std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
    std::vector<EvaluatedOperationBounds>& bounds) {
    const auto colorA = resolveParameter(checker.colorA, plan, resolved);
    const auto colorB = resolveParameter(checker.colorB, plan, resolved);
    if (!colorA || !colorB)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Checker colors could not be resolved");
    const auto a = render::solidPixelFromStraightLinearRec709Scene(colorA->value);
    const auto b = render::solidPixelFromStraightLinearRec709Scene(colorB->value);
    if (!a || !b)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Checker colors are not evaluable");
    const auto proxyWidth = static_cast<std::uint32_t>(fullDisplayWindow.extent().width());
    const auto proxyHeight = static_cast<std::uint32_t>(fullDisplayWindow.extent().height());
    const CheckerGeometry geometry{checker.width, checker.height, checker.cellWidth,
                                   checker.cellHeight, plan.format().width(),
                                   plan.format().height(), proxyWidth, proxyHeight};
    if (!geometry.valid())
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Checker geometry exceeds supported bounds");
    for (const auto value : {geometry.authorWidth, geometry.authorHeight, geometry.cellWidth,
                             geometry.cellHeight, geometry.compositionWidth,
                             geometry.compositionHeight, geometry.proxyWidth,
                             geometry.proxyHeight})
        if (value > kCheckerMaximumNativeDimension)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Checker native dimensions exceed 16384; CPU fallback is required");
    const auto width = (std::uint64_t{checker.width} * proxyWidth +
                        geometry.compositionWidth - 1) / geometry.compositionWidth;
    const auto height = (std::uint64_t{checker.height} * proxyHeight +
                         geometry.compositionHeight - 1) / geometry.compositionHeight;
    if (width == 0 || height == 0 || width > kCheckerMaximumNativeDimension ||
        height > kCheckerMaximumNativeDimension)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Checker native output exceeds 16384; CPU fallback is required");
    const auto window = render::ImageWindow::create(0, 0, width, height);
    if (!window)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Checker bounds are invalid");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), fullDisplayWindow, fullPixelAspect);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Checker descriptor is invalid");
    const auto dataWindow = descriptor.value()->dataWindow();
    auto& entry = bounds[index];
    entry.local = boundsForWindow(dataWindow, hScale, vScale);
    entry.local.right = checker.width;
    entry.local.bottom = checker.height;
    entry.output = entry.local;
    outputWindowOf[index] = dataWindow;
    OperationKey key;
    key.add(std::string{"gpu-checker-v1"});
    for (const auto pixel : {*a.value(), *b.value()}) {
        key.add(std::bit_cast<std::uint32_t>(pixel.red()));
        key.add(std::bit_cast<std::uint32_t>(pixel.green()));
        key.add(std::bit_cast<std::uint32_t>(pixel.blue()));
        key.add(std::bit_cast<std::uint32_t>(pixel.alpha()));
    }
    for (const auto value : {checker.width, checker.height, checker.cellWidth,
                             checker.cellHeight, geometry.compositionWidth,
                             geometry.compositionHeight, proxyWidth, proxyHeight})
        key.add(value);
    addWindowToKey(key, dataWindow);
    addWindowToKey(key, descriptor.value()->displayWindow());
    addPixelAspectToKey(key, fullPixelAspect);
    key.add(std::string{kGpuCheckerSpirvSha256});
    keyOf[index] = key.digest();
    if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
        return error;
    GpuSceneSolidCommand command{.sourceOperation = operationIndex,
                                 .pixel = *a.value(),
                                 .checker = render::GpuSolidParameters::Checker{
                                     *b.value(), checker.width, checker.height,
                                     checker.cellWidth, checker.cellHeight,
                                     geometry.compositionWidth, geometry.compositionHeight,
                                     proxyWidth, proxyHeight},
                                 .dataWindow = dataWindow,
                                 .displayWindow = descriptor.value()->displayWindow(),
                                 .pixelAspect = fullPixelAspect,
                                 .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(command));
    return std::nullopt;
}

template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitCompositionOutputCommand(
    const CompiledCompositionOutput& output, const ResolvedEvaluation& resolved,
    const std::size_t index, const OperationIndex operationIndex, Emit&& emit, Charge&& charge,
    std::vector<GpuSceneCommandIndex>& commandForOperation, std::vector<std::string>& keyOf,
    std::vector<EvaluatedOperationBounds>& bounds) {
    bounds[index] = bounds[output.input.value()];
    const auto inner = commandForOperation[output.input.value()];
    OperationKey key;
    key.add(std::string{"gpu-composition-output-v1"});
    key.add(inner == kInvalidGpuSceneCommand ? std::string{"none"} : keyOf[output.input.value()]);
    addWindowToKey(key, resolved.imageDescriptor.dataWindow());
    addPixelAspectToKey(key, resolved.imageDescriptor.pixelAspect());
    keyOf[index] = key.digest();

    if (const auto error = charge(resolved.imageDescriptor.dataWindow().extent().width(),
                                  resolved.imageDescriptor.dataWindow().extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneCompositionOutputCommand command{.sourceOperation = operationIndex,
                                             .input = inner,
                                             .dataWindow = resolved.imageDescriptor.dataWindow(),
                                             .displayWindow =
                                                 resolved.imageDescriptor.displayWindow(),
                                             .pixelAspect = resolved.imageDescriptor.pixelAspect(),
                                             .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(command));
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_PREPARATION_LEAF_EMISSION_HPP
