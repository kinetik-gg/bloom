#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_scene_executor_private.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::affineFieldsFinite;
using gpu_scene_executor_detail::cachedDescriptorMatches;
using gpu_scene_executor_detail::checkedImageBytes;
using gpu_scene_executor_detail::descriptorMatches;
using gpu_scene_executor_detail::effectiveCommandKey;
using gpu_scene_executor_detail::expectedDescriptorOf;
using gpu_scene_executor_detail::makeDiagnostic;

// The ACTUAL BlendV1 artifact token, delegated to the render-owned canonical identity. It selects
// the pipeline the native op really built from the device's shaderFloat64 capability and the
// GpuBlend kernel policy: exact Float32 for Normal/Add, the exact Float64 companion for the six
// general modes when selected, and the portable compensated-Float32 kernel otherwise. A
// producer-supplied `artifactDigest` is never consulted, so an adversarially wrong digest still
// cannot point the cache at another shader's output.
[[nodiscard]] std::string actualBlendArtifactToken(const render::GpuBlend& blend,
                                                   const core::BlendMode mode) {
    return std::string(blend.shaderIdentity(mode));
}

// Effective, executor-owned semantic key, and the single authoritative BlendV1 artifact selection.
// Blend commands are keyed here from the actual portable/Float64/Float32 pipeline; every other
// command keeps the shared canonicalization. The blend input keys and geometry are still recomputed
// from declared fields, never trusted from the producer.
[[nodiscard]] std::string effectiveCommandKey(const GpuSceneCommand& command,
                                              const render::GpuBlend* blend) {
    if (const auto* blendCommand = std::get_if<GpuSceneBlendCommand>(&command)) {
        if (blend == nullptr) {
            return {};
        }
        return makeGpuSceneBlendSemanticKey(blendCommand->sourceKey, blendCommand->destinationKey,
                                            blendCommand->mode, blendCommand->sourceWindow,
                                            blendCommand->outputWindow, blendCommand->pixelAspect,
                                            actualBlendArtifactToken(*blend, blendCommand->mode));
    }
    return effectiveCommandKey(command);
}

} // namespace

// Validate the reachable command DAG from the terminal output and flatten it into typed native
// steps, consulting the content cache first (a hit cuts the whole subtree). Cache hits are pinned
// into the live ledger immediately; produced commands are only assigned at completion.
GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::planCommand(const GpuSceneCommandIndex index,
                                                               std::vector<std::uint8_t>& color) {
    const std::size_t count = scene->commands().size();
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= count) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidReference,
                              "the scene references a missing command");
    }
    if (color[index] == 2) {
        return {};
    }
    if (color[index] == 1) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::CycleDetected,
                              "the scene command graph contains a cycle");
    }
    color[index] = 1;
    const GpuSceneCommand& command = scene->commands()[index];
    const bool isOutput = index == scene->outputCommand();
    const std::string key = effectiveCommandKey(command, blend.get());

    if (auto hit = cache->find(key)) {
        if (!hit->isValid() || !hit->isBoundTo(*device)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::CacheForeignImage,
                                  "a cache hit is not bound to this device generation");
        }
        const bool acceptable = isOutput ? descriptorMatches(command, *hit, nullptr)
                                         : cachedDescriptorMatches(command, *hit);
        if (acceptable) {
            assignImage(index, std::move(hit));
            ++counters.commandsServedFromCache;
            if (isOutput) {
                ++counters.outputCacheHits;
            }
            ++counters.commandCacheHits;
            color[index] = 2;
            return {};
        }
        static_cast<void>(cache->erase(key));
    }

    ++counters.commandCacheMisses;
    if (isOutput) {
        ++counters.outputCacheMisses;
    }

    if (const auto* solidCommand = std::get_if<GpuSceneSolidCommand>(&command)) {
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(solidCommand->dataWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a solid command has an empty window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Solid;
        step.command = index;
        step.cacheKey = solidCommand->semanticKey;
        step.cacheOnComplete = true;
        step.solidPixel = solidCommand->pixel;
        step.solidDataWindow = solidCommand->dataWindow;
        step.solidDisplayWindow = solidCommand->displayWindow;
        step.solidPixelAspect = solidCommand->pixelAspect;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* covered = std::get_if<GpuSceneCoverageSolidCommand>(&command)) {
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(covered->outputWindow, bytes) ||
            (covered->geometry == nullptr && covered->coverage == nullptr)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a covered solid command is incomplete");
        }
        const std::uint64_t width =
            static_cast<std::uint64_t>(covered->outputWindow.extent().width());
        const std::uint64_t height =
            static_cast<std::uint64_t>(covered->outputWindow.extent().height());
        const std::uint64_t expected = width * height;
        if (covered->geometry != nullptr) {
            const auto& geometry = *covered->geometry;
            if (geometry.width != width || geometry.height != height ||
                geometry.rows.size() != height * 4ULL) {
                color[index] = 2;
                return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                      "a covered solid geometry does not match its window");
            }
        } else if (covered->coverage->size() != expected) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a covered solid coverage size does not match its window");
        }
        if (coverageBytes > budgets.maxCoverageBytes ||
            expected > budgets.maxCoverageBytes - coverageBytes) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                                  "the scene coverage bytes exceed the configured ceiling");
        }
        coverageBytes += expected;
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::CoveredSolid;
        step.command = index;
        step.cacheKey = covered->semanticKey;
        step.cacheOnComplete = true;
        step.solidPixel = covered->pixel;
        step.solidDataWindow = covered->outputWindow;
        step.solidDisplayWindow = covered->displayWindow;
        step.solidPixelAspect = covered->pixelAspect;
        step.coverageGeometry = covered->geometry;
        step.hostCoverage = covered->coverage;
        step.coveredOpacity = covered->opacity;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* uploadCommand = std::get_if<GpuSceneUploadCommand>(&command)) {
        if (uploadCommand->image == nullptr) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an upload command has no converted source image");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(uploadCommand->descriptor.dataWindow(), bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an upload command has an empty data window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Upload;
        step.command = index;
        step.cacheKey = uploadCommand->semanticKey;
        step.cacheOnComplete = true;
        step.uploadSource = uploadCommand->image;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* translation = std::get_if<GpuSceneTranslationCommand>(&command)) {
        if (const auto plan = planCommand(translation->input, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (color[translation->input] != 2) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a translation input was not planned");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(translation->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a translation command has an empty window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Translation;
        step.command = index;
        step.input = translation->input;
        step.cacheKey = translation->semanticKey;
        step.cacheOnComplete = true;
        step.outputWindow = translation->outputWindow;
        step.translationX = translation->translationX;
        step.translationY = translation->translationY;
        step.translationOpacity = translation->opacity;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* affineCommand = std::get_if<GpuSceneAffineCommand>(&command)) {
        if (!affineFieldsFinite(*affineCommand)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an affine command has a non-finite matrix or opacity");
        }
        if (const auto plan = planCommand(affineCommand->input, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (color[affineCommand->input] != 2) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "an affine input was not planned");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(affineCommand->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an affine command has an empty output window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Affine;
        step.command = index;
        step.input = affineCommand->input;
        step.cacheKey = key;
        step.cacheOnComplete = true;
        step.outputWindow = affineCommand->outputWindow;
        step.affineMatrix = affineCommand->matrix;
        step.affineOpacity = affineCommand->opacity;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* blendCommand = std::get_if<GpuSceneBlendCommand>(&command)) {
        if (const auto plan = planCommand(blendCommand->source, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (const auto plan = planCommand(blendCommand->destination, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (color[blendCommand->source] != 2 || color[blendCommand->destination] != 2) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a blend input was not planned");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(blendCommand->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a blend command has an empty output window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Blend;
        step.command = index;
        step.input = blendCommand->source;
        step.destination = blendCommand->destination;
        step.cacheKey = key;
        step.cacheOnComplete = true;
        step.outputWindow = blendCommand->outputWindow;
        step.blendMode = blendCommand->mode;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* ocioCommand = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
        if (ocioCommand->program == nullptr ||
            ocioCommand->program->encoding() != GpuOcioOutputEncoding::FinalRgba32f) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported,
                                  "an OCIO effect command has no ProcessEffect program");
        }
        if (const auto plan = planCommand(ocioCommand->input, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (color[ocioCommand->input] != 2) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "an OCIO effect input was not planned");
        }
        const auto inputDescriptor = expectedDescriptorOf(*scene, ocioCommand->input, 0);
        if (!inputDescriptor.has_value()) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "the OCIO effect input descriptor is not derivable");
        }
        const auto geometry = ocioCommand->program->geometry();
        const bool sameGeometry = inputDescriptor->data == ocioCommand->outputWindow &&
                                  inputDescriptor->display == ocioCommand->displayWindow &&
                                  inputDescriptor->pixelAspect == ocioCommand->pixelAspect &&
                                  ocioCommand->outputWindow.extent().width() == geometry.width &&
                                  ocioCommand->outputWindow.extent().height() == geometry.height;
        if (!sameGeometry) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an OCIO effect command geometry does not match its input");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(ocioCommand->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "an OCIO effect command has an empty output window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::OcioEffect;
        step.command = index;
        step.input = ocioCommand->input;
        step.cacheKey = key;
        step.cacheOnComplete = true;
        step.outputWindow = ocioCommand->outputWindow;
        step.ocioCommand = ocioCommand->program;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* resample = std::get_if<GpuScenePointResampleCommand>(&command)) {
        if (!gpu_scene_executor_detail::pointResampleFieldsValid(*resample)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a point-resample command has invalid proxy scales");
        }
        if (const auto plan = planCommand(resample->input, color);
            plan.code != GpuSceneExecutorDiagnosticCode::None) {
            return plan;
        }
        if (color[resample->input] != 2) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a point-resample input was not planned");
        }
        const auto inputDescriptor = expectedDescriptorOf(*scene, resample->input, 0);
        if (!inputDescriptor.has_value()) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "the point-resample input descriptor is not derivable");
        }
        // The output window must be exactly the derived proxy window; the source window and pixel
        // aspect must match the input; the display window is carried explicitly (the resample
        // publishes a new display window, so it need not equal the source's). This keeps a
        // malformed command from resampling to another grid or mismatching the composition.
        const auto expectedOutput = render::ImageWindow::create(
            0, 0,
            static_cast<std::uint64_t>(std::max(
                1.0, std::ceil(static_cast<double>(inputDescriptor->data.extent().width()) *
                               resample->horizontalScale))),
            static_cast<std::uint64_t>(std::max(
                1.0, std::ceil(static_cast<double>(inputDescriptor->data.extent().height()) *
                               resample->verticalScale))));
        const bool geometryMatches = static_cast<bool>(expectedOutput) &&
                                     resample->outputWindow == *expectedOutput.value() &&
                                     resample->sourceWindow == inputDescriptor->data &&
                                     resample->pixelAspect == inputDescriptor->pixelAspect;
        if (!geometryMatches) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a point-resample command geometry does not match its input");
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(resample->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a point-resample command has an empty output window");
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::PointResample;
        step.command = index;
        step.input = resample->input;
        step.cacheKey = key;
        step.cacheOnComplete = true;
        step.outputWindow = resample->outputWindow;
        step.pointResampleHorizontalScale = resample->horizontalScale;
        step.pointResampleVerticalScale = resample->verticalScale;
        const auto outputDescriptor = render::Rgba32fImageDescriptor::create(
            resample->outputWindow, resample->displayWindow, resample->pixelAspect);
        if (!outputDescriptor) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a point-resample descriptor is invalid");
        }
        step.pointResampleOutput = *outputDescriptor.value();
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    if (const auto* merge = std::get_if<GpuSceneMergeCommand>(&command)) {
        for (const GpuSceneCommandIndex foreground : merge->foregrounds) {
            if (const auto plan = planCommand(foreground, color);
                plan.code != GpuSceneExecutorDiagnosticCode::None) {
                return plan;
            }
        }
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(merge->outputWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a merge command has an empty window");
        }
        GpuSceneExecutorStep base;
        base.kind = GpuSceneExecutorStepKind::Solid;
        base.command = index;
        base.solidPixel = render::Rgba32f::transparent();
        base.solidDataWindow = merge->outputWindow;
        base.solidDisplayWindow = merge->displayWindow;
        base.solidPixelAspect = merge->pixelAspect;
        base.cacheOnComplete = merge->foregrounds.empty();
        if (base.cacheOnComplete) {
            base.cacheKey = merge->semanticKey;
        }
        steps.push_back(std::move(base));
        for (std::size_t i = 0; i < merge->foregrounds.size(); ++i) {
            GpuSceneExecutorStep step;
            step.kind = GpuSceneExecutorStepKind::SourceOver;
            step.command = index;
            step.input = merge->foregrounds[i];
            step.destination = kInvalidGpuSceneCommand; // the current accumulator at `index`
            const bool last = i + 1 == merge->foregrounds.size();
            step.cacheOnComplete = last;
            if (last) {
                step.cacheKey = merge->semanticKey;
            }
            steps.push_back(std::move(step));
        }
        color[index] = 2;
        return {};
    }

    if (const auto* output = std::get_if<GpuSceneCompositionOutputCommand>(&command)) {
        std::uint64_t bytes = 0;
        if (!checkedImageBytes(output->dataWindow, bytes)) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "the composition output has an empty window");
        }
        if (output->input != kInvalidGpuSceneCommand) {
            if (const auto plan = planCommand(output->input, color);
                plan.code != GpuSceneExecutorDiagnosticCode::None) {
                return plan;
            }
            const auto inputDescriptor = expectedDescriptorOf(*scene, output->input, 0);
            if (!inputDescriptor.has_value()) {
                color[index] = 2;
                return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                      "the composition input descriptor is not derivable");
            }
            const bool sameGeometry = inputDescriptor->display == output->displayWindow &&
                                      inputDescriptor->pixelAspect == output->pixelAspect;
            if (sameGeometry) {
                GpuSceneExecutorStep step;
                step.kind = GpuSceneExecutorStepKind::Translation;
                step.command = index;
                step.input = output->input;
                step.cacheKey = output->semanticKey;
                step.cacheOnComplete = true;
                step.outputWindow = output->dataWindow;
                step.translationX = static_cast<double>(inputDescriptor->data.originX()) -
                                    static_cast<double>(output->dataWindow.originX());
                step.translationY = static_cast<double>(inputDescriptor->data.originY()) -
                                    static_cast<double>(output->dataWindow.originY());
                step.translationOpacity = 1.0F;
                steps.push_back(std::move(step));
            } else {
                GpuSceneExecutorStep base;
                base.kind = GpuSceneExecutorStepKind::Solid;
                base.command = index;
                base.solidPixel = render::Rgba32f::transparent();
                base.solidDataWindow = output->dataWindow;
                base.solidDisplayWindow = output->displayWindow;
                base.solidPixelAspect = output->pixelAspect;
                steps.push_back(std::move(base));
                GpuSceneExecutorStep step;
                step.kind = GpuSceneExecutorStepKind::SourceOver;
                step.command = index;
                step.input = output->input;
                step.destination = kInvalidGpuSceneCommand;
                step.cacheKey = output->semanticKey;
                step.cacheOnComplete = true;
                steps.push_back(std::move(step));
            }
            color[index] = 2;
            return {};
        }
        GpuSceneExecutorStep step;
        step.kind = GpuSceneExecutorStepKind::Solid;
        step.command = index;
        step.cacheKey = output->semanticKey;
        step.cacheOnComplete = true;
        step.solidPixel = render::Rgba32f::transparent();
        step.solidDataWindow = output->dataWindow;
        step.solidDisplayWindow = output->displayWindow;
        step.solidPixelAspect = output->pixelAspect;
        steps.push_back(std::move(step));
        color[index] = 2;
        return {};
    }

    color[index] = 2;
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported,
                          "a reachable command is outside the executor's subset");
}

} // namespace bloom::runtime
