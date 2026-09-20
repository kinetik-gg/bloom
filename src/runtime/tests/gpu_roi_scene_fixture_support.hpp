#pragma once

// Shared ROI scene fixtures for the CPU and native ROI parity tests. Included AFTER
// gpu_scene_preparation_test_support.hpp, whose anonymous-namespace vocabulary (format,
// LayerValues, layerOutput, publish, requestFor, ...) these helpers build on. Every helper is in an
// anonymous namespace so including this header in more than one test translation unit cannot create
// an ODR clash.

#include "gpu_scene_preparation_test_support.hpp"

namespace {

// A region-of-interest window for a request. A zero-extent window cannot be represented by
// ImageWindow, and is not a valid ROI anyway (the shared preflight refuses it).
[[nodiscard]] bloom::render::ImageWindow roi(const std::int64_t x, const std::int64_t y,
                                             const std::uint64_t w, const std::uint64_t h) {
    const auto window = bloom::render::ImageWindow::create(x, y, w, h);
    if (!window) {
        throw std::logic_error("test ROI window is invalid");
    }
    return *window.value();
}

// A child composition: two independently-coloured solid branches merged bottom-to-top.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
nestedChildPlan(const std::uint64_t idBase, const std::uint64_t compositionRaw,
                const Color4d colorB) {
    const auto layerIdsA = LayerIds{bloom::document::ParameterId::fromRaw(idBase + 0),
                                    bloom::document::ParameterId::fromRaw(idBase + 1),
                                    bloom::document::ParameterId::fromRaw(idBase + 2),
                                    bloom::document::ParameterId::fromRaw(idBase + 3),
                                    bloom::document::ParameterId::fromRaw(idBase + 4),
                                    bloom::document::ParameterId::fromRaw(idBase + 5)};
    const auto layerIdsB = LayerIds{bloom::document::ParameterId::fromRaw(idBase + 6),
                                    bloom::document::ParameterId::fromRaw(idBase + 7),
                                    bloom::document::ParameterId::fromRaw(idBase + 8),
                                    bloom::document::ParameterId::fromRaw(idBase + 9),
                                    bloom::document::ParameterId::fromRaw(idBase + 10),
                                    bloom::document::ParameterId::fromRaw(idBase + 11)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{
        bloom::document::NodeId::fromRaw(idBase + 10),
        {bloom::document::ParameterId::fromRaw(idBase + 12), Color4d{0.5, 0.25, 0.125, 1.0}},
        {bloom::document::ParameterId::fromRaw(idBase + 13), 8.0},
        {bloom::document::ParameterId::fromRaw(idBase + 14), 6.0}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 20),
                                        bloom::document::LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(0), layerIdsA,
                                        LayerValues{.position = {4.0, 3.0}}));
    operations.emplace_back(
        CompiledSolid{bloom::document::NodeId::fromRaw(idBase + 40),
                      {bloom::document::ParameterId::fromRaw(idBase + 41), colorB},
                      {bloom::document::ParameterId::fromRaw(idBase + 42), 6.0},
                      {bloom::document::ParameterId::fromRaw(idBase + 43), 5.0}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 44),
                                        bloom::document::LayerId::fromRaw(idBase + 45),
                                        OperationIndex::fromRaw(2), layerIdsB,
                                        LayerValues{.position = {6.0, 5.0}, .opacity = 0.75}));
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 60),
                      std::vector<CompiledMergeInput>{
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 61),
                                             bloom::document::LayerId::fromRaw(idBase + 30),
                                             OperationIndex::fromRaw(1)},
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 62),
                                             bloom::document::LayerId::fromRaw(idBase + 45),
                                             OperationIndex::fromRaw(3)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 70),
                                                      OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{
        bloom::document::Revision::fromRaw(7),
        kProjectId,
        bloom::document::CompositionId::fromRaw(compositionRaw),
        format(12, 10),
        std::move(operations),
        OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    return publish(std::move(definition));
}

// A parent that consumes the child through a CompiledCompositionSource, merges it, and outputs it.
// The caller must pass a composition id distinct from the child's; nested-resolution readiness
// enforces that.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
nestedParentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child,
                 const std::uint64_t compositionRaw) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledCompositionSource{
        bloom::document::NodeId::fromRaw(9000), 0,
        bloom::runtime::CompiledCompositionTimeMapping{
            {bloom::document::ParameterId::fromRaw(9001), 0.0},
            {bloom::document::ParameterId::fromRaw(9002), 1.0},
            0}});
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(9003),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          bloom::document::LayerSlotId::fromRaw(9004), bloom::document::LayerId{},
                          OperationIndex::fromRaw(0)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(9005),
                                                      OperationIndex::fromRaw(1)});
    CompiledCompositionPlanDefinition definition{
        bloom::document::Revision::fromRaw(7),
        kProjectId,
        bloom::document::CompositionId::fromRaw(compositionRaw),
        format(12, 10),
        std::move(operations),
        OperationIndex::fromRaw(2)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return publish(std::move(definition));
}

} // namespace
