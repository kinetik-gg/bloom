#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_LEAF_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_LEAF_EMISSION_HPP

// Private to src/runtime. The self-contained leaf command emissions moved out of the builder
// orchestrator to keep cpu_gpu_scene_preparation.cpp within its source-size budget: the SolidV1
// solid command and the terminal Composition Output. Both are templated on the builder's
// emit/charge closures so no std::function or shared state is introduced; the builder still owns
// the command vector, the per-operation maps, the running budget, and the coverage cache.

#include "gpu_scene_effect_emission.hpp"
#include "checker_image.hpp"
#include "gradient_source.hpp"
#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>

#include <algorithm>
#include <array>
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
    if (!color->value.isValid())
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Source colour is invalid");
    const auto pixel = solid.gradient
        ? render::ImageResult<render::Rgba32f>::success(render::Rgba32f::transparent())
        : render::solidPixelFromStraightLinearRec709Scene(color->value);
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
    std::optional<render::GpuBatchGradient> gpuGradient;
    if (solid.gradient) {
        const auto endColor = resolveParameter(solid.gradient->endColor, plan, resolved);
        const auto start = resolveParameter(solid.gradient->start, plan, resolved);
        const auto end = resolveParameter(solid.gradient->end, plan, resolved);
        if (!endColor || !start || !end || !endColor->value.isValid() ||
            !std::isfinite(start->value.x) || !std::isfinite(start->value.y) ||
            !std::isfinite(end->value.x) || !std::isfinite(end->value.y))
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Gradient endpoints are not evaluable");
        gpuGradient.emplace(render::GpuBatchGradient{solidWindow, solidDisplay, solidAspect});
        gpuGradient->first = {static_cast<float>(color->value.red),
                              static_cast<float>(color->value.green),
                              static_cast<float>(color->value.blue),
                              static_cast<float>(color->value.alpha)};
        gpuGradient->second = {static_cast<float>(endColor->value.red),
                               static_cast<float>(endColor->value.green),
                               static_cast<float>(endColor->value.blue),
                               static_cast<float>(endColor->value.alpha)};
        gpuGradient->endpoints = {static_cast<float>(start->value.x),
                                  static_cast<float>(start->value.y),
                                  static_cast<float>(end->value.x),
                                  static_cast<float>(end->value.y)};
        const auto finiteColor = [](const auto& components) {
            return std::ranges::all_of(components, [](const float value) {
                return std::isfinite(value);
            });
        };
        if (!finiteColor(gpuGradient->first) || !finiteColor(gpuGradient->second))
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                        "Gradient straight colours exceed native Float32 representation");
        const double aspect = static_cast<double>(solidAspect.numerator()) /
                              solidAspect.denominator();
        const double dx = (end->value.x - start->value.x) * aspect;
        const double dy = end->value.y - start->value.y;
        const double span = std::hypot(dx, dy);
        std::array<double, 4> coordinates{};
        if (start->value.x != end->value.x || start->value.y != end->value.y) {
            if (!(span > 0.0) || !std::isfinite(span))
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                            "Gradient span exceeds native coordinate preparation");
            const double x = ((0.5 / hScale - start->value.x) * aspect) / span;
            const double y = (0.5 / vScale - start->value.y) / span;
            const double column = (aspect / hScale) / span;
            const double row = (1.0 / vScale) / span;
            coordinates = solid.gradient->radial
                ? std::array<double, 4>{x, y, column, row}
                : std::array<double, 4>{(dx == 0.0 ? 0.0 : x * (dx / span)) +
                                           (dy == 0.0 ? 0.0 : y * (dy / span)),
                                       column * (dx / span), row * (dy / span), 0.0};
        }
        std::array<float, 8> split{};
        for (std::size_t component = 0; component < coordinates.size(); ++component) {
            const auto high = static_cast<float>(coordinates[component]);
            const auto low = static_cast<float>(coordinates[component] - static_cast<double>(high));
            if (!std::isfinite(high) || !std::isfinite(low) ||
                (coordinates[component] != 0.0 && high == 0.0F && low == 0.0F))
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                            "Gradient normalized coordinates exceed native Float32 pair representation");
            split[component * 2] = high;
            split[component * 2 + 1] = low;
        }
        gpuGradient->normalizedCoordinates = split;
        gpuGradient->inverseHorizontalScale = static_cast<float>(1.0 / hScale);
        gpuGradient->inverseVerticalScale = static_cast<float>(1.0 / vScale);
        gpuGradient->exactWidth = static_cast<float>(exactWidth);
        gpuGradient->exactHeight = static_cast<float>(exactHeight);
        gpuGradient->repeat = static_cast<std::uint32_t>(solid.gradient->repeat);
        gpuGradient->radial = solid.gradient->radial;
    }
    auto& entry = bounds[index];
    entry.local = boundsForWindow(solidWindow, hScale, vScale);
    entry.local.right = width->value;
    entry.local.bottom = height->value;
    entry.output = entry.local;
    outputWindowOf[index] = solidWindow;

    OperationKey key;
    key.add(std::string{gpuGradient ? "gpu-gradient-v1" : "gpu-solid-v1"});
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
    key.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
    addWindowToKey(key, solidWindow);
    addWindowToKey(key, solidDisplay);
    addPixelAspectToKey(key, solidAspect);
    if (gpuGradient) {
        for (const auto component : gpuGradient->first)
            key.add(std::bit_cast<std::uint32_t>(component));
        for (const auto component : gpuGradient->second)
            key.add(std::bit_cast<std::uint32_t>(component));
        for (const auto component : gpuGradient->endpoints)
            key.add(std::bit_cast<std::uint32_t>(component));
        key.add(std::bit_cast<std::uint32_t>(gpuGradient->inverseHorizontalScale));
        key.add(std::bit_cast<std::uint32_t>(gpuGradient->inverseVerticalScale));
        key.add(std::bit_cast<std::uint32_t>(gpuGradient->exactWidth));
        key.add(std::bit_cast<std::uint32_t>(gpuGradient->exactHeight));
        key.add(gpuGradient->normalizedCoordinates.has_value());
        if (gpuGradient->normalizedCoordinates)
            for (const auto component : *gpuGradient->normalizedCoordinates)
                key.add(std::bit_cast<std::uint32_t>(component));
        key.add(gpuGradient->repeat);
        key.add(gpuGradient->radial);
    }
    key.add(std::string{gpuGradient ? kGpuGradientSpirvSha256 : kGpuSolidSpirvSha256});
    keyOf[index] = key.digest();

    if (const auto error = charge(solidWindow.extent().width(), solidWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneSolidCommand command{.sourceOperation = operationIndex,
                                 .pixel = *pixel.value(),
                                 .checker = std::nullopt,
                                 .gradient = std::move(gpuGradient),
                                 .dataWindow = solidWindow,
                                 .displayWindow = solidDisplay,
                                 .pixelAspect = solidAspect,
                                 .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(command));
    if (!solid.gradient)
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
                                 .gradient = std::nullopt,
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
