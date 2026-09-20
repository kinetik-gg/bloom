// Concrete CompiledCompositionPlan builders for the GPU scene preparation tests (solid, two-layer,
// text and shape graphs). Included by gpu_scene_preparation_test_support.hpp inside its anonymous
// namespace, so every fixture builder keeps its exact identity and behaviour.

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
