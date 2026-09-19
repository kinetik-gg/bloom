#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_media_preparation.hpp"
#include "gpu_scene_coverage.hpp"
#include "gpu_scene_media_layer.hpp"
#include "gpu_scene_preparation_private.hpp"

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
    return {nullptr, PreparedGpuSceneDiagnostic{code, std::move(message), std::move(statistics)}};
}

[[nodiscard]] bool isSubsetOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation);
}

void addWindow(detail::OperationKey& key, const render::ImageWindow window) {
    key.add(window.originX());
    key.add(window.originY());
    key.add(window.extent().width());
    key.add(window.extent().height());
}

void addPixelAspect(detail::OperationKey& key, const core::PixelAspectRatio ratio) {
    key.add(ratio.numerator());
    key.add(ratio.denominator());
}

[[nodiscard]] std::string keyDigest(detail::OperationKey& key) { return key.digest(); }

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
        request.colorIntent.workingColorSpaceId != kLinearRec709SceneColorSpaceId ||
        request.colorIntent.workingColorSpaceId.find('\0') != std::string_view::npos ||
        request.roi) {
        return failed(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                      "Only Reference quality, lin_rec709_scene, and no ROI are prepared");
    }
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
    std::vector<bool> reachable(operationCount, false);
    std::vector<std::size_t> pending{request.output.value()};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operationCount) {
            return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                          "Plan references an invalid operation");
        }
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        detail::forEachInput(plan->operations()[index], [&pending](const OperationIndex input) {
            pending.push_back(input.value());
        });
    }
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (!reachable[index]) {
            continue;
        }
        const auto& operation = plan->operations()[index];
        if (!isSubsetOperation(operation)) {
            return failed(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                          "A reachable operation is outside the prepared subset");
        }
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation);
            layer && layer->parent) {
            return failed(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                          "A parented layer is not prepared");
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
        std::string message = checked.diagnostic.has_value() ? checked.diagnostic->summary
                                                             : std::string{"Preflight failed"};
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
    const double authorWidth = static_cast<double>(plan->format().width());
    const double authorHeight = static_cast<double>(plan->format().height());

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

    const auto emit = [&](GpuSceneCommand command) -> GpuSceneCommandIndex {
        const auto index = static_cast<GpuSceneCommandIndex>(commands.size());
        std::visit([index](auto& item) { item.index = index; }, command);
        commands.push_back(std::move(command));
        return index;
    };

    // Concurrently-held budget: every prepared command's resident output PLUS each unique coverage
    // raster (shared_ptr identity counts once, so a cache alias is not double charged) must fit the
    // request's pixel allowance. Overflow-checked with subtraction-only comparators. The coverage
    // cache's own retained-byte budget is separate ownership accounting and is not charged here.
    const std::uint64_t allowance = request.pixelStorageByteLimit;
    std::uint64_t chargedBytes = 0;
    std::unordered_set<const void*> countedCoverage;
    const auto charge =
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
        if (chargedBytes > allowance || *bytes > allowance - chargedBytes) {
            return detail::GpuSceneLeafFailure{
                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                "Prepared scene exceeds the request pixel allowance"};
        }
        chargedBytes += *bytes;
        return std::nullopt;
    };
    const auto chargeCoverage =
        [&](const std::shared_ptr<const std::vector<std::uint8_t>>& coverage,
            const std::uint64_t width,
            const std::uint64_t height) -> std::optional<detail::GpuSceneLeafFailure> {
        if (coverage == nullptr || !countedCoverage.insert(coverage.get()).second) {
            return std::nullopt;
        }
        return charge(width, height, sizeof(std::uint8_t));
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
            const auto color = detail::resolveParameter(solid->color, *plan, resolved);
            const auto width = detail::resolveParameter(solid->width, *plan, resolved);
            const auto height = detail::resolveParameter(solid->height, *plan, resolved);
            if (!color || !width || !height || width->value < 1.0 || height->value < 1.0) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Solid parameters are not evaluable");
            }
            const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color->value);
            if (!pixel) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Solid colour is not evaluable");
            }
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
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Solid dimensions exceed the supported extent");
            }
            const auto window = render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(w),
                                                            static_cast<std::uint64_t>(h));
            if (!window) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Solid bounds are invalid");
            }
            const auto descriptor = render::Rgba32fImageDescriptor::create(
                *window.value(), fullDisplayWindow, fullPixelAspect);
            if (!descriptor) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Solid descriptor is invalid");
            }
            const auto solidWindow = descriptor.value()->dataWindow();
            const auto solidDisplay = descriptor.value()->displayWindow();
            const auto solidAspect = descriptor.value()->pixelAspect();
            auto& entry = bounds[index];
            entry.local = detail::boundsForWindow(solidWindow, hScale, vScale);
            entry.local.right = width->value;
            entry.local.bottom = height->value;
            entry.output = entry.local;
            outputWindowOf[index] = solidWindow;

            detail::OperationKey key;
            key.add(std::string{"gpu-solid-v1"});
            key.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
            key.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
            key.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
            key.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
            addWindow(key, solidWindow);
            addWindow(key, solidDisplay);
            addPixelAspect(key, solidAspect);
            key.add(std::string{detail::kGpuSolidSpirvSha256});
            keyOf[index] = keyDigest(key);

            if (const auto error = charge(solidWindow.extent().width(),
                                          solidWindow.extent().height(), sizeof(render::Rgba32f))) {
                return failed(error->code, error->message);
            }
            GpuSceneSolidCommand command{.sourceOperation = operationIndex,
                                         .pixel = *pixel.value(),
                                         .dataWindow = solidWindow,
                                         .displayWindow = solidDisplay,
                                         .pixelAspect = solidAspect,
                                         .semanticKey = keyOf[index]};
            commandForOperation[index] = emit(std::move(command));
            continue;
        }

        if (const auto* image = std::get_if<CompiledImageSource>(&operation)) {
            const auto remainingBudget = allowance > chargedBytes ? allowance - chargedBytes : 0;
            detail::GpuSceneUploadLeafResult leaf;
            const auto error = detail::buildImageUploadLeaf(
                *image, request, *plan, resolved, mediaContext_, remainingBudget, hScale, vScale,
                charge, cancellation, mediaStatistics, leaf);
            if (error) {
                return failed(error->code, error->message, mediaStatistics);
            }
            bounds[index] = leaf.bounds;
            outputWindowOf[index] = leaf.outputWindow;
            keyOf[index] = leaf.semanticKey;
            GpuSceneUploadCommand command{.sourceOperation = operationIndex,
                                          .image = std::move(leaf.image),
                                          .descriptor = *leaf.descriptor,
                                          .semanticKey = leaf.semanticKey};
            commandForOperation[index] = emit(std::move(command));
            continue;
        }

        if (const auto* video = std::get_if<CompiledVideoSource>(&operation)) {
            const auto remainingBudget = allowance > chargedBytes ? allowance - chargedBytes : 0;
            detail::GpuSceneUploadLeafResult leaf;
            const auto error = detail::buildVideoUploadLeaf(
                *video, request, *plan, resolved, mediaContext_, remainingBudget, hScale, vScale,
                charge, cancellation, mediaStatistics, leaf);
            if (error) {
                return failed(error->code, error->message, mediaStatistics);
            }
            bounds[index] = leaf.bounds;
            outputWindowOf[index] = leaf.outputWindow;
            keyOf[index] = leaf.semanticKey;
            GpuSceneUploadCommand command{.sourceOperation = operationIndex,
                                          .image = std::move(leaf.image),
                                          .descriptor = *leaf.descriptor,
                                          .semanticKey = leaf.semanticKey};
            commandForOperation[index] = emit(std::move(command));
            continue;
        }

        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            // The CPU Layer Output stage publishes no image and no bounds outside its active range.
            if (request.time < layer->inPoint ||
                (layer->outPoint.has_value() && request.time >= *layer->outPoint)) {
                continue;
            }
            const auto position = detail::resolveParameter(layer->position, *plan, resolved);
            const auto anchor = detail::resolveParameter(layer->anchor, *plan, resolved);
            const auto scale = detail::resolveParameter(layer->scale, *plan, resolved);
            const auto rotation = detail::resolveParameter(layer->rotation, *plan, resolved);
            const auto opacity = detail::resolveParameter(layer->opacity, *plan, resolved);
            const auto blend = detail::resolveParameter(*layer, resolved);
            if (!position || !anchor || !scale || !rotation || !opacity || !blend) {
                return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Layer parameters are not evaluable");
            }
            if (*blend != core::BlendMode::Normal) {
                return failed(PreparedGpuSceneDiagnosticCode::UnsupportedBlend,
                              "Only Normal blending is prepared");
            }
            // Only a direct solid or media source is in this subset. A layer fed by another layer
            // (or a merge) would build a nested vector chain, which is deliberately NOT
            // approximated here. A solid takes the vector-coverage path; media always takes the
            // raster translation path, exactly as the CPU evaluator does.
            const auto& inputOperation = plan->operations()[layer->input.value()];
            const auto* solidInput = std::get_if<CompiledSolid>(&inputOperation);
            const bool mediaInput = std::holds_alternative<CompiledImageSource>(inputOperation) ||
                                    std::holds_alternative<CompiledVideoSource>(inputOperation);
            if (solidInput == nullptr && !mediaInput) {
                return failed(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                              "A layer fed by a non-source input is not prepared");
            }
            const auto inputIndex = commandForOperation[layer->input.value()];
            if (inputIndex == kInvalidGpuSceneCommand || !outputWindowOf[layer->input.value()]) {
                continue;
            }
            const auto sourceWindow = *outputWindowOf[layer->input.value()];
            render::LayerTransform::Authored authored{
                .anchorX = anchor->value.x,
                .anchorY = anchor->value.y,
                .scaleX = scale->value.x,
                .scaleY = scale->value.y,
                .rotationDegrees = rotation->value,
                .opacity = opacity->value,
            };
            if (authored.scaleX == 0.0 || authored.scaleY == 0.0) {
                continue;
            }
            auto& geometry = bounds[index];
            geometry.layerId = layer->layerId;
            geometry.local = bounds[layer->input.value()].output;
            if (geometry.local.empty()) {
                continue;
            }
            const auto centre = geometry.local.centre();
            const auto bufferCentre =
                detail::boundsForWindow(sourceWindow, hScale, vScale).centre();
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
            if (!transform.value()->isTranslationOnly()) {
                return failed(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                              "Only translation-only layers are prepared");
            }
            const auto workingWindow =
                render::ImageWindow::create(-16777216, -16777216, 33554432, 33554432);
            const auto layerWindow = transform.value()->supportBounds(*workingWindow.value());
            if (!layerWindow) {
                continue;
            }
            const auto& transformValue = *transform.value();
            const auto forward = [&](const double x, const double y) {
                return transformValue.forwardMap(x, y);
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

            // The CPU evaluator keys the vector chain by the authored matrix built from the input's
            // CONTENT-BOUNDS centre (not the pixel-area centre), and uses the raster coverage path
            // whenever its device translation is not on the integer grid.
            const auto matrixPivot = geometry.local.centre();
            const auto matrix = detail::LayerMatrix::authored(
                position->value, {matrixPivot.x + anchor->value.x, matrixPivot.y + anchor->value.y},
                scale->value, rotation->value);
            const bool nativeGrid = matrix.a == 1 && matrix.b == 0 && matrix.c == 0 &&
                                    matrix.d == 1 &&
                                    matrix.x * hScale == std::floor(matrix.x * hScale) &&
                                    matrix.y * vScale == std::floor(matrix.y * vScale);

            // Media has no vector chain, so the CPU evaluator always resamples it with the raster
            // (bilinear) path regardless of whether the device translation is on the integer grid;
            // the translation command reproduces that path exactly. Solids keep the coverage path
            // whenever the grid is fractional.
            if (nativeGrid || mediaInput) {
                const auto device = transformValue.translationOnlyDeviceTranslation();
                if (!device.has_value()) {
                    return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                  "Translation-only transform lost its device translation");
                }
                const double gpuTranslationX =
                    device->x - (static_cast<double>(layerWindow->originX()) -
                                 static_cast<double>(sourceWindow.originX()));
                const double gpuTranslationY =
                    device->y - (static_cast<double>(layerWindow->originY()) -
                                 static_cast<double>(sourceWindow.originY()));
                detail::OperationKey key;
                key.add(std::string{"gpu-translation-opacity-v1"});
                key.add(keyOf[layer->input.value()]);
                key.add(gpuTranslationX);
                key.add(gpuTranslationY);
                key.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity->value)));
                addWindow(key, sourceWindow);
                addWindow(key, *layerWindow);
                addPixelAspect(key, fullPixelAspect);
                key.add(std::string{detail::kGpuTranslationOpacitySpirvSha256});
                keyOf[index] = keyDigest(key);

                if (const auto error =
                        charge(layerWindow->extent().width(), layerWindow->extent().height(),
                               sizeof(render::Rgba32f))) {
                    return failed(error->code, error->message);
                }
                GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                                   .input = inputIndex,
                                                   .sourceWindow = sourceWindow,
                                                   .outputWindow = *layerWindow,
                                                   .translationX = gpuTranslationX,
                                                   .translationY = gpuTranslationY,
                                                   .opacity = static_cast<float>(opacity->value),
                                                   .semanticKey = keyOf[index]};
                commandForOperation[index] = emit(std::move(command));
                continue;
            }

            // Fractional device grid: the CPU composes this layer through its vector-coverage path.
            GpuSceneCoverageSolidCommand coverageCommand{
                .index = kInvalidGpuSceneCommand,
                .sourceOperation = operationIndex,
                .pixel = render::Rgba32f::transparent(),
                .opacity = 1.0F,
                .coverage = nullptr,
                .outputWindow = *layerWindow,
                .displayWindow = fullDisplayWindow,
                .pixelAspect = fullPixelAspect,
                .geometryKey = {},
                .semanticKey = {},
            };
            const auto chargeResidentBytes = [&](const std::uint64_t width,
                                                 const std::uint64_t height,
                                                 const std::uint64_t bytesPerPixel)
                -> std::optional<detail::GpuSceneLeafFailure> {
                return charge(width, height, bytesPerPixel);
            };
            if (const auto error = detail::buildCoverageSolidLeaf(
                    *solidInput, *plan, resolved, matrix, *layerWindow, fullDisplayWindow,
                    fullPixelAspect, hScale, vScale, opacity->value, coverageCache_,
                    chargeResidentBytes, cancellation, coverageCommand)) {
                return failed(error->code, error->message);
            }
            if (const auto error =
                    chargeCoverage(coverageCommand.coverage, layerWindow->extent().width(),
                                   layerWindow->extent().height())) {
                return failed(error->code, error->message);
            }
            keyOf[index] = coverageCommand.semanticKey;
            coverageCommand.sourceOperation = operationIndex;
            commandForOperation[index] = emit(std::move(coverageCommand));
            continue;
        }

        if (const auto* stack = std::get_if<CompiledMerge>(&operation)) {
            ContentBounds storage;
            for (const auto& entry : stack->entries) {
                bounds[index].local =
                    detail::unionBounds(bounds[index].local, bounds[entry.input.value()].output);
                if (outputWindowOf[entry.input.value()]) {
                    storage = detail::unionBounds(
                        storage,
                        detail::boundsForWindow(*outputWindowOf[entry.input.value()], 1.0, 1.0));
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
                    return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                  "Merge bounds are invalid");
                }
                const auto local = render::Rgba32fImageDescriptor::create(
                    *window.value(), descriptor.displayWindow(), descriptor.pixelAspect());
                if (!local) {
                    return failed(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                  "Merge descriptor is invalid");
                }
                descriptor = *local.value();
            }
            outputWindowOf[index] = descriptor.dataWindow();

            std::vector<GpuSceneCommandIndex> foregrounds;
            detail::OperationKey key;
            key.add(std::string{"gpu-source-over-chain-v1"});
            for (auto entry = stack->entries.rbegin(); entry != stack->entries.rend(); ++entry) {
                if (const auto* layerOutput =
                        std::get_if<CompiledLayerOutput>(&plan->operations()[entry->input.value()]);
                    layerOutput != nullptr && entry->layerId.isValid()) {
                    const auto blend = detail::resolveParameter(*layerOutput, resolved);
                    if (!blend || *blend != core::BlendMode::Normal) {
                        return failed(PreparedGpuSceneDiagnosticCode::UnsupportedBlend,
                                      "Only Normal merge entries are prepared");
                    }
                }
                const auto inner = commandForOperation[entry->input.value()];
                if (inner == kInvalidGpuSceneCommand) {
                    continue;
                }
                foregrounds.push_back(inner);
                key.add(keyOf[entry->input.value()]);
            }
            addWindow(key, descriptor.dataWindow());
            addWindow(key, descriptor.displayWindow());
            addPixelAspect(key, descriptor.pixelAspect());
            key.add(std::string{detail::kGpuSourceOverSpirvSha256});
            keyOf[index] = keyDigest(key);

            if (const auto error =
                    charge(descriptor.dataWindow().extent().width(),
                           descriptor.dataWindow().extent().height(), sizeof(render::Rgba32f))) {
                return failed(error->code, error->message);
            }
            GpuSceneMergeCommand command{.sourceOperation = operationIndex,
                                         .foregrounds = std::move(foregrounds),
                                         .outputWindow = descriptor.dataWindow(),
                                         .displayWindow = descriptor.displayWindow(),
                                         .pixelAspect = descriptor.pixelAspect(),
                                         .semanticKey = keyOf[index]};
            commandForOperation[index] = emit(std::move(command));
            continue;
        }

        if (const auto* output = std::get_if<CompiledCompositionOutput>(&operation)) {
            bounds[index] = bounds[output->input.value()];
            const auto inner = commandForOperation[output->input.value()];
            detail::OperationKey key;
            key.add(std::string{"gpu-composition-output-v1"});
            key.add(inner == kInvalidGpuSceneCommand ? std::string{"none"}
                                                     : keyOf[output->input.value()]);
            addWindow(key, resolved.imageDescriptor.dataWindow());
            addPixelAspect(key, resolved.imageDescriptor.pixelAspect());
            keyOf[index] = keyDigest(key);

            if (const auto error = charge(resolved.imageDescriptor.dataWindow().extent().width(),
                                          resolved.imageDescriptor.dataWindow().extent().height(),
                                          sizeof(render::Rgba32f))) {
                return failed(error->code, error->message);
            }
            GpuSceneCompositionOutputCommand command{
                .sourceOperation = operationIndex,
                .input = inner,
                .dataWindow = resolved.imageDescriptor.dataWindow(),
                .displayWindow = resolved.imageDescriptor.displayWindow(),
                .pixelAspect = resolved.imageDescriptor.pixelAspect(),
                .semanticKey = keyOf[index]};
            commandForOperation[index] = emit(std::move(command));
            continue;
        }

        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Reachable operation was not handled");
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
        std::move(bounds), resolved.imageDescriptor, std::move(mediaStatistics)));
    return {std::move(scene), PreparedGpuSceneDiagnostic{}};
}

} // namespace bloom::runtime
