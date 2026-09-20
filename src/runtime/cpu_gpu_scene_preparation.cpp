#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_media_preparation.hpp"
#include "gpu_scene_coverage.hpp"
#include "gpu_scene_effect_emission.hpp"
#include "gpu_scene_layer_emission.hpp"
#include "gpu_scene_media_layer.hpp"
#include "gpu_scene_nested.hpp"
#include "gpu_scene_preparation_builders.hpp"
#include "gpu_scene_preparation_leaf_emission.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "gpu_scene_vector_emission.hpp"

#include "operation_key.hpp"
#include <bloom/core/blend_mode.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {
namespace {

[[nodiscard]] PreparedGpuSceneBuildResult failed(const PreparedGpuSceneDiagnosticCode code,
                                                 std::string message,
                                                 GpuSceneMediaStatistics statistics = {}) {
    return {nullptr, PreparedGpuSceneDiagnostic{code, std::move(message), statistics}};
}

// The CPU evaluator's vector chain: a solid/text/shape leaf, the composed author-space matrix every
// enclosing layer multiplied into it, and the accumulated layer opacity. It is propagated through a
// Layer Output exactly as the evaluator does, so a layer fed by another vector layer rasterizes the
// ORIGINAL leaf geometry through the full chain rather than resampling an intermediate raster.
// Defined in gpu_scene_effect_emission.hpp so an identity image effect can propagate it unchanged.
using detail::GpuSceneVectorChain;

// Operations whose pixels are colour-space agnostic under any resolvable working space, verified
// against the CPU reference:
//  * Solid / Text / Shape authored colours are numeric values in the effective working space; the
//    CPU evaluator only premultiplies them (render::solidPixelFromStraightLinearRec709Scene is a
//    pure premultiply, no working-space transform), and the GPU solid / coverage commands store the
//    same premultiplied values, so they are correct for any working space.
//  * Layer Output / Affine / Translation / Merge / Blend operate on already-resolved premultiplied
//    values and are colour-space agnostic.
//  * Image Source / Video Source are decoded raw and converted to the working space through a real
//    input->working OCIO ProcessEffect command.
//  * Image Effect resolves its from/to ids in the effective working space.
//  * A nested composition inherits the working space; the composition output is a passthrough.
// An operation not listed here is refused for a non-neutral working space, so a future
// working-space-specific operation cannot silently mis-colour.
[[nodiscard]] bool isWorkingSpaceAgnosticOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledText>(operation) ||
           std::holds_alternative<CompiledShape>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledImageEffect>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionSource>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation);
}

} // namespace

PreparedGpuSceneBuildResult
CpuGpuSceneBuilder::build(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                          const EvaluationRequest& request,
                          const CancellationToken& cancellation) const {
    try {
        return buildImpl(plan, request, cancellation);
    } catch (const std::bad_alloc&) {
        return failed(PreparedGpuSceneDiagnosticCode::AllocationFailure,
                      "Scene preparation ran out of memory");
    } catch (...) {
        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Scene preparation failed unexpectedly");
    }
}

PreparedGpuSceneBuildResult
CpuGpuSceneBuilder::buildImpl(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                              const EvaluationRequest& request,
                              const CancellationToken& cancellation) const {
    if (plan == nullptr) {
        return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan, "No compiled plan");
    }
    if (plan->planSemanticsVersion() != kCompiledCompositionPlanSemanticsVersion ||
        plan->animationSamplingSemanticsVersion() != kAnimationSamplingSemanticsVersion) {
        return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                      "Compiled plan semantics are unsupported");
    }
    if (request.quality != EvaluationQuality::Reference ||
        request.colorIntent.workingColorSpaceId.find('\0') != std::string_view::npos ||
        request.roi) {
        return failed(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                      "Only Reference quality and no ROI are prepared");
    }
    // The non-media prepared subset (solid pixels, vector coverage, image effects) assumes
    // lin_rec709_scene pixel semantics, so a different working space is refused for it. A
    // media-only scene carries no such operation: its pixels are resolved through the exact
    // configured input->working OCIO transform, so an ACES scene-linear working space (or any other
    // resolvable one) is prepared. The reachable-subset check runs below.
    const bool neutralWorkingSpace =
        request.colorIntent.workingColorSpaceId == kLinearRec709SceneColorSpaceId;
    if (plan->operations().empty() || request.output.value() >= plan->operations().size() ||
        request.output != plan->output() ||
        !std::holds_alternative<CompiledCompositionOutput>(
            plan->operations()[request.output.value()])) {
        return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                      "Request output is not the plan's terminal Composition Output");
    }
    if (cancellation.isCancellationRequested()) {
        return failed(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
    }

    const std::size_t operationCount = plan->operations().size();
    // The reachable-operation subset classifier is extracted to gpu_scene_preparation_builders.hpp.
    std::vector<bool> reachable;
    if (const auto error = detail::computeReachablePreparedOperations(*plan, request.output.value(),
                                                                      cancellation, reachable)) {
        return failed(error->code, error->message);
    }
    // A non-neutral working space is admitted for every operation whose pixels the CPU reference
    // resolves in the effective working space (see isWorkingSpaceAgnosticOperation). The display
    // consumer remains a separate lane.
    if (!neutralWorkingSpace) {
        for (std::size_t index = 0; index < operationCount; ++index) {
            if (reachable[index] && !isWorkingSpaceAgnosticOperation(plan->operations()[index])) {
                return failed(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                              "A non-lin_rec709_scene working space reached an operation without "
                              "verified working-space semantics");
            }
        }
    }

    auto checked = detail::preflight(plan, request, cancellation, {}, nullptr, nullptr);
    if (checked.cancelled || cancellation.isCancellationRequested()) {
        return failed(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
    }
    if (!checked.resolved.has_value()) {
        const auto code = checked.diagnostic.has_value()
                              ? checked.diagnostic->code
                              : EvaluationDiagnosticCode::InternalInvariant;
        std::string message;
        if (checked.diagnostic.has_value()) {
            message = checked.diagnostic->summary;
            if (message.empty()) {
                message = checked.diagnostic->detail;
            }
        }
        if (message.empty()) {
            message = "Preflight failed";
        }
        return failed(code == EvaluationDiagnosticCode::PixelStorageBudgetExceeded
                          ? PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded
                          : PreparedGpuSceneDiagnosticCode::PreflightFailure,
                      std::move(message));
    }
    const detail::ResolvedEvaluation& resolved = *checked.resolved;
    if (resolved.imageDescriptor.dataWindow().extent().width() == 0) {
        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Preflight produced an invalid process descriptor");
    }
    const auto fullDescriptor = render::Rgba32fImageDescriptor::create(
        resolved.imageDescriptor.displayWindow(), resolved.imageDescriptor.displayWindow(),
        resolved.imageDescriptor.pixelAspect());
    if (!fullDescriptor) {
        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Composition descriptor is invalid");
    }
    const auto fullWindow = fullDescriptor.value()->dataWindow();
    const auto fullDisplayWindow = fullDescriptor.value()->displayWindow();
    const auto fullPixelAspect = fullDescriptor.value()->pixelAspect();

    const double hScale = resolved.horizontalScale;
    const double vScale = resolved.verticalScale;

    // Screen every reachable layer for an unsupported blend or transform BEFORE any media leaf is
    // resolved or decoded, so a graph containing an unsupported layer never prepares -- and never
    // decodes -- its media. An inactive layer, exactly as in the CPU evaluator, publishes nothing
    // and is therefore not screened.
    if (const auto error =
            detail::screenUnsupportedLayers(*plan, request, resolved, cancellation)) {
        return failed(error->code, error->message);
    }

    // This build's own CPU work counters. They are local to the call -- never shared across
    // concurrent preparation jobs -- and are published either on the prepared scene or, for a
    // failed build, on the returned diagnostic.
    GpuSceneMediaStatistics mediaStatistics;

    std::vector<GpuSceneCommand> commands;
    std::vector<GpuSceneCommandIndex> commandForOperation(operationCount, kInvalidGpuSceneCommand);
    std::vector<EvaluatedOperationBounds> bounds(operationCount);
    std::vector<std::optional<render::ImageWindow>> outputWindowOf(operationCount);
    std::vector<std::string> keyOf(operationCount);
    // Per-operation author-space composed layer matrix (parent*child) and vector chain, exactly as
    // the CPU evaluator computes them in operation order before it evaluates any layer.
    std::vector<std::optional<detail::LayerMatrix>> layerMatrices(operationCount);
    std::vector<std::optional<GpuSceneVectorChain>> vectors(operationCount);
    // Reachable text leaves publish no command of their own; the Layer Output that consumes one
    // builds the coverage command from the layer matrix. This records that consumption so a text
    // that reaches the output by any other route fails closed instead of silently disappearing.
    std::vector<bool> textConsumed(operationCount, false);
    std::vector<bool> shapeConsumed(operationCount, false);

    const auto emit = [&](GpuSceneCommand command) -> GpuSceneCommandIndex {
        const auto index = static_cast<GpuSceneCommandIndex>(commands.size());
        std::visit([index](auto& item) { item.index = index; }, command);
        commands.push_back(std::move(command));
        return index;
    };

    // The prepared scene RETAINS only host allocations: the frozen upload images and the coverage
    // rasters. Every other command (solid, translation, affine, blend, merge, composition output)
    // is a window + transform description whose RGBA32F output is allocated at executor time under
    // the executor's own LIVE-pin budget. Summing those mutually exclusive output lifetimes into
    // one per-request total refuses graphs the executor -- and the CPU reference -- can actually
    // run (for example a 4608x3164 source over an FHD solid and text). Only the retained host set
    // is bounded here; GPU residency is bounded by GpuSceneExecutor::begin(scene, budget), which
    // takes the honest CPU fallback on refusal. Overflow-checked with subtraction-only comparators.
    const std::uint64_t allowance = request.pixelStorageByteLimit;
    std::uint64_t retainedBytes = 0;
    std::unordered_set<const void*> countedCoverage;
    const auto chargeRetained =
        [&](const std::uint64_t width, const std::uint64_t height,
            const std::uint64_t bytesPerPixel) -> std::optional<detail::GpuSceneLeafFailure> {
        if (width == 0 || height == 0) {
            return detail::GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                               "Prepared command has an empty window"};
        }
        const std::array<std::uint64_t, 3> factors{width, height, bytesPerPixel};
        const auto bytes = detail::checkedProduct(factors);
        if (!bytes.has_value()) {
            return detail::GpuSceneLeafFailure{
                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                "Prepared command size overflows"};
        }
        if (retainedBytes > allowance || *bytes > allowance - retainedBytes) {
            return detail::GpuSceneLeafFailure{
                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                "Prepared scene exceeds the request pixel allowance"};
        }
        retainedBytes += *bytes;
        return std::nullopt;
    };
    // GPU-transient outputs are not summed here; the executor's live-pin ledger bounds them.
    const auto chargeTransient = [](const std::uint64_t, const std::uint64_t, const std::uint64_t)
        -> std::optional<detail::GpuSceneLeafFailure> { return std::nullopt; };
    const auto chargeCoverage =
        [&](const std::shared_ptr<const std::vector<std::uint8_t>>& coverage,
            const std::uint64_t width,
            const std::uint64_t height) -> std::optional<detail::GpuSceneLeafFailure> {
        if (coverage == nullptr || !countedCoverage.insert(coverage.get()).second) {
            return std::nullopt;
        }
        return chargeRetained(width, height, sizeof(std::uint8_t));
    };

    for (std::size_t index = 0; index < operationCount; ++index) {
        if (!reachable[index]) {
            continue;
        }
        if (cancellation.isCancellationRequested()) {
            return failed(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        const OperationIndex operationIndex = OperationIndex::fromRaw(index);
        const auto& operation = plan->operations()[index];

        if (const auto* solid = std::get_if<CompiledSolid>(&operation)) {
            if (const auto error = detail::emitSolidCommand(
                    *solid, *plan, resolved, index, operationIndex, fullWindow, fullDisplayWindow,
                    fullPixelAspect, hScale, vScale, emit, chargeTransient, commandForOperation,
                    keyOf, outputWindowOf, bounds, vectors)) {
                return failed(error->code, error->message);
            }
            continue;
        }

        if (std::holds_alternative<CompiledText>(operation) ||
            std::holds_alternative<CompiledShape>(operation)) {
            // A text or shape leaf emits no command of its own: the Layer Output that consumes it
            // builds the vector-coverage command from the layer matrix. Consumption is recorded
            // there and checked after the loop, so a vector leaf reaching the output by any other
            // route fails closed. Its own evaluated bounds are still published here so the prepared
            // scene's bounds match the CPU frame exactly, leaf included.
            if (const auto error = detail::publishVectorLeafBounds(
                    operation, *plan, resolved, allowance, hScale, vScale, cancellation, index,
                    outputWindowOf, bounds)) {
                return failed(error->code, error->message, mediaStatistics);
            }
            vectors[index] = GpuSceneVectorChain{index, {}, 1.0};
            continue;
        }

        if (const auto* image = std::get_if<CompiledImageSource>(&operation)) {
            const auto remainingBudget = allowance > retainedBytes ? allowance - retainedBytes : 0;
            detail::GpuSceneUploadLeafResult leaf;
            const auto error = detail::buildImageColorLeaf(
                *image, request, *plan, resolved, mediaContext_, ocioContext_, remainingBudget,
                hScale, vScale, chargeRetained, cancellation, mediaStatistics, leaf);
            if (error) {
                return failed(error->code, error->message, mediaStatistics);
            }
            bounds[index] = leaf.bounds;
            outputWindowOf[index] = leaf.outputWindow;
            if (const auto emitError =
                    detail::emitMediaLeafCommands(operationIndex, leaf, chargeTransient, emit,
                                                  commandForOperation[index], keyOf[index])) {
                return failed(emitError->code, emitError->message, mediaStatistics);
            }
            continue;
        }

        if (const auto* video = std::get_if<CompiledVideoSource>(&operation)) {
            const auto remainingBudget = allowance > retainedBytes ? allowance - retainedBytes : 0;
            detail::GpuSceneUploadLeafResult leaf;
            const auto error = detail::buildVideoColorLeaf(
                *video, request, *plan, resolved, mediaContext_, ocioContext_, remainingBudget,
                hScale, vScale, chargeRetained, cancellation, mediaStatistics, leaf);
            if (error) {
                return failed(error->code, error->message, mediaStatistics);
            }
            bounds[index] = leaf.bounds;
            outputWindowOf[index] = leaf.outputWindow;
            if (const auto emitError =
                    detail::emitMediaLeafCommands(operationIndex, leaf, chargeTransient, emit,
                                                  commandForOperation[index], keyOf[index])) {
                return failed(emitError->code, emitError->message, mediaStatistics);
            }
            continue;
        }

        if (const auto* effect = std::get_if<CompiledImageEffect>(&operation)) {
            const detail::GpuSceneEffectEmissionContext effectContext{
                .plan = plan.get(),
                .request = &request,
                .resolved = &resolved,
                .vectors = &vectors,
                .commandForOperation = &commandForOperation,
                .keyOf = &keyOf,
                .outputWindowOf = &outputWindowOf,
                .bounds = &bounds,
                .commands = &commands,
                .fullDisplayWindow = fullDisplayWindow,
                .fullPixelAspect = fullPixelAspect,
                .hScale = hScale,
                .vScale = vScale,
                .allowance = allowance,
                .assetBaseDirectory = mediaContext_.assetBaseDirectory,
                .coverageCache = coverageCache_,
            };
            if (const auto error = detail::emitImageEffectOperation(
                    *effect, operationIndex, effectContext, ocioContext_, cancellation, emit,
                    chargeTransient, chargeCoverage, commandForOperation, keyOf, outputWindowOf,
                    bounds, vectors, textConsumed, shapeConsumed)) {
                return failed(error->code, error->message, mediaStatistics);
            }
            continue;
        }

        if (const auto* source = std::get_if<CompiledCompositionSource>(&operation)) {
            // A nested composition is prepared by recursively building the CHILD plan with this
            // same production builder and splicing its genuine GPU commands into this list. The
            // child's terminal Composition Output command is aliased as this operation's image; the
            // child's content-addressed command keys are preserved, so an unrelated child edit
            // still reuses every untouched branch. No finished child frame is ever CPU-uploaded.
            detail::GpuSceneNestedResult nested;
            const auto error = detail::prepareNestedComposition(
                *source, *plan, request, resolved, hScale, vScale, detail::nestedCompositionDepth(),
                allowance, retainedBytes, cancellation,
                [this](const std::shared_ptr<const CompiledCompositionPlan>& childPlan,
                       const EvaluationRequest& childRequest, const CancellationToken& cancel) {
                    return build(childPlan, childRequest, cancel);
                },
                commands, nested);
            if (error) {
                return failed(error->code, error->message, mediaStatistics);
            }
            bounds[index] = nested.bounds;
            outputWindowOf[index] = *nested.outputWindow;
            keyOf[index] = nested.outputKey;
            commandForOperation[index] = nested.outputCommand;
            continue;
        }

        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            const auto position = detail::resolveParameter(layer->position, *plan, resolved);
            const auto anchor = detail::resolveParameter(layer->anchor, *plan, resolved);
            const auto scale = detail::resolveParameter(layer->scale, *plan, resolved);
            const auto rotation = detail::resolveParameter(layer->rotation, *plan, resolved);
            const auto opacity = detail::resolveParameter(layer->opacity, *plan, resolved);
            if (!position || !anchor || !scale || !rotation || !opacity) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Layer parameters are not evaluable");
            }
            const std::size_t inputIndexValue = layer->input.value();
            if (inputIndexValue >= operationCount) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Layer input is invalid");
            }
            // The input's command, window and authored content bounds. A vector leaf (solid, text,
            // shape) published them when it was reached; a merge or another layer published its own
            // raster command and bounds. A parent chain is resolved through the parent matrix.
            const auto& inputVec = vectors[inputIndexValue];
            const GpuSceneCommandIndex inputIndex = commandForOperation[inputIndexValue];
            std::optional<render::ImageWindow> sourceWindowHolder;
            ContentBounds inputLocal;
            if (inputVec.has_value()) {
                const auto& leafWindow = outputWindowOf[inputIndexValue];
                if (!leafWindow) {
                    continue;
                }
                sourceWindowHolder = *leafWindow;
                inputLocal = bounds[inputIndexValue].output;
            } else {
                const auto& inputWindow = outputWindowOf[inputIndexValue];
                if (inputIndex == kInvalidGpuSceneCommand || !inputWindow) {
                    continue;
                }
                sourceWindowHolder = *inputWindow;
                inputLocal = bounds[inputIndexValue].output;
            }
            // The CPU evaluator builds the matrix from the input's CONTENT-BOUNDS centre, composes
            // the parent matrix (which retains shear and inherits neither visibility nor opacity),
            // and records it for every layer whether or not the layer is active. The layer's own
            // published bounds are only written inside its active range, exactly as on the CPU.
            const auto centre = inputLocal.centre();
            const auto authoredMatrix = detail::LayerMatrix::authored(
                position->value, {centre.x + anchor->value.x, centre.y + anchor->value.y},
                scale->value, rotation->value);
            detail::LayerMatrix composed = authoredMatrix;
            if (layer->parent) {
                const auto parentIndex = layer->parent->value();
                if (parentIndex >= operationCount || !layerMatrices[parentIndex].has_value()) {
                    return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                  "Layer parent matrix is unavailable");
                }
                composed = layerMatrices[parentIndex]->times(authoredMatrix);
            }
            layerMatrices[index] = composed;
            if (inputVec.has_value()) {
                vectors[index] =
                    GpuSceneVectorChain{inputVec->source, composed.times(inputVec->matrix),
                                        inputVec->opacity * opacity->value};
            }
            // The CPU Layer Output stage publishes no image and no bounds outside its active range.
            if (request.time < layer->inPoint ||
                (layer->outPoint.has_value() && request.time >= *layer->outPoint)) {
                continue;
            }
            if (scale->value.x == 0.0 || scale->value.y == 0.0 || inputLocal.empty()) {
                continue;
            }
            auto& geometry = bounds[index];
            geometry.layerId = layer->layerId;
            geometry.local = inputLocal;
            const auto sourceWindow = *sourceWindowHolder;
            const auto bufferCentre =
                detail::boundsForWindow(sourceWindow, hScale, vScale).centre();
            render::LayerTransform::Authored authored{
                .anchorX = anchor->value.x,
                .anchorY = anchor->value.y,
                .scaleX = scale->value.x,
                .scaleY = scale->value.y,
                .rotationDegrees = rotation->value,
                .opacity = opacity->value,
            };
            authored.translationX = position->value.x - centre.x - anchor->value.x;
            authored.translationY = position->value.y - centre.y - anchor->value.y;
            authored.anchorX = centre.x - bufferCentre.x + anchor->value.x;
            authored.anchorY = centre.y - bufferCentre.y + anchor->value.y;
            if (!std::isfinite(authored.translationX * hScale) ||
                !std::isfinite(authored.translationY * vScale)) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Layer position produces a non-finite translation");
            }
            const auto transform =
                render::LayerTransform::create(authored, sourceWindow, hScale, vScale);
            if (!transform) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Layer transform is not evaluable");
            }
            std::optional<detail::ParentedLayerTransform> parented;
            if (layer->parent) {
                parented.emplace(composed, sourceWindow, hScale, vScale,
                                 static_cast<float>(opacity->value));
                if (!parented->finite()) {
                    return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                  "Parented transform exceeds the supported numeric range");
                }
                if (!parented->invertible()) {
                    continue;
                }
            }
            const auto workingWindow =
                render::ImageWindow::create(-16777216, -16777216, 33554432, 33554432);
            const auto layerWindow = parented
                                         ? parented->supportBounds(*workingWindow.value())
                                         : transform.value()->supportBounds(*workingWindow.value());
            if (!layerWindow) {
                continue;
            }
            const auto& transformValue = *transform.value();
            const auto forward = [&](const double x, const double y) {
                return parented ? parented->forwardMap(x, y) : transformValue.forwardMap(x, y);
            };
            const auto map = [&](const document::Vec2d point) {
                const auto mapped =
                    forward(point.x * hScale - 0.5 - static_cast<double>(sourceWindow.originX()),
                            point.y * vScale - 0.5 - static_cast<double>(sourceWindow.originY()));
                return document::Vec2d{(mapped.x + 0.5) / hScale, (mapped.y + 0.5) / vScale};
            };
            geometry.polygon = detail::boundsCorners(geometry.local);
            for (auto& point : geometry.polygon) {
                point = map(point);
            }
            geometry.output = {geometry.polygon[0].x, geometry.polygon[0].y, geometry.polygon[0].x,
                               geometry.polygon[0].y};
            for (const auto point : geometry.polygon) {
                geometry.output.left = std::min(geometry.output.left, point.x);
                geometry.output.top = std::min(geometry.output.top, point.y);
                geometry.output.right = std::max(geometry.output.right, point.x);
                geometry.output.bottom = std::max(geometry.output.bottom, point.y);
            }
            geometry.anchor = map({centre.x + anchor->value.x, centre.y + anchor->value.y});
            outputWindowOf[index] = *layerWindow;

            const auto isNativeGrid = [&](const detail::LayerMatrix& matrix) {
                return matrix.a == 1 && matrix.b == 0 && matrix.c == 0 && matrix.d == 1 &&
                       matrix.x * hScale == std::floor(matrix.x * hScale) &&
                       matrix.y * vScale == std::floor(matrix.y * vScale);
            };
            const bool nativeGrid = isNativeGrid(composed);
            // The full AUTHOR-space chain matrix the CPU rasterizes through: the enclosing layers'
            // composed matrix multiplied into the input's own chain. For a direct leaf this is just
            // the layer's own matrix.
            const detail::LayerMatrix chainMatrix =
                inputVec.has_value() ? vectors[index]->matrix : composed;

            if (inputVec.has_value()) {
                // A vector layer (direct leaf or a chain through enclosing layers) is rasterized by
                // the CPU evaluator's PathRaster through the FULL composed matrix, never resampled
                // as an intermediate raster. A direct solid on the integer grid keeps the exact
                // optimized translation command; every other vector layer emits coverage.
                const std::size_t chainSource = inputVec->source;
                const auto& leafOperation = plan->operations()[chainSource];
                const bool direct = chainSource == inputIndexValue;
                if (direct && !layer->parent &&
                    std::holds_alternative<CompiledSolid>(leafOperation) && nativeGrid) {
                    const auto delta = detail::translationOnlyDeviceDelta(
                        transformValue, sourceWindow, *layerWindow);
                    if (!delta.has_value()) {
                        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                      "Translation-only transform lost its device translation");
                    }
                    if (const auto error = detail::emitTranslationOpacityCommand(
                            inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow,
                            delta->first, delta->second, opacity->value, fullPixelAspect,
                            operationIndex, chargeTransient, emit, commandForOperation[index],
                            keyOf[index])) {
                        return failed(error->code, error->message);
                    }
                    continue;
                }
                GpuSceneCommandIndex composedIndex = kInvalidGpuSceneCommand;
                std::string semanticKey;
                bool consumedTextLeaf = false;
                bool consumedShapeLeaf = false;
                const bool leafPath = direct && nativeGrid;
                if (const auto error = detail::emitVectorLeafLayer(
                        leafOperation, *plan, resolved, chainMatrix, *layerWindow,
                        fullDisplayWindow, fullPixelAspect, hScale, vScale,
                        inputVec->opacity * opacity->value, leafPath && nativeGrid, allowance,
                        operationIndex, &transformValue, coverageCache_, cancellation, emit,
                        chargeTransient, chargeCoverage, composedIndex, semanticKey,
                        consumedTextLeaf, consumedShapeLeaf)) {
                    return failed(error->code, error->message);
                }
                keyOf[index] = semanticKey;
                commandForOperation[index] = composedIndex;
                textConsumed[chainSource] = textConsumed[chainSource] || consumedTextLeaf;
                shapeConsumed[chainSource] = shapeConsumed[chainSource] || consumedShapeLeaf;
                continue;
            }

            // A raster input (media, merge, or another raster layer). The exact translation path is
            // retained when it applies; every other affine appears on the accepted GpuAffine
            // command so rotation, nonuniform/signed scale, anchor and any parent-composed shear
            // are exact.
            if (!layer->parent) {
                if (const auto delta = detail::translationOnlyDeviceDelta(
                        transformValue, sourceWindow, *layerWindow)) {
                    if (const auto error = detail::emitTranslationOpacityCommand(
                            inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow,
                            delta->first, delta->second, opacity->value, fullPixelAspect,
                            operationIndex, chargeTransient, emit, commandForOperation[index],
                            keyOf[index])) {
                        return failed(error->code, error->message);
                    }
                    continue;
                }
            }
            if (const auto error = detail::emitAffineLayerCommand(
                    inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow, composed,
                    hScale, vScale, opacity->value, fullPixelAspect, operationIndex,
                    chargeTransient, emit, commandForOperation[index], keyOf[index])) {
                return failed(error->code, error->message);
            }
            continue;
        }

        if (const auto* stack = std::get_if<CompiledMerge>(&operation)) {
            if (const auto error = detail::emitMergeCommand(
                    *stack, *plan, resolved, index, bounds, outputWindowOf, commandForOperation,
                    keyOf, operationIndex, emit, chargeTransient)) {
                return failed(error->code, error->message);
            }
            continue;
        }

        if (const auto* output = std::get_if<CompiledCompositionOutput>(&operation)) {
            if (const auto error = detail::emitCompositionOutputCommand(
                    *output, resolved, index, operationIndex, emit, chargeTransient,
                    commandForOperation, keyOf, bounds)) {
                return failed(error->code, error->message);
            }
            continue;
        }

        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Reachable operation was not handled");
    }

    // A reachable vector leaf that no prepared layer consumed has no representation in this subset;
    // fail closed rather than publish a scene whose output silently omits it.
    if (detail::hasUnconsumedVectorLeaf(*plan, reachable, textConsumed, shapeConsumed)) {
        return failed(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                      "A vector source not consumed by a prepared layer is not prepared");
    }

    ProcessFrameIdentity identity{
        .plan = plan,
        .time = request.time,
        .output = request.output,
        .resolution = request.resolution,
        .quality = request.quality,
        .colorIntent = request.colorIntent,
        .provider = EvaluationProvider::CpuReference,
        .evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion,
        .animationSamplingSemanticsVersion = plan->animationSamplingSemanticsVersion(),
        .imagePrimitiveSemanticsVersion = render::kCpuImagePrimitiveSemanticsVersion,
        .roi = request.roi,
        .bypassLookNodes = request.bypassLookNodes,
    };
    const GpuSceneCommandIndex outputCommand = commandForOperation[request.output.value()];
    auto scene = std::shared_ptr<const PreparedGpuScene>(new PreparedGpuScene(
        std::move(commands), std::move(commandForOperation), outputCommand, std::move(identity),
        std::move(bounds), resolved.imageDescriptor, mediaStatistics));
    return {std::move(scene), PreparedGpuSceneDiagnostic{}};
}

} // namespace bloom::runtime
