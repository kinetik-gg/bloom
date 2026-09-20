#ifndef BLOOM_RUNTIME_GPU_SCENE_VECTOR_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_VECTOR_EMISSION_HPP

// Private to src/runtime. The Layer Output emission for the vector leaves (solid, text, shape) that
// the shared builder in cpu_gpu_scene_preparation.cpp delegates to. It is header-only and templated
// on the builder's emit/charge closures so the split adds no std::function and no shared mutable
// state: the builder still owns the command vector, the per-operation maps, the running budget and
// the coverage cache, and this file owns only the ordered-command wiring for one layer.
//
// Semantics are unchanged from the inline version it replaces: a solid/text layer emits one
// coverage command; a text integer-grid layer emits the leaf coverage plus an identity
// TranslationOpacityBilinearV1 move; a shape emits fill and/or stroke coverage, composes
// stroke-over-fill with SourceOverV1 in CPU order, and applies the layer opacity AFTER that
// composition with an identity TranslationOpacityBilinearV1 pass when needed. A stroke-only shape
// assigns its stroke command directly as the composed layer output.

#include "gpu_scene_coverage.hpp"
#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "operation_key.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace bloom::runtime::detail {

// The evaluator's leaf process window and authored content bounds for a text or shape source. A
// leaf that publishes no image leaves `window` disengaged (and the layer is skipped). Text bounds
// are the window divided by the proxy scales; shape bounds are the raw PathRaster bounds.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
resolveVectorLeafWindow(const CompiledOperation& operation, const CompiledCompositionPlan& plan,
                        const ResolvedEvaluation& resolved, const std::uint64_t coverageByteLimit,
                        const double hScale, const double vScale,
                        const CancellationToken& cancellation,
                        std::optional<render::ImageWindow>& window, ContentBounds& local) {
    window = std::nullopt;
    local = {};
    if (const auto* text = std::get_if<CompiledText>(&operation)) {
        if (const auto error = resolveTextLeafWindow(*text, plan, resolved, coverageByteLimit,
                                                     hScale, vScale, cancellation, window)) {
            return error;
        }
        if (window) {
            local = boundsForWindow(*window, hScale, vScale);
        }
        return std::nullopt;
    }
    if (const auto* shape = std::get_if<CompiledShape>(&operation)) {
        return resolveShapeLeafWindow(*shape, plan, resolved, hScale, vScale, cancellation, window,
                                      local);
    }
    return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InternalInvariant,
                               "A non-vector leaf reached vector window resolution"};
}

// Publishes a leaf's own evaluated bounds (and window) without emitting a command. Returns a
// failure when the leaf itself is invalid; a leaf that publishes no image writes nothing.
template <typename WindowVector, typename BoundsVector>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
publishVectorLeafBounds(const CompiledOperation& operation, const CompiledCompositionPlan& plan,
                        const ResolvedEvaluation& resolved, const std::uint64_t allowance,
                        const double hScale, const double vScale,
                        const CancellationToken& cancellation, const std::size_t index,
                        WindowVector& outputWindowOf, BoundsVector& bounds) {
    std::optional<render::ImageWindow> leafWindow;
    ContentBounds leafLocal;
    if (const auto error = resolveVectorLeafWindow(operation, plan, resolved, allowance, hScale,
                                                   vScale, cancellation, leafWindow, leafLocal)) {
        return error;
    }
    if (leafWindow) {
        outputWindowOf[index] = *leafWindow;
        bounds[index].local = leafLocal;
        bounds[index].output = leafLocal;
    }
    return std::nullopt;
}

// Resolves a vector leaf feeding a Layer Output. `skip` is set when the leaf publishes no image.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> resolveLayerVectorInput(
    const CompiledOperation& inputOperation, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const std::uint64_t allowance, const double hScale,
    const double vScale, const CancellationToken& cancellation,
    std::optional<render::ImageWindow>& sourceWindow, ContentBounds& local, bool& skip) {
    skip = false;
    std::optional<render::ImageWindow> leafWindow;
    if (const auto error =
            resolveVectorLeafWindow(inputOperation, plan, resolved, allowance, hScale, vScale,
                                    cancellation, leafWindow, local)) {
        return error;
    }
    if (!leafWindow) {
        skip = true;
        return std::nullopt;
    }
    sourceWindow = *leafWindow;
    return std::nullopt;
}

// True when a reachable text or shape leaf was consumed by no prepared layer.
[[nodiscard]] inline bool hasUnconsumedVectorLeaf(const CompiledCompositionPlan& plan,
                                                  const std::vector<bool>& reachable,
                                                  const std::vector<bool>& textConsumed,
                                                  const std::vector<bool>& shapeConsumed) {
    for (std::size_t index = 0; index < plan.operations().size(); ++index) {
        if (!reachable[index]) {
            continue;
        }
        const bool unconsumedText =
            std::holds_alternative<CompiledText>(plan.operations()[index]) && !textConsumed[index];
        const bool unconsumedShape =
            std::holds_alternative<CompiledShape>(plan.operations()[index]) &&
            !shapeConsumed[index];
        if (unconsumedText || unconsumedShape) {
            return true;
        }
    }
    return false;
}

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitVectorLeafLayer(
    const CompiledOperation& inputOperation, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
    const render::ImageWindow layerWindow, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const double hScale, const double vScale,
    const double opacity, const bool nativeGrid, const std::uint64_t allowance,
    const OperationIndex operationIndex, const render::LayerTransform& transformValue,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, GpuSceneCommandIndex& composed, std::string& semanticKey,
    bool& consumedText, bool& consumedShape) {
    const auto* solidInput = std::get_if<CompiledSolid>(&inputOperation);
    const auto* textInput = std::get_if<CompiledText>(&inputOperation);
    const auto* shapeInput = std::get_if<CompiledShape>(&inputOperation);

    if (textInput != nullptr && nativeGrid) {
        // The integer-grid CPU arm renders the text leaf's own proxy-scaled coverage image and
        // translates it by an exact integer. Build that leaf coverage over its own bitmap window,
        // then the identity TranslationOpacityBilinearV1 command performs the exact integer move
        // and the layer opacity.
        GpuSceneCoverageSolidCommand leafCommand{
            .index = kInvalidGpuSceneCommand,
            .sourceOperation = operationIndex,
            .pixel = render::Rgba32f::transparent(),
            .opacity = 1.0F,
            .coverage = nullptr,
            .outputWindow = layerWindow,
            .displayWindow = fullDisplayWindow,
            .pixelAspect = fullPixelAspect,
            .geometryKey = {},
            .semanticKey = {},
        };
        if (const auto error = buildTextLeafCoverageLeaf(
                *textInput, plan, resolved, layerWindow, fullDisplayWindow, fullPixelAspect, hScale,
                vScale, opacity, allowance, coverageCache, charge, cancellation, leafCommand)) {
            return error;
        }
        if (const auto error =
                chargeCoverage(leafCommand.coverage, leafCommand.outputWindow.extent().width(),
                               leafCommand.outputWindow.extent().height())) {
            return error;
        }
        const auto leafSourceWindow = leafCommand.outputWindow;
        const std::string leafKey = leafCommand.semanticKey;
        leafCommand.sourceOperation = operationIndex;
        const auto leafIndex = emit(std::move(leafCommand));
        const auto device = transformValue.translationOnlyDeviceTranslation();
        if (!device.has_value()) {
            return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                       "Translation-only transform lost its device translation"};
        }
        const double gpuTranslationX =
            device->x - (static_cast<double>(layerWindow.originX()) -
                         static_cast<double>(leafSourceWindow.originX()));
        const double gpuTranslationY =
            device->y - (static_cast<double>(layerWindow.originY()) -
                         static_cast<double>(leafSourceWindow.originY()));
        OperationKey key;
        key.add(std::string{"gpu-translation-opacity-v1"});
        key.add(leafKey);
        key.add(gpuTranslationX);
        key.add(gpuTranslationY);
        key.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity)));
        addWindowToKey(key, leafSourceWindow);
        addWindowToKey(key, layerWindow);
        addPixelAspectToKey(key, fullPixelAspect);
        key.add(std::string{kGpuTranslationOpacitySpirvSha256});
        const auto translationKey = key.digest();
        if (const auto error = charge(layerWindow.extent().width(), layerWindow.extent().height(),
                                      sizeof(render::Rgba32f))) {
            return error;
        }
        GpuSceneTranslationCommand translation{.sourceOperation = operationIndex,
                                               .input = leafIndex,
                                               .sourceWindow = leafSourceWindow,
                                               .outputWindow = layerWindow,
                                               .translationX = gpuTranslationX,
                                               .translationY = gpuTranslationY,
                                               .opacity = static_cast<float>(opacity),
                                               .semanticKey = translationKey};
        semanticKey = translationKey;
        composed = emit(std::move(translation));
        consumedText = true;
        return std::nullopt;
    }

    // Fractional device grid: the CPU composes this layer through its vector-coverage path.
    GpuSceneCoverageSolidCommand coverageCommand{
        .index = kInvalidGpuSceneCommand,
        .sourceOperation = operationIndex,
        .pixel = render::Rgba32f::transparent(),
        .opacity = 1.0F,
        .coverage = nullptr,
        .outputWindow = layerWindow,
        .displayWindow = fullDisplayWindow,
        .pixelAspect = fullPixelAspect,
        .geometryKey = {},
        .semanticKey = {},
    };
    if (shapeInput != nullptr) {
        GpuSceneShapeCoverage shapeCoverage;
        if (const auto error =
                buildShapeCoverage(*shapeInput, plan, resolved, matrix, layerWindow,
                                   fullDisplayWindow, fullPixelAspect, hScale, vScale, opacity,
                                   coverageCache, charge, cancellation, shapeCoverage)) {
            return error;
        }
        std::string composedKey;
        GpuSceneCommandIndex strokeIndex = kInvalidGpuSceneCommand;
        std::string strokeKey;
        if (shapeCoverage.hasFill()) {
            if (const auto error =
                    chargeCoverage(shapeCoverage.fill->coverage, layerWindow.extent().width(),
                                   layerWindow.extent().height())) {
                return error;
            }
            composedKey = shapeCoverage.fill->semanticKey;
            shapeCoverage.fill->sourceOperation = operationIndex;
            composed = emit(std::move(*shapeCoverage.fill));
        }
        if (shapeCoverage.hasStroke()) {
            if (const auto error =
                    chargeCoverage(shapeCoverage.stroke->coverage, layerWindow.extent().width(),
                                   layerWindow.extent().height())) {
                return error;
            }
            strokeKey = shapeCoverage.stroke->semanticKey;
            shapeCoverage.stroke->sourceOperation = operationIndex;
            strokeIndex = emit(std::move(*shapeCoverage.stroke));
            // A stroke-only shape has no fill and no internal fill/stroke merge, so the stroke
            // coverage IS the layer command. Without this the layer composed nothing.
            if (!shapeCoverage.hasFill()) {
                composed = strokeIndex;
                composedKey = strokeKey;
            }
        }
        if (shapeCoverage.hasFill() && shapeCoverage.hasStroke()) {
            // CPU composes stroke OVER fill; the merge folds foregrounds in order over a
            // transparent destination, so {fill, stroke} reproduces it exactly.
            OperationKey mergeKey;
            mergeKey.add(std::string{"gpu-source-over-chain-v1"});
            mergeKey.add(composedKey);
            mergeKey.add(strokeKey);
            addWindowToKey(mergeKey, layerWindow);
            addWindowToKey(mergeKey, fullDisplayWindow);
            addPixelAspectToKey(mergeKey, fullPixelAspect);
            mergeKey.add(std::string{kGpuSourceOverSpirvSha256});
            composedKey = mergeKey.digest();
            GpuSceneMergeCommand merge{.sourceOperation = operationIndex,
                                       .foregrounds = {composed, strokeIndex},
                                       .outputWindow = layerWindow,
                                       .displayWindow = fullDisplayWindow,
                                       .pixelAspect = fullPixelAspect,
                                       .semanticKey = composedKey};
            composed = emit(std::move(merge));
        }
        if (shapeCoverage.needsPostOpacity) {
            if (const auto error = charge(layerWindow.extent().width(),
                                          layerWindow.extent().height(), sizeof(render::Rgba32f))) {
                return error;
            }
            OperationKey key;
            key.add(std::string{"gpu-translation-opacity-v1"});
            key.add(composedKey);
            key.add(0.0);
            key.add(0.0);
            key.add(std::bit_cast<std::uint32_t>(shapeCoverage.opacity));
            addWindowToKey(key, layerWindow);
            addWindowToKey(key, layerWindow);
            addPixelAspectToKey(key, fullPixelAspect);
            key.add(std::string{kGpuTranslationOpacitySpirvSha256});
            composedKey = key.digest();
            GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                               .input = composed,
                                               .sourceWindow = layerWindow,
                                               .outputWindow = layerWindow,
                                               .translationX = 0.0,
                                               .translationY = 0.0,
                                               .opacity = shapeCoverage.opacity,
                                               .semanticKey = composedKey};
            composed = emit(std::move(command));
        }
        semanticKey = composedKey;
        consumedShape = true;
        return std::nullopt;
    }

    if (solidInput != nullptr) {
        if (const auto error =
                buildCoverageSolidLeaf(*solidInput, plan, resolved, matrix, layerWindow,
                                       fullDisplayWindow, fullPixelAspect, hScale, vScale, opacity,
                                       coverageCache, charge, cancellation, coverageCommand)) {
            return error;
        }
    } else {
        const auto textError =
            nativeGrid
                ? buildTextLeafCoverageLeaf(*textInput, plan, resolved, layerWindow,
                                            fullDisplayWindow, fullPixelAspect, hScale, vScale,
                                            opacity, allowance, coverageCache, charge, cancellation,
                                            coverageCommand)
                : buildTextCoverageLeaf(*textInput, plan, resolved, matrix, layerWindow,
                                        fullDisplayWindow, fullPixelAspect, hScale, vScale, opacity,
                                        coverageCache, charge, cancellation, coverageCommand);
        if (textError) {
            return textError;
        }
        consumedText = true;
    }
    if (const auto error = chargeCoverage(coverageCommand.coverage, layerWindow.extent().width(),
                                          layerWindow.extent().height())) {
        return error;
    }
    semanticKey = coverageCommand.semanticKey;
    coverageCommand.sourceOperation = operationIndex;
    composed = emit(std::move(coverageCommand));
    return std::nullopt;
}

// Builds the Normal SourceOver merge command for a merged stack of already-emitted foreground
// commands, folding entries bottom-to-top and charging the merge output.
template <typename WindowVector, typename BoundsVector, typename Emit, typename Charge>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
emitMergeCommand(const CompiledMerge& stack, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved, const std::size_t index, BoundsVector& bounds,
                 WindowVector& outputWindowOf,
                 std::vector<GpuSceneCommandIndex>& commandForOperation,
                 std::vector<std::string>& keyOf, const OperationIndex operationIndex, Emit&& emit,
                 Charge&& charge) {
    ContentBounds storage;
    for (const auto& entry : stack.entries) {
        bounds[index].local =
            detail::unionBounds(bounds[index].local, bounds[entry.input.value()].output);
        const auto& entryWindow = outputWindowOf[entry.input.value()];
        if (entryWindow) {
            storage = detail::unionBounds(storage, detail::boundsForWindow(*entryWindow, 1.0, 1.0));
        }
    }
    bounds[index].output = bounds[index].local;
    auto descriptor = resolved.imageDescriptor;
    if (!storage.empty()) {
        const auto window = render::ImageWindow::create(
            static_cast<std::int64_t>(storage.left), static_cast<std::int64_t>(storage.top),
            static_cast<std::uint64_t>(storage.right - storage.left),
            static_cast<std::uint64_t>(storage.bottom - storage.top));
        if (!window) {
            return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                       "Merge bounds are invalid"};
        }
        const auto local = render::Rgba32fImageDescriptor::create(
            *window.value(), descriptor.displayWindow(), descriptor.pixelAspect());
        if (!local) {
            return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                       "Merge descriptor is invalid"};
        }
        descriptor = *local.value();
    }
    outputWindowOf[index] = descriptor.dataWindow();

    std::vector<GpuSceneCommandIndex> foregrounds;
    OperationKey key;
    key.add(std::string{"gpu-source-over-chain-v1"});
    for (auto entry = stack.entries.rbegin(); entry != stack.entries.rend(); ++entry) {
        if (const auto* layerOutput =
                std::get_if<CompiledLayerOutput>(&plan.operations()[entry->input.value()]);
            layerOutput != nullptr && entry->layerId.isValid()) {
            const auto blend = resolveParameter(*layerOutput, resolved);
            if (!blend || *blend != core::BlendMode::Normal) {
                return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::UnsupportedBlend,
                                           "Only Normal merge entries are prepared"};
            }
        }
        const auto inner = commandForOperation[entry->input.value()];
        if (inner == kInvalidGpuSceneCommand) {
            continue;
        }
        foregrounds.push_back(inner);
        key.add(keyOf[entry->input.value()]);
    }
    addWindowToKey(key, descriptor.dataWindow());
    addWindowToKey(key, descriptor.displayWindow());
    addPixelAspectToKey(key, descriptor.pixelAspect());
    key.add(std::string{kGpuSourceOverSpirvSha256});
    keyOf[index] = key.digest();
    if (const auto error =
            charge(descriptor.dataWindow().extent().width(),
                   descriptor.dataWindow().extent().height(), sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneMergeCommand command{.sourceOperation = operationIndex,
                                 .foregrounds = std::move(foregrounds),
                                 .outputWindow = descriptor.dataWindow(),
                                 .displayWindow = descriptor.displayWindow(),
                                 .pixelAspect = descriptor.pixelAspect(),
                                 .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(command));
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_VECTOR_EMISSION_HPP
