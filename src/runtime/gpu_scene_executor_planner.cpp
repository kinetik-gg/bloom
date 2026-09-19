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

using gpu_scene_executor_detail::cachedDescriptorMatches;
using gpu_scene_executor_detail::checkedImageBytes;
using gpu_scene_executor_detail::commandKey;
using gpu_scene_executor_detail::descriptorMatches;
using gpu_scene_executor_detail::expectedDescriptorOf;
using gpu_scene_executor_detail::makeDiagnostic;

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
    const std::string& key = commandKey(command);

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
        if (!checkedImageBytes(covered->outputWindow, bytes) || covered->coverage == nullptr) {
            color[index] = 2;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::MalformedDescriptor,
                                  "a covered solid command is incomplete");
        }
        const std::uint64_t expected =
            static_cast<std::uint64_t>(covered->outputWindow.extent().width()) *
            static_cast<std::uint64_t>(covered->outputWindow.extent().height());
        if (covered->coverage->size() != expected) {
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
        step.coverage =
            std::span<const std::uint8_t>(covered->coverage->data(), covered->coverage->size());
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
