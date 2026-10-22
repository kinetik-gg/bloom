#ifndef BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP

// Private to src/runtime. The image-effect scene-command emission the shared builder in
// cpu_gpu_scene_preparation.cpp delegates to.
//
// CPU semantics reproduced here (see detail::ImageEffectContext / applyImageEffect):
//  * bypass, an IdentityImageKernel, a look-tagged effect under request.bypassLookNodes, a CST
//  whose
//    resolved from/to ids are equal, a file transform the CPU would refuse (missing/invalid LUT
//    asset, unreadable bytes, a changed digest, or an unbuildable/identity CPU LUT processor), and
//    an effect whose input publishes no image are all EXACT identity: the output aliases the input
//    command, key, window and bounds, and a deferred vector chain propagates unchanged.
//  * a non-identity CST emits one real GPU OCIO ProcessEffect command. A file transform emits the
//    CPU's exact chain -- working -> process CST, the LUT, process -> working CST -- with the CST
//    legs omitted when the process space is the working space. If its input is a deferred vector
//    leaf, that leaf is first materialized as the accepted coverage command the CPU leaf arm would
//    rasterize, so no vector leaf is ever silently dropped.
//  * any preparation refusal other than a genuine identity fails closed (Unsupported) so the caller
//    takes the existing CPU reference path; it is never substituted with an identity result.

#include "canvas_image_effect.hpp"
#include "discrete_image_effect.hpp"
#include "gpu_scene_layer_emission.hpp"
#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_vector_emission.hpp"
#include "input_color_context.hpp"
#include "layer_parent_transform.hpp"
#include "pointwise_image_effect.hpp"
#include "resize_fit_geometry.hpp"
#include "separable_image_effect.hpp"

#include <bloom/media/image.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

// The evaluator's vector chain: a solid/text/shape leaf, the composed author-space matrix every
// enclosing layer multiplied into it, and the accumulated layer opacity. Defined here so the effect
// emission can materialize a leaf and the builder orchestrator can propagate a chain through an
// identity effect.
struct GpuSceneVectorChain final {
    std::size_t source = 0;
    LayerMatrix matrix;
    double opacity = 1.0;
};

inline constexpr std::size_t kNoVectorLeaf = std::numeric_limits<std::size_t>::max();

// The descriptor a command's output image carries, derived from the already-emitted scene alone.
// Mirrors the executor's expectedDescriptorOf so a produced OCIO effect command is validated
// against the exact input geometry at execution time.
struct GpuSceneCommandDescriptor final {
    render::ImageWindow data;
    render::ImageWindow display;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
};

[[nodiscard]] inline std::optional<GpuSceneCommandDescriptor>
gpuSceneCommandDescriptorOf(const std::vector<GpuSceneCommand>& commands,
                            const GpuSceneCommandIndex index, const std::size_t depth) {
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= commands.size() ||
        depth > 64) {
        return std::nullopt;
    }
    return std::visit(
        [&](const auto& item) -> std::optional<GpuSceneCommandDescriptor> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, GpuSceneAffineCommand>) {
                if (item.outputDescriptor)
                    return GpuSceneCommandDescriptor{item.outputWindow,
                                                     item.outputDescriptor->displayWindow(),
                                                     item.outputDescriptor->pixelAspect()};
                const auto source = gpuSceneCommandDescriptorOf(commands, item.input, depth + 1);
                if (!source)
                    return std::nullopt;
                return GpuSceneCommandDescriptor{item.outputWindow, source->display,
                                                 source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneTranslationCommand>) {
                const auto source = gpuSceneCommandDescriptorOf(commands, item.input, depth + 1);
                if (!source.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{
                    item.outputWindow,
                    [&]() {
                        if constexpr (std::is_same_v<T, GpuSceneTranslationCommand>)
                            return item.outputDisplayWindow.value_or(source->display);
                        return source->display;
                    }(),
                    source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneBlendCommand>) {
                const auto backdrop =
                    gpuSceneCommandDescriptorOf(commands, item.destination, depth + 1);
                if (!backdrop.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{item.outputWindow, backdrop->display,
                                                 backdrop->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneOcioEffectCommand> ||
                                 std::is_same_v<T, GpuScenePointResampleCommand> ||
                                 std::is_same_v<T, GpuSceneMergeCommand> ||
                                 std::is_same_v<T, GpuSceneCoverageSolidCommand>) {
                return GpuSceneCommandDescriptor{item.outputWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneCompositionOutputCommand> ||
                                 std::is_same_v<T, GpuSceneSolidCommand>) {
                return GpuSceneCommandDescriptor{item.dataWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneUploadCommand>) {
                return GpuSceneCommandDescriptor{item.descriptor.dataWindow(),
                                                 item.descriptor.displayWindow(),
                                                 item.descriptor.pixelAspect()};
            } else {
                return std::nullopt;
            }
        },
        commands[index]);
}

// The builder state a single effect emission reads. Every pointer refers to the live build a
// cpu_gpu_scene_preparation.cpp loop owns; nothing here is shared or retained.
struct GpuSceneEffectEmissionContext final {
    const CompiledCompositionPlan* plan = nullptr;
    const EvaluationRequest* request = nullptr;
    const ResolvedEvaluation* resolved = nullptr;
    const std::vector<std::optional<GpuSceneVectorChain>>* vectors = nullptr;
    const std::vector<GpuSceneCommandIndex>* commandForOperation = nullptr;
    const std::vector<std::string>* keyOf = nullptr;
    const std::vector<std::optional<render::ImageWindow>>* outputWindowOf = nullptr;
    const std::vector<EvaluatedOperationBounds>* bounds = nullptr;
    const std::vector<GpuSceneCommand>* commands = nullptr;
    render::ImageWindow fullDisplayWindow;
    core::PixelAspectRatio fullPixelAspect = core::PixelAspectRatio::square();
    double hScale = 1.0;
    double vScale = 1.0;
    std::uint64_t allowance = 0;
    std::filesystem::path assetBaseDirectory;
    std::shared_ptr<GpuSceneCoverageCache> coverageCache;
};

struct GpuSceneEffectEmission final {
    // True when the CPU evaluation of this effect is exact identity: the caller aliases the input
    // command/key/window/bounds and propagates the deferred vector chain.
    bool identity = false;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    std::optional<render::ImageWindow> window;
    ContentBounds local;
    ContentBounds output;
    // The vector leaf a materialization consumed, or kNoVectorLeaf when the input was already a
    // resident command.
    std::size_t consumedLeaf = kNoVectorLeaf;
    bool consumedText = false;
    bool consumedShape = false;
    std::size_t consumedSecondLeaf = kNoVectorLeaf;
    bool consumedSecondText = false;
    bool consumedSecondShape = false;
};

#include "gpu_scene_resize_fit_emission.ipp"

// Emits the scene command(s) for one reachable CompiledImageEffect. Returns nullopt on success and
// a fail-closed failure otherwise.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitImageEffectCommand(const CompiledImageEffect& effect, const OperationIndex operationIndex,
                       const GpuSceneEffectEmissionContext& ctx,
                       const GpuSceneOcioContext& ocioContext,
                       const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                       ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    const auto& plan = *ctx.plan;
    const auto& request = *ctx.request;
    const auto& resolved = *ctx.resolved;
    const auto& vectors = *ctx.vectors;
    const auto& commandForOperation = *ctx.commandForOperation;
    const auto& keyOf = *ctx.keyOf;
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto& bounds = *ctx.bounds;
    const auto& commands = *ctx.commands;

    if (effect.input.value() >= plan.operations().size()) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Image effect input is invalid");
    }
    const std::size_t inputValue = effect.input.value();
    const auto identity = [&result]() {
        result.identity = true;
        return std::optional<GpuSceneLeafFailure>{};
    };

    // Exact CPU identity cases.
    if (effect.bypass || (request.bypassLookNodes && effect.look) ||
        std::holds_alternative<IdentityImageKernel>(effect.kernel)) {
        return identity();
    }
    if (const auto* discrete = std::get_if<DiscreteImageKernel>(&effect.kernel)) {
        if (discrete->quarterTurns > 3)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Invalid quarter turn");
        const auto inputWindow = outputWindowOf[inputValue];
        if (discrete->identity() || !inputWindow)
            return identity();
        // The CPU lattice supports int64 origins. The affine map is Float64 host metadata;
        // outside this exact-integer domain, use the explicit CPU fallback rather than round.
        constexpr std::int64_t exactOriginLimit = std::int64_t{1} << 52;
        if (inputWindow->originX() < -exactOriginLimit ||
            inputWindow->originY() < -exactOriginLimit ||
            inputWindow->maxXExclusive() > exactOriginLimit ||
            inputWindow->maxYExclusive() > exactOriginLimit)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                        "Discrete image origin exceeds exact GPU affine coordinates");
        const auto window = discreteOutputWindow(*discrete, *inputWindow);
        if (!window)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                        "Discrete image output window overflows");
        GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
        std::string inputKey = keyOf[inputValue];
        if (inputCommand == kInvalidGpuSceneCommand) {
            const auto& chain = vectors[inputValue];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Discrete image input cannot be materialized");
            if (const auto error = emitVectorLeafLayer(
                    plan.operations()[chain->source], plan, resolved, chain->matrix, *inputWindow,
                    ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale,
                    chain->opacity, false, ctx.allowance, operationIndex, nullptr,
                    ctx.coverageCache, cancellation, emit, charge, chargeCoverage, inputCommand,
                    inputKey, result.consumedText, result.consumedShape))
                return error;
            result.consumedLeaf = chain->source;
        }
        const auto matrix = discreteAffineMatrix(*discrete, *inputWindow);
        result.key =
            makeGpuSceneAffineSemanticKey(inputKey, *inputWindow, matrix, 1.0F, *window.value(),
                                          ctx.fullPixelAspect, std::string{kGpuAffineSpirvSha256});
        if (const auto error = charge(window.value()->extent().width(),
                                      window.value()->extent().height(), sizeof(render::Rgba32f)))
            return error;
        result.command = emit(GpuSceneAffineCommand{.sourceOperation = operationIndex,
                                                    .input = inputCommand,
                                                    .inputKey = inputKey,
                                                    .sourceWindow = *inputWindow,
                                                    .outputWindow = *window.value(),
                                                    .matrix = matrix,
                                                    .opacity = 1.0F,
                                                    .pixelAspect = ctx.fullPixelAspect,
                                                    .semanticKey = result.key});
        result.window = *window.value();
        result.local = {static_cast<double>(window.value()->originX()) / ctx.hScale,
                        static_cast<double>(window.value()->originY()) / ctx.vScale,
                        static_cast<double>(window.value()->maxXExclusive()) / ctx.hScale,
                        static_cast<double>(window.value()->maxYExclusive()) / ctx.vScale};
        result.output = result.local;
        return std::nullopt;
    }
    if (const auto* kernel = std::get_if<ResizeFitKernel>(&effect.kernel))
        return emitResizeFitCommand(*kernel, effect.input, operationIndex, ctx, cancellation, emit,
                                    charge, chargeCoverage, result);
    const auto* cst = std::get_if<CstKernel>(&effect.kernel);
    const auto* file = std::get_if<FileTransformKernel>(&effect.kernel);
    const auto* pointwise = std::get_if<PointwiseImageKernel>(&effect.kernel);
    const auto* combine = std::get_if<ImageCombineKernel>(&effect.kernel);
    if (combine != nullptr) {
        auto channel = static_cast<std::int64_t>(combine->channel);
        if (combine->drivenChannel) {
            const auto* sampled = resolvedValue<std::int64_t>(*combine->drivenChannel, resolved);
            if (sampled == nullptr)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Image combine channel driver is not an Integer");
            channel = *sampled;
        }
        if (!effect.secondaryInput || effect.secondaryInput->value() >= plan.operations().size() ||
            static_cast<std::uint8_t>(combine->mode) >
                static_cast<std::uint8_t>(ImageCombineMode::MultiplyAlpha) ||
            channel < 0 || channel > static_cast<std::int64_t>(render::ImageCoverageChannel::Luma))
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image combine kernel or second input is invalid");
        const auto secondValue = effect.secondaryInput->value();
        if (!outputWindowOf[inputValue])
            return identity();
        if (!outputWindowOf[secondValue])
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Image combine second input has no resident image");
        float factor = 1.0F;
        if (combine->factor) {
            const auto sampled = resolveParameter(*combine->factor, plan, resolved);
            if (!sampled || !std::isfinite(sampled->value) || sampled->value < 0.0 ||
                sampled->value > 1.0)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Image combine factor is outside its unit domain");
            factor = static_cast<float>(sampled->value);
        } else if (combine->mode != ImageCombineMode::SetAlpha) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image combine factor is missing");
        }
        if (combine->mode == ImageCombineMode::Mix && factor == 0.0F)
            return identity();

        // Both image inputs may be deferred vector leaves. Materialize each through the same
        // native coverage route the ordinary image-effect path uses before the two-input dispatch.
        const auto residentInput = [&](const std::size_t value, GpuSceneCommandIndex& command,
                                       std::string& key, std::size_t& consumedLeaf,
                                       bool& consumedText,
                                       bool& consumedShape) -> std::optional<GpuSceneLeafFailure> {
            command = commandForOperation[value];
            key = keyOf[value];
            if (command != kInvalidGpuSceneCommand)
                return std::nullopt;
            const auto& chain = vectors[value];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Image combine vector input is not materializable");
            const auto error = emitVectorLeafLayer(
                plan.operations()[chain->source], plan, resolved, chain->matrix,
                *outputWindowOf[value], ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale,
                ctx.vScale, chain->opacity, false, ctx.allowance, operationIndex, nullptr,
                ctx.coverageCache, cancellation, emit, charge, chargeCoverage, command, key,
                consumedText, consumedShape);
            if (!error)
                consumedLeaf = chain->source;
            return error;
        };
        GpuSceneCommandIndex baseCommand = kInvalidGpuSceneCommand;
        GpuSceneCommandIndex secondCommand = kInvalidGpuSceneCommand;
        std::string baseKey;
        std::string secondKey;
        if (const auto error = residentInput(inputValue, baseCommand, baseKey, result.consumedLeaf,
                                             result.consumedText, result.consumedShape))
            return error;
        if (const auto error =
                residentInput(secondValue, secondCommand, secondKey, result.consumedSecondLeaf,
                              result.consumedSecondText, result.consumedSecondShape))
            return error;
        const auto baseDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, baseCommand, 0);
        const auto secondDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, secondCommand, 0);
        if (!baseDescriptor || !secondDescriptor)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "Image combine resident descriptors are not derivable");
        const auto outputWindow = baseDescriptor->data;
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f)))
            return error;
        const render::GpuImageCombineParameters parameters{
            static_cast<render::GpuImageCombineKind>(combine->mode), factor,
            static_cast<render::ImageCoverageChannel>(channel)};
        GpuSceneBlendCommand command{.sourceOperation = operationIndex,
                                     .source = secondCommand,
                                     .destination = baseCommand,
                                     .sourceKey = secondKey,
                                     .destinationKey = baseKey,
                                     .mode = core::BlendMode::Normal,
                                     .imageCombine = parameters,
                                     .sourceWindow = secondDescriptor->data,
                                     .outputWindow = outputWindow,
                                     .pixelAspect = baseDescriptor->pixelAspect,
                                     .artifactDigest =
                                         std::string(render::gpuImageCombineShaderIdentity()),
                                     .semanticKey = {}};
        command.semanticKey = makeGpuSceneBlendSemanticKey(
            secondKey, baseKey, command.mode, command.sourceWindow, command.outputWindow,
            command.pixelAspect, command.artifactDigest, command.imageCombine);
        result.command = emit(std::move(command));
        result.key = makeGpuSceneBlendSemanticKey(
            secondKey, baseKey, core::BlendMode::Normal, secondDescriptor->data, outputWindow,
            baseDescriptor->pixelAspect, std::string(render::gpuImageCombineShaderIdentity()),
            parameters);
        result.window = outputWindow;
        // Mix can add pixels where only the second image has content; Set Alpha can assign
        // coverage to a transparent base pixel. Use the whole base data window as conservative
        // content bounds so downstream ROI never silently drops those pixels.
        result.local = boundsForWindow(outputWindow, ctx.hScale, ctx.vScale);
        result.output = result.local;
        return std::nullopt;
    }
    const auto* canvas = std::get_if<CanvasImageKernel>(&effect.kernel);
    const auto* blurKernel = std::get_if<SeparableBlurKernel>(&effect.kernel);
    std::optional<PreparedSeparableBlur> blur;
    if (cst == nullptr && file == nullptr && pointwise == nullptr && canvas == nullptr &&
        blurKernel == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "An image effect kernel is not supported");
    }

    if (canvas != nullptr) {
        std::array<double, 4> sides{};
        for (std::size_t i = 0; i < sides.size(); ++i) {
            const auto sampled = resolveParameter(canvas->margins[i], plan, resolved);
            if (!sampled)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Canvas margin is not evaluable");
            sides[i] = sampled->value;
        }
        const CanvasMargins margins{canvas->effect, sides};
        if (margins.identity() || !outputWindowOf[inputValue])
            return identity();
        const auto sourceWindow = *outputWindowOf[inputValue];
        const auto outputWindow = canvasOutputWindow(sourceWindow, margins, ctx.hScale, ctx.vScale);
        if (!outputWindow)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Canvas margins leave an invalid data window");
        GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
        std::string inputKey = keyOf[inputValue];
        if (inputCommand == kInvalidGpuSceneCommand) {
            const auto& chain = vectors[inputValue];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Canvas input vector chain is not materializable");
            if (const auto error = emitVectorLeafLayer(
                    plan.operations()[chain->source], plan, resolved, chain->matrix, sourceWindow,
                    ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale,
                    chain->opacity, false, ctx.allowance, operationIndex, nullptr,
                    ctx.coverageCache, cancellation, emit, charge, chargeCoverage, inputCommand,
                    inputKey, result.consumedText, result.consumedShape))
                return error;
            result.consumedLeaf = chain->source;
        }
        const auto descriptor = gpuSceneCommandDescriptorOf(commands, inputCommand, 0);
        if (!descriptor || descriptor->data != sourceWindow)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "Canvas input descriptor does not match its data window");
        const auto displayWindow =
            canvasOutputWindow(descriptor->display, margins, ctx.hScale, ctx.vScale);
        if (!displayWindow)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Canvas margins leave an invalid display window");
        const double deltaX = static_cast<double>(sourceWindow.originX() - outputWindow->originX());
        const double deltaY = static_cast<double>(sourceWindow.originY() - outputWindow->originY());
        OperationKey commandKey;
        commandKey.add(std::string{"gpu-canvas-translation-v1"});
        commandKey.add(inputKey);
        addWindowToKey(commandKey, sourceWindow);
        addWindowToKey(commandKey, *outputWindow);
        addWindowToKey(commandKey, *displayWindow);
        addPixelAspectToKey(commandKey, descriptor->pixelAspect);
        commandKey.add(deltaX);
        commandKey.add(deltaY);
        commandKey.add(std::string{kGpuTranslationOpacitySpirvSha256});
        result.key = commandKey.digest();
        if (const auto error = charge(outputWindow->extent().width(),
                                      outputWindow->extent().height(), sizeof(render::Rgba32f)))
            return error;
        GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                           .input = inputCommand,
                                           .sourceWindow = sourceWindow,
                                           .outputWindow = *outputWindow,
                                           .outputDisplayWindow = *displayWindow,
                                           .translationX = deltaX,
                                           .translationY = deltaY,
                                           .opacity = 1.0F,
                                           .semanticKey = result.key};
        result.command = emit(std::move(command));
        result.window = *outputWindow;
        result.local = boundsForWindow(*outputWindow, ctx.hScale, ctx.vScale);
        result.output = result.local;
        return std::nullopt;
    }

    const std::string working{request.colorIntent.workingColorSpaceId};
    const auto cancellationProbe = [&cancellation] {
        return cancellation.isCancellationRequested();
    };

    // Resolve the exact CPU chain for this kernel, short-circuiting every case the CPU treats as an
    // identity passthrough before any geometry or GPU work happens.
    std::vector<GpuOcioTransformSpec> legs;
    if (blurKernel != nullptr) {
        const auto sampled = resolveParameter(blurKernel->radius, plan, resolved);
        blur = prepareSeparableBlur(*blurKernel,
                                    sampled ? std::optional{sampled->value} : std::nullopt,
                                    ctx.hScale, ctx.vScale);
        if (!blur)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Blur radius is out of domain");
        if (blur->identity())
            return identity();
        for (unsigned axis = 0; axis < 2; ++axis) {
            GpuOcioTransformSpec leg;
            leg.kind = GpuOcioTransformKind::SeparableBlur;
            leg.blurWeights = axis == 0 ? blur->horizontal : blur->vertical;
            leg.blurVertical = axis == 1;
            leg.blurEdge = static_cast<std::uint8_t>(blur->edge);
            legs.push_back(std::move(leg));
        }
    } else if (pointwise != nullptr) {
        std::optional<double> amount;
        if (pointwise->amount.has_value()) {
            const auto sampled = resolveParameter(*pointwise->amount, plan, resolved);
            if (!sampled) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Pointwise effect amount is not evaluable");
            }
            amount = sampled->value;
        }
        const auto affine = pointwiseAffine(*pointwise, amount);
        if (!affine) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Pointwise effect amount cannot be represented");
        }
        if (affine->identity()) {
            return identity();
        }
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::PointwiseAffine;
        leg.affineScale = affine->scale;
        leg.affineOffset = affine->offset;
        legs.push_back(std::move(leg));
    } else if (cst != nullptr) {
        const std::string from = cst->fromId.empty() ? working : cst->fromId;
        const std::string to = cst->toId.empty() ? working : cst->toId;
        if (from == to) {
            return identity();
        }
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::Cst;
        leg.fromId = from;
        leg.toId = to;
        legs.push_back(std::move(leg));
    } else {
        if (!file->asset || file->asset->kind != document::AssetKind::Lut ||
            file->asset->id != file->lutAssetId) {
            // The CPU refuses a missing or mismatched LUT asset with a warning passthrough.
            return identity();
        }
        const auto path = media::resolveImagePath(
            file->asset->locator.path, file->asset->locator.relinkHint, ctx.assetBaseDirectory);
        auto resource = color::readLutFile(path, cancellationProbe);
        if (resource.error == color::LutError::HelperCancelled) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Image effect LUT read was cancelled");
        }
        if (resource.error != color::LutError::None ||
            resource.digest != file->asset->contentDigest) {
            // Unreadable bytes or a changed digest: the CPU refuses and passes the input through.
            return identity();
        }
        const auto interpolation = static_cast<color::LutInterpolation>(file->interpolation);
        const auto direction = static_cast<color::LutDirection>(file->direction);
        auto cpuLut = color::CpuFileTransformProcessor::prepare(resource, interpolation, direction,
                                                                cancellationProbe);
        if (cpuLut.error == color::LutError::HelperCancelled) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Image effect LUT preparation was cancelled");
        }
        if (cpuLut.error != color::LutError::None || cpuLut.processor == nullptr) {
            // The CPU cannot build the LUT processor and passes the input through.
            return identity();
        }
        if (cpuLut.processor->isIdentity()) {
            // The CPU resets every surrounding processor for a proven identity LUT: exact identity.
            return identity();
        }
        const std::string process = file->processSpaceId.empty() ? working : file->processSpaceId;
        if (process != working) {
            GpuOcioTransformSpec inLeg;
            inLeg.kind = GpuOcioTransformKind::Cst;
            inLeg.fromId = working;
            inLeg.toId = process;
            legs.push_back(std::move(inLeg));
        }
        GpuOcioTransformSpec fileLeg;
        fileLeg.kind = GpuOcioTransformKind::FileTransform;
        fileLeg.lutFile = std::make_shared<const color::LutFile>(std::move(resource));
        fileLeg.interpolation = interpolation;
        fileLeg.direction = direction;
        fileLeg.processSpaceId = process;
        fileLeg.workingSpaceId = working;
        legs.push_back(std::move(fileLeg));
        if (process != working) {
            GpuOcioTransformSpec outLeg;
            outLeg.kind = GpuOcioTransformKind::Cst;
            outLeg.fromId = process;
            outLeg.toId = working;
            legs.push_back(std::move(outLeg));
        }
    }

    // A residual input publishes no image and the CPU effect is a no-op (identity).
    const auto inputWindow = outputWindowOf[inputValue];
    if (!inputWindow.has_value()) {
        return identity();
    }
    if (ocioContext.preparer == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "No GPU OCIO preparer is configured for image effects");
    }
    const auto config = resolveInputColorConfig(request.colorIntent);
    if (!config.has_value()) {
        // The CPU refuses the transform and passes the input through; fail closed so the CPU path
        // reproduces that frame rather than substituting identity here.
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The OCIO configuration for the image effect is unavailable");
    }
    const auto filterWindow = blur ? blurOutputWindow(*blur, *inputWindow) : inputWindow;
    if (!filterWindow)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation, "Blur halo overflows");
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(filterWindow->extent().width()),
        .height = static_cast<std::uint32_t>(filterWindow->extent().height())};
    std::vector<std::shared_ptr<const PreparedGpuOcioCommand>> programs;
    programs.reserve(legs.size());
    for (const auto& leg : legs) {
        auto prepared = ocioContext.preparer->prepare(
            *config, leg, geometry, ocioContext.compileOptions, cancellationProbe);
        if (!prepared) {
            if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
                cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "GPU image-effect preparation was cancelled");
            }
            // Never substitute identity for a valid transform whose GPU program could not be built.
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "GPU image-effect preparation failed: " +
                            std::string{gpuOcioPreparationErrorName(prepared.error)});
        }
        programs.push_back(std::move(prepared.command));
    }
    if (programs.empty()) {
        return identity();
    }

    // The exact resident input command: an already-emitted raster, or a deferred vector leaf that
    // must be materialized through the accepted coverage commands first.
    GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
    std::string inputKey = keyOf[inputValue];
    if (inputCommand == kInvalidGpuSceneCommand) {
        const auto& vectorChain = vectors[inputValue];
        if (!vectorChain.has_value()) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A non-identity image effect input is not materializable");
        }
        const auto& chain = *vectorChain;
        if (chain.source >= plan.operations().size()) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "An image effect vector chain is invalid");
        }
        // Materialize the leaf at its own window with its chain matrix/opacity; nativeGrid is left
        // false so the general coverage arm handles every leaf kind without a layer transform.
        GpuSceneCommandIndex materialized = kInvalidGpuSceneCommand;
        std::string materializedKey;
        if (const auto error = emitVectorLeafLayer(
                plan.operations()[chain.source], plan, resolved, chain.matrix, *inputWindow,
                ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale, chain.opacity,
                false, ctx.allowance, operationIndex, nullptr, ctx.coverageCache, cancellation,
                emit, charge, chargeCoverage, materialized, materializedKey, result.consumedText,
                result.consumedShape)) {
            return error;
        }
        result.consumedLeaf = chain.source;
        inputCommand = materialized;
        inputKey = materializedKey;
    }

    const auto descriptor = gpuSceneCommandDescriptorOf(commands, inputCommand, 0);
    if (!descriptor.has_value()) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The image effect input descriptor is not derivable");
    }
    const auto outputWindow = blur ? *filterWindow : descriptor->data;
    if (blur && outputWindow != descriptor->data) {
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f)))
            return error;
        OperationKey paddingKey;
        paddingKey.add(inputKey);
        paddingKey.add(std::string("blur-transparent-halo-v1"));
        paddingKey.add(outputWindow.originX());
        paddingKey.add(outputWindow.originY());
        paddingKey.add(outputWindow.extent().width());
        paddingKey.add(outputWindow.extent().height());
        inputKey = paddingKey.bytes();
        inputCommand = emit(GpuSceneTranslationCommand{
            .index = kInvalidGpuSceneCommand,
            .sourceOperation = operationIndex,
            .input = inputCommand,
            .sourceWindow = descriptor->data,
            .outputWindow = outputWindow,
            .translationX =
                static_cast<double>(descriptor->data.originX() - outputWindow.originX()),
            .translationY =
                static_cast<double>(descriptor->data.originY() - outputWindow.originY()),
            .opacity = 1.0F,
            .semanticKey = inputKey});
    }

    // Emit the prepared legs as one resident RGBA32F chain. Each command's descriptor equals the
    // original input's, so every leg reuses `descriptor`.
    GpuSceneCommandIndex currentCommand = inputCommand;
    std::string currentKey = std::move(inputKey);
    for (auto& program : programs) {
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f))) {
            return error;
        }
        GpuSceneOcioEffectCommand command{.index = kInvalidGpuSceneCommand,
                                          .sourceOperation = operationIndex,
                                          .input = currentCommand,
                                          .inputKey = currentKey,
                                          .program = std::move(program),
                                          .outputWindow = outputWindow,
                                          .displayWindow = descriptor->display,
                                          .pixelAspect = descriptor->pixelAspect,
                                          .semanticKey = {}};
        command.semanticKey = makeGpuSceneOcioEffectSemanticKey(
            currentKey, command.program->identity(), command.outputWindow, command.pixelAspect);
        currentKey = command.semanticKey;
        currentCommand = emit(std::move(command));
    }
    result.command = currentCommand;
    result.key = std::move(currentKey);
    result.window = outputWindow;
    result.local =
        blur ? boundsForWindow(outputWindow, ctx.hScale, ctx.vScale) : bounds[inputValue].output;
    result.output = result.local;
    return std::nullopt;
}

// Emits and wires one reachable CompiledImageEffect into the builder's per-operation state.
// Identity effects alias the input command/key/window/bounds and keep the deferred vector chain; a
// non-identity effect publishes its OCIO command chain and marks any materialized leaf consumed.
// Keeping this here leaves the builder orchestrator within its size budget.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitImageEffectOperation(
    const CompiledImageEffect& effect, const OperationIndex operationIndex,
    const GpuSceneEffectEmissionContext& ctx, const GpuSceneOcioContext& ocioContext,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, std::vector<GpuSceneCommandIndex>& commandForOperation,
    std::vector<std::string>& keyOf,
    std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
    std::vector<EvaluatedOperationBounds>& bounds,
    std::vector<std::optional<GpuSceneVectorChain>>& vectors, std::vector<bool>& textConsumed,
    std::vector<bool>& shapeConsumed) {
    GpuSceneEffectEmission emitted;
    if (const auto error =
            emitImageEffectCommand(effect, operationIndex, ctx, ocioContext, cancellation, emit,
                                   charge, chargeCoverage, emitted)) {
        return error;
    }
    const std::size_t index = operationIndex.value();
    const std::size_t input = effect.input.value();
    if (emitted.identity) {
        commandForOperation[index] = commandForOperation[input];
        keyOf[index] = keyOf[input];
        outputWindowOf[index] = outputWindowOf[input];
        bounds[index].local = bounds[input].output;
        bounds[index].output = bounds[index].local;
        vectors[index] = vectors[input];
        return std::nullopt;
    }
    commandForOperation[index] = emitted.command;
    keyOf[index] = emitted.key;
    outputWindowOf[index] = emitted.window;
    bounds[index].local = emitted.local;
    bounds[index].output = emitted.output;
    if (emitted.consumedLeaf != kNoVectorLeaf) {
        if (emitted.consumedText) {
            textConsumed[emitted.consumedLeaf] = true;
        }
        if (emitted.consumedShape) {
            shapeConsumed[emitted.consumedLeaf] = true;
        }
    }
    if (emitted.consumedSecondLeaf != kNoVectorLeaf) {
        if (emitted.consumedSecondText)
            textConsumed[emitted.consumedSecondLeaf] = true;
        if (emitted.consumedSecondShape)
            shapeConsumed[emitted.consumedSecondLeaf] = true;
    }
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP
