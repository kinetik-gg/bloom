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

#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_vector_emission.hpp"
#include "input_color_context.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/media/image.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

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
            if constexpr (std::is_same_v<T, GpuSceneTranslationCommand> ||
                          std::is_same_v<T, GpuSceneAffineCommand>) {
                const auto source = gpuSceneCommandDescriptorOf(commands, item.input, depth + 1);
                if (!source.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{item.outputWindow, source->display,
                                                 source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneBlendCommand>) {
                const auto backdrop =
                    gpuSceneCommandDescriptorOf(commands, item.destination, depth + 1);
                if (!backdrop.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{item.outputWindow, backdrop->display,
                                                 backdrop->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneOcioEffectCommand>) {
                return GpuSceneCommandDescriptor{item.outputWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneCompositionOutputCommand> ||
                                 std::is_same_v<T, GpuSceneSolidCommand>) {
                return GpuSceneCommandDescriptor{item.dataWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneMergeCommand> ||
                                 std::is_same_v<T, GpuSceneCoverageSolidCommand>) {
                return GpuSceneCommandDescriptor{item.outputWindow, item.displayWindow,
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
};

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
    const auto* cst = std::get_if<CstKernel>(&effect.kernel);
    const auto* file = std::get_if<FileTransformKernel>(&effect.kernel);
    if (cst == nullptr && file == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "An image effect kernel is not a supported colour transform");
    }

    const std::string working{request.colorIntent.workingColorSpaceId};
    const auto cancellationProbe = [&cancellation] {
        return cancellation.isCancellationRequested();
    };

    // Resolve the exact CPU chain for this kernel, short-circuiting every case the CPU treats as an
    // identity passthrough before any geometry or GPU work happens.
    std::vector<GpuOcioTransformSpec> legs;
    if (cst != nullptr) {
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
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(inputWindow->extent().width()),
        .height = static_cast<std::uint32_t>(inputWindow->extent().height())};
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
        if (!vectors[inputValue].has_value()) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A non-identity image effect input is not materializable");
        }
        const auto& chain = *vectors[inputValue];
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
    const auto outputWindow = descriptor->data;

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
    result.local = bounds[inputValue].output;
    result.output = result.local;
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP
