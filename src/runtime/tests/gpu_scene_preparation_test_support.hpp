#pragma once

// Shared fixtures and CPU reference replay for the GPU scene preparation tests.

// Focused tests for CPU-side GPU scene preparation (solid/translation-only/Normal merge/output).
//
// Every fixture is a real CompiledCompositionPlan built with canonical, globally unique parameter
// identities. The prepared commands are compared against a genuine CpuCompositionEvaluator frame:
// identity, bounds, output descriptor, and -- by replaying the commands with the EXISTING CPU
// primitives in this test only -- every output pixel bit for bit.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_scene_coverage_geometry_test_support.hpp"
#include "layer_parent_transform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::PreparedGpuSceneDiagnosticCode;
using bloom::runtime::ProcessFrame;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

[[maybe_unused, nodiscard]] std::string firstMismatch(const Rgba32fImage& replayed,
                                                      const Rgba32fImage& cpu) {
    if (replayed.pixels().size() != cpu.pixels().size()) {
        return "size " + std::to_string(replayed.pixels().size()) + " vs " +
               std::to_string(cpu.pixels().size());
    }
    for (std::size_t i = 0; i < replayed.pixels().size(); ++i) {
        if (!(replayed.pixels()[i] == cpu.pixels()[i])) {
            return "index " + std::to_string(i) + " (" +
                   std::to_string(replayed.pixels()[i].red()) + "," +
                   std::to_string(replayed.pixels()[i].green()) + ") vs (" +
                   std::to_string(cpu.pixels()[i].red()) + "," +
                   std::to_string(cpu.pixels()[i].green()) + ")";
        }
    }
    return "equal";
}

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNodeA = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNodeA = bloom::document::NodeId::fromRaw(11);

struct LayerIds final {
    bloom::document::ParameterId position;
    bloom::document::ParameterId anchor;
    bloom::document::ParameterId scale;
    bloom::document::ParameterId rotation;
    bloom::document::ParameterId opacity;
    bloom::document::ParameterId blendMode;
};

struct LayerValues final {
    bloom::document::Vec2d position{2.0, 1.0};
    bloom::document::Vec2d anchor{0.0, 0.0};
    bloom::document::Vec2d scale{1.0, 1.0};
    double rotation = 0.0;
    double opacity = 1.0;
    bloom::core::BlendMode blendMode = bloom::core::kDefaultBlendMode;
};

[[nodiscard]] CompositionFormat
format(const std::uint32_t width, const std::uint32_t height,
       const bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square()) {
    const auto value = CompositionFormat::create(width, height, pixelAspect);
    if (!value.has_value()) {
        throw std::logic_error("test composition format must be valid");
    }
    return *value;
}

// Checked constructors for the canonical test fixture values. These keep every call site free of an
// unchecked optional dereference while preserving the fail-fast behaviour on an invalid fixture.
[[maybe_unused, nodiscard]] bloom::core::PixelAspectRatio
pixelAspect(const std::uint64_t numerator, const std::uint64_t denominator) {
    const auto value = bloom::core::PixelAspectRatio::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test pixel aspect must be valid");
    }
    return *value;
}

[[maybe_unused, nodiscard]] RationalTime rationalTime(const std::int64_t numerator,
                                                      const std::int64_t denominator) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test rational time must be valid");
    }
    return *value;
}

[[nodiscard]] CompiledLayerOutput layerOutput(const bloom::document::NodeId nodeId,
                                              const bloom::document::LayerId layerId,
                                              const OperationIndex input, const LayerIds ids,
                                              const LayerValues values) {
    return CompiledLayerOutput{nodeId,
                               layerId,
                               input,
                               CompiledVec2Parameter{ids.position, values.position},
                               CompiledVec2Parameter{ids.anchor, values.anchor},
                               CompiledVec2Parameter{ids.scale, values.scale},
                               CompiledScalarParameter{ids.rotation, values.rotation},
                               CompiledScalarParameter{ids.opacity, values.opacity},
                               ids.blendMode,
                               values.blendMode};
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
publish(CompiledCompositionPlanDefinition definition) {
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] EvaluationRequest requestFor(const CompiledCompositionPlan& plan,
                                           const RationalTime time = RationalTime::fromInteger(0),
                                           const std::size_t budget = 1U << 28U) {
    return EvaluationRequest{
        .time = time,
        .output = plan.output(),
        .resolution = bloom::runtime::CompositionFormatResolution{},
        .quality = bloom::runtime::EvaluationQuality::Reference,
        .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = budget,
    };
}

// A solid -> translation-only layer -> Normal merge -> output plan with two layers.
[[maybe_unused, nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
twoLayerPlan(const CompositionFormat compositionFormat, const LayerValues a, const LayerValues b,
             const double solidWidth, const double solidHeight, const std::uint64_t idBase) {
    const LayerIds idsA{bloom::document::ParameterId::fromRaw(idBase + 0),
                        bloom::document::ParameterId::fromRaw(idBase + 1),
                        bloom::document::ParameterId::fromRaw(idBase + 2),
                        bloom::document::ParameterId::fromRaw(idBase + 3),
                        bloom::document::ParameterId::fromRaw(idBase + 4),
                        bloom::document::ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{bloom::document::ParameterId::fromRaw(idBase + 6),
                        bloom::document::ParameterId::fromRaw(idBase + 7),
                        bloom::document::ParameterId::fromRaw(idBase + 8),
                        bloom::document::ParameterId::fromRaw(idBase + 9),
                        bloom::document::ParameterId::fromRaw(idBase + 10),
                        bloom::document::ParameterId::fromRaw(idBase + 11)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{
        kSolidNodeA,
        {bloom::document::ParameterId::fromRaw(idBase + 20), Color4d{0.5, 0.25, 0.125, 1.0}},
        {bloom::document::ParameterId::fromRaw(idBase + 21), solidWidth},
        {bloom::document::ParameterId::fromRaw(idBase + 22), solidHeight}});
    operations.emplace_back(layerOutput(kLayerNodeA, bloom::document::LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(0), idsA, a));
    operations.emplace_back(CompiledSolid{
        bloom::document::NodeId::fromRaw(idBase + 40),
        {bloom::document::ParameterId::fromRaw(idBase + 41), Color4d{0.125, 0.375, 0.75, 0.5}},
        {bloom::document::ParameterId::fromRaw(idBase + 42), solidWidth},
        {bloom::document::ParameterId::fromRaw(idBase + 43), solidHeight}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 44),
                                        bloom::document::LayerId::fromRaw(idBase + 45),
                                        OperationIndex::fromRaw(2), idsB, b));
    const CompiledMergeInput first{bloom::document::LayerSlotId::fromRaw(idBase + 50),
                                   bloom::document::LayerId::fromRaw(idBase + 30),
                                   OperationIndex::fromRaw(1)};
    const CompiledMergeInput second{bloom::document::LayerSlotId::fromRaw(idBase + 51),
                                    bloom::document::LayerId::fromRaw(idBase + 45),
                                    OperationIndex::fromRaw(3)};
    operations.emplace_back(CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 52),
                                          std::vector<CompiledMergeInput>{first, second}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 53),
                                                      OperationIndex::fromRaw(4)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(5)});
}

// A text -> translation-only layer -> Normal merge -> output plan. Text is a single premultiplied
// colour through an 8-bit glyph coverage, so it prepares through CoveredSolidV1 coverage.
[[maybe_unused, nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
textPlan(const CompositionFormat compositionFormat, const LayerValues values,
         const std::uint64_t idBase) {
    const LayerIds ids{bloom::document::ParameterId::fromRaw(idBase + 0),
                       bloom::document::ParameterId::fromRaw(idBase + 1),
                       bloom::document::ParameterId::fromRaw(idBase + 2),
                       bloom::document::ParameterId::fromRaw(idBase + 3),
                       bloom::document::ParameterId::fromRaw(idBase + 4),
                       bloom::document::ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledText{
        bloom::document::NodeId::fromRaw(idBase + 60),
        bloom::document::ParameterId::fromRaw(idBase + 61),
        "BLOOM",
        {bloom::document::ParameterId::fromRaw(idBase + 62), 10.0},
        {bloom::document::ParameterId::fromRaw(idBase + 63), Color4d{0.8, 0.4, 0.2, 1.0}},
        bloom::runtime::CompiledTextLayout{
            bloom::document::ParameterId::fromRaw(idBase + 64),
            0,
            {bloom::document::ParameterId::fromRaw(idBase + 65), 1.0},
            {bloom::document::ParameterId::fromRaw(idBase + 66), 0.0}}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 67),
                                        bloom::document::LayerId::fromRaw(idBase + 68),
                                        OperationIndex::fromRaw(0), ids, values));
    operations.emplace_back(CompiledMerge{
        bloom::document::NodeId::fromRaw(idBase + 70),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            bloom::document::LayerSlotId::fromRaw(idBase + 71),
            bloom::document::LayerId::fromRaw(idBase + 68), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 69),
                                                      OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

struct ShapeValues final {
    bloom::document::ShapeKind kind = bloom::document::ShapeKind::Rectangle;
    bloom::document::Vec2d size{9.0, 7.0};
    double cornerRadius = 0.0;
    std::int64_t points = 5;
    double innerRatio = 0.5;
    Color4d fillColor{0.6, 0.3, 0.15, 1.0};
    bool fillEnabled = true;
    Color4d strokeColor{0.1, 0.2, 0.9, 0.9};
    bool strokeEnabled = false;
    double strokeWidth = 0.0;
    bloom::document::ShapeStrokeAlign strokeAlign = bloom::document::ShapeStrokeAlign::Center;
    bloom::document::ShapeStrokeJoin strokeJoin = bloom::document::ShapeStrokeJoin::Miter;
    bloom::document::ShapeStrokeCap strokeCap = bloom::document::ShapeStrokeCap::Butt;
    bloom::document::ShapeFillRule fillRule = bloom::document::ShapeFillRule::NonZero;
};

// A shape -> translation-only layer -> Normal merge -> output plan.
[[maybe_unused, nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
shapePlan(const CompositionFormat compositionFormat, const LayerValues values,
          const ShapeValues shapeValues, const std::uint64_t idBase) {
    const LayerIds ids{bloom::document::ParameterId::fromRaw(idBase + 0),
                       bloom::document::ParameterId::fromRaw(idBase + 1),
                       bloom::document::ParameterId::fromRaw(idBase + 2),
                       bloom::document::ParameterId::fromRaw(idBase + 3),
                       bloom::document::ParameterId::fromRaw(idBase + 4),
                       bloom::document::ParameterId::fromRaw(idBase + 5)};
    bloom::runtime::CompiledShape shape;
    shape.sourceNodeId = bloom::document::NodeId::fromRaw(idBase + 60);
    shape.kind = shapeValues.kind;
    shape.size = {bloom::document::ParameterId::fromRaw(idBase + 62), shapeValues.size};
    shape.cornerRadius = shapeValues.cornerRadius;
    shape.points = shapeValues.points;
    shape.innerRatio = shapeValues.innerRatio;
    shape.lineStart = {0.0, 0.0};
    shape.lineEnd = {shapeValues.size.x, shapeValues.size.y};
    shape.fillEnabled = shapeValues.fillEnabled;
    shape.fillColor = {bloom::document::ParameterId::fromRaw(idBase + 63), shapeValues.fillColor};
    shape.strokeEnabled = shapeValues.strokeEnabled;
    shape.strokeColor = {bloom::document::ParameterId::fromRaw(idBase + 64),
                         shapeValues.strokeColor};
    shape.strokeWidth = {bloom::document::ParameterId::fromRaw(idBase + 65),
                         shapeValues.strokeWidth};
    shape.strokeAlign = shapeValues.strokeAlign;
    shape.strokeJoin = shapeValues.strokeJoin;
    shape.strokeCap = shapeValues.strokeCap;
    shape.fillRule = shapeValues.fillRule;
    if (shapeValues.kind == bloom::document::ShapeKind::Path) {
        shape.path.closed = true;
        shape.path.anchors = {
            bloom::document::PathAnchor{bloom::document::Vec2d{0.0, 0.0}, {}, {}},
            bloom::document::PathAnchor{bloom::document::Vec2d{shapeValues.size.x, 0.0}, {}, {}},
            bloom::document::PathAnchor{
                bloom::document::Vec2d{shapeValues.size.x * 0.5, shapeValues.size.y}, {}, {}}};
    }
    std::vector<CompiledOperation> operations;
    operations.emplace_back(std::move(shape));
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 67),
                                        bloom::document::LayerId::fromRaw(idBase + 68),
                                        OperationIndex::fromRaw(0), ids, values));
    operations.emplace_back(CompiledMerge{
        bloom::document::NodeId::fromRaw(idBase + 70),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            bloom::document::LayerSlotId::fromRaw(idBase + 71),
            bloom::document::LayerId::fromRaw(idBase + 68), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 69),
                                                      OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

// --- Replay with the existing CPU primitives (test-only)
// ------------------------------------------
[[nodiscard]] std::shared_ptr<const Rgba32fImage> freeze(Rgba32fImageBuilder& builder) {
    auto frozen = std::move(builder).freeze();
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

[[maybe_unused, nodiscard]] bool
replayScene(const PreparedGpuScene& scene, std::vector<std::shared_ptr<const Rgba32fImage>>& images,
            const double hScale = 1.0, const double vScale = 1.0) {
    constexpr std::size_t kBudget = 1U << 28U;
    images.assign(scene.commands().size(), nullptr);
    for (const auto& command : scene.commands()) {
        if (const auto* solid = std::get_if<bloom::runtime::GpuSceneSolidCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                solid->dataWindow, solid->displayWindow, solid->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
            if (!builder) {
                return false;
            }
            for (std::int64_t y = solid->dataWindow.originY();
                 y < solid->dataWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                bloom::render::fillSolidRow(*row.value(), solid->pixel);
            }
            images[solid->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* translation =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command)) {
            const auto* input = images[translation->input].get();
            if (input == nullptr) {
                return false;
            }
            const auto sourceView = input->view();
            if (!sourceView) {
                return false;
            }
            const auto params = bloom::render::TranslationOpacity::create(
                translation->translationX, translation->translationY,
                static_cast<double>(translation->opacity));
            if (!params) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                translation->outputWindow, input->descriptor()->displayWindow(),
                input->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
            if (!builder) {
                return false;
            }
            for (std::int64_t y = translation->outputWindow.originY();
                 y < translation->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                if (const auto status = bloom::render::translateOpacityBilinearRow(
                        *sourceView.value(), translation->outputWindow, y, *params.value(),
                        *row.value())) {
                    (void)status;
                    return false;
                }
            }
            images[translation->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* coverage =
                std::get_if<bloom::runtime::GpuSceneCoverageSolidCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                coverage->outputWindow, coverage->displayWindow, coverage->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto width = coverage->outputWindow.extent().width();
            const std::vector<std::uint8_t> mask =
                bloom::gpu_scene_coverage_test::coverageMaskBytes(*coverage);
            if (mask.empty()) {
                return false;
            }
            for (std::int64_t y = coverage->outputWindow.originY();
                 y < coverage->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                const auto offset =
                    static_cast<std::size_t>(y - coverage->outputWindow.originY()) * width;
                const auto coverageRow = std::span<const std::uint8_t>(mask.data() + offset, width);
                if (const auto status = bloom::render::coverageSolidRow(
                        coverageRow, coverage->pixel, *row.value())) {
                    (void)status;
                    return false;
                }
                for (auto& value : *row.value()) {
                    const auto faded = Rgba32f::fromPremultiplied(
                        value.red() * coverage->opacity, value.green() * coverage->opacity,
                        value.blue() * coverage->opacity, value.alpha() * coverage->opacity);
                    if (!faded) {
                        return false;
                    }
                    value = *faded.value();
                }
            }
            images[coverage->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* upload = std::get_if<bloom::runtime::GpuSceneUploadCommand>(&command)) {
            // The upload command already carries the frozen converted source, so replay is an
            // alias: this is exactly the immutability the native upload would rely on.
            images[upload->index] = upload->image;
            continue;
        }
        if (const auto* merge = std::get_if<bloom::runtime::GpuSceneMergeCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                merge->outputWindow, merge->displayWindow, merge->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto destinationWindow = merge->outputWindow;
            for (const auto foreground : merge->foregrounds) {
                const auto* source = images[foreground].get();
                if (source == nullptr) {
                    continue;
                }
                const auto sourceView = source->view();
                if (!sourceView) {
                    return false;
                }
                const auto sourceWindow = source->descriptor()->dataWindow();
                const auto firstColumn =
                    std::max(sourceWindow.originX(), destinationWindow.originX());
                const auto lastColumn =
                    std::min(sourceWindow.maxXExclusive(), destinationWindow.maxXExclusive());
                if (lastColumn <= firstColumn) {
                    continue;
                }
                const auto sourceOffset = firstColumn - sourceWindow.originX();
                const auto columnOffset = firstColumn - destinationWindow.originX();
                const auto columnCount = static_cast<std::size_t>(lastColumn - firstColumn);
                for (std::int64_t y = std::max(sourceWindow.originY(), destinationWindow.originY());
                     y < std::min(sourceWindow.maxYExclusive(), destinationWindow.maxYExclusive());
                     ++y) {
                    auto sourceRow = sourceView.value()->row(y);
                    auto destinationRow = builder.value()->row(y);
                    if (!sourceRow || !destinationRow) {
                        return false;
                    }
                    if (const auto status = bloom::render::sourceOverLinearRec709SceneRow(
                            sourceRow.value()->subspan(static_cast<std::size_t>(sourceOffset),
                                                       columnCount),
                            destinationRow.value()->subspan(static_cast<std::size_t>(columnOffset),
                                                            columnCount))) {
                        (void)status;
                        return false;
                    }
                }
            }
            images[merge->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* affine = std::get_if<bloom::runtime::GpuSceneAffineCommand>(&command)) {
            const auto* input = images[affine->input].get();
            if (input == nullptr) {
                return false;
            }
            const auto view = input->view();
            if (!view) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                affine->outputWindow, input->descriptor()->displayWindow(),
                input->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            // Recover the AUTHOR-space LayerMatrix from the device-space GpuAffine matrix and
            // replay with the exact CPU oracle the evaluator's parented raster arm uses.
            const auto& g = affine->matrix;
            const double b = g.b * (vScale / hScale);
            const double c = g.c * (hScale / vScale);
            const double ox = static_cast<double>(affine->sourceWindow.originX()) + 0.5;
            const double oy = static_cast<double>(affine->sourceWindow.originY()) + 0.5;
            const double mx = (g.tx - g.a * ox - g.b * oy + 0.5) / hScale;
            const double my = (g.ty - g.c * ox - g.d * oy + 0.5) / vScale;
            bloom::runtime::detail::LayerMatrix matrix{g.a, b, c, g.d, mx, my};
            bloom::runtime::detail::ParentedLayerTransform parented(
                matrix, affine->sourceWindow, hScale, vScale, affine->opacity);
            for (std::int64_t y = affine->outputWindow.originY();
                 y < affine->outputWindow.maxYExclusive(); ++y) {
                auto row = builder.value()->row(y);
                if (!row) {
                    return false;
                }
                if (const auto status =
                        parented.row(*view.value(), affine->outputWindow, y, *row.value())) {
                    (void)status;
                    return false;
                }
            }
            images[affine->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* blend = std::get_if<bloom::runtime::GpuSceneBlendCommand>(&command)) {
            const auto* source = images[blend->source].get();
            const auto* destination = images[blend->destination].get();
            if (source == nullptr || destination == nullptr) {
                return false;
            }
            const auto sourceView = source->view();
            const auto destinationView = destination->view();
            if (!sourceView || !destinationView) {
                return false;
            }
            const auto descriptor = Rgba32fImageDescriptor::create(
                blend->outputWindow, destination->descriptor()->displayWindow(),
                destination->descriptor()->pixelAspect());
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            const auto outputWindow = blend->outputWindow;
            const auto destinationWindow = destination->descriptor()->dataWindow();
            const auto firstColumn = std::max(destinationWindow.originX(), outputWindow.originX());
            const auto lastColumn =
                std::min(destinationWindow.maxXExclusive(), outputWindow.maxXExclusive());
            const auto firstRow = std::max(destinationWindow.originY(), outputWindow.originY());
            const auto lastRow =
                std::min(destinationWindow.maxYExclusive(), outputWindow.maxYExclusive());
            if (lastColumn > firstColumn && lastRow > firstRow) {
                const auto count = static_cast<std::size_t>(lastColumn - firstColumn);
                const auto destinationOffset = firstColumn - destinationWindow.originX();
                const auto outputOffset = firstColumn - outputWindow.originX();
                for (std::int64_t y = firstRow; y < lastRow; ++y) {
                    auto destinationRow = destinationView.value()->row(y);
                    auto outputRow = builder.value()->row(y);
                    if (!destinationRow || !outputRow) {
                        return false;
                    }
                    std::copy_n(destinationRow.value()->begin() + destinationOffset, count,
                                outputRow.value()->begin() + outputOffset);
                }
            }
            const auto sourceWindow = source->descriptor()->dataWindow();
            const auto blendFirstColumn = std::max(sourceWindow.originX(), outputWindow.originX());
            const auto blendLastColumn =
                std::min(sourceWindow.maxXExclusive(), outputWindow.maxXExclusive());
            const auto blendFirstRow = std::max(sourceWindow.originY(), outputWindow.originY());
            const auto blendLastRow =
                std::min(sourceWindow.maxYExclusive(), outputWindow.maxYExclusive());
            if (blendLastColumn > blendFirstColumn && blendLastRow > blendFirstRow) {
                const auto count = static_cast<std::size_t>(blendLastColumn - blendFirstColumn);
                const auto sourceOffset = blendFirstColumn - sourceWindow.originX();
                const auto outputOffset = blendFirstColumn - outputWindow.originX();
                for (std::int64_t y = blendFirstRow; y < blendLastRow; ++y) {
                    auto sourceRow = sourceView.value()->row(y);
                    auto outputRow = builder.value()->row(y);
                    if (!sourceRow || !outputRow) {
                        return false;
                    }
                    if (const auto status = bloom::render::blendLinearRec709SceneRow(
                            blend->mode,
                            sourceRow.value()->subspan(static_cast<std::size_t>(sourceOffset),
                                                       count),
                            outputRow.value()->subspan(static_cast<std::size_t>(outputOffset),
                                                       count))) {
                        (void)status;
                        return false;
                    }
                }
            }
            images[blend->index] = freeze(*builder.value());
            continue;
        }
        if (const auto* output =
                std::get_if<bloom::runtime::GpuSceneCompositionOutputCommand>(&command)) {
            const auto descriptor = Rgba32fImageDescriptor::create(
                output->dataWindow, output->displayWindow, output->pixelAspect);
            if (!descriptor) {
                return false;
            }
            auto builder =
                Rgba32fImageBuilder::create(*descriptor.value(), kBudget, Rgba32f::transparent());
            if (!builder) {
                return false;
            }
            if (output->input != bloom::runtime::kInvalidGpuSceneCommand) {
                const auto* source = images[output->input].get();
                if (source != nullptr) {
                    const auto sourceWindow = source->descriptor()->dataWindow();
                    const auto firstColumn =
                        std::max(sourceWindow.originX(), output->dataWindow.originX());
                    const auto lastColumn =
                        std::min(sourceWindow.maxXExclusive(), output->dataWindow.maxXExclusive());
                    const auto firstRow =
                        std::max(sourceWindow.originY(), output->dataWindow.originY());
                    const auto lastRow =
                        std::min(sourceWindow.maxYExclusive(), output->dataWindow.maxYExclusive());
                    if (lastColumn > firstColumn && lastRow > firstRow) {
                        const auto count = static_cast<std::size_t>(lastColumn - firstColumn);
                        const auto sourceOffset = firstColumn - sourceWindow.originX();
                        const auto destinationOffset = firstColumn - output->dataWindow.originX();
                        for (std::int64_t y = firstRow; y < lastRow; ++y) {
                            const auto sourceView = source->view();
                            auto sourceRow = sourceView.value()->row(y);
                            auto destinationRow = builder.value()->row(y);
                            std::copy_n(sourceRow.value()->begin() + sourceOffset, count,
                                        destinationRow.value()->begin() + destinationOffset);
                        }
                    }
                }
            }
            images[output->index] = freeze(*builder.value());
            continue;
        }
        return false;
    }
    return true;
}
} // namespace
