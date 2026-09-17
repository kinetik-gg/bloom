std::uint64_t boundsPixelDigest(const runtime::ProcessFrame& frame) {
    std::uint64_t digest = 14695981039346656037ULL;
    for (const auto pixelValue : frame.processImage().pixels()) {
        for (const auto bits : std::bit_cast<std::array<std::uint32_t, 4>>(pixelValue)) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                digest ^= (bits >> shift) & 255U;
                digest *= 1099511628211ULL;
            }
        }
    }
    return digest;
}
void localSolid(runtime::CompiledCompositionPlanDefinition& definition, const std::size_t index,
                const double width, const double height, const std::uint64_t firstParameter) {
    auto& solid = std::get<runtime::CompiledSolid>(definition.operations[index]);
    solid.width =
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(firstParameter), width};
    solid.height = runtime::CompiledScalarParameter{
        document::ParameterId::fromRaw(firstParameter + 1), height};
}
void testContentBounds(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto baseline = oneSolidPlan();
    auto defaults = baseline->copyDefinition();
    localSolid(defaults, 0, 4, 2, 90);
    const auto current = publishPlan(defaults);
    const auto before = evaluator.evaluate(baseline, requestFor(*baseline), {});
    const auto after = evaluator.evaluate(current, requestFor(*current), {});
    expectations.expect(before.frame() && after.frame() &&
                            boundsPixelDigest(*before.frame()) == boundsPixelDigest(*after.frame()),
                        "composition-sized solid with default anchor remains bit-identical");
    if (!after.frame())
        return;
    expectations.expect(after.frame()->evaluatedBounds()[0].local ==
                            runtime::ContentBounds{0, 0, 4, 2},
                        "solid's compiled dimension operands define its local bounds");
    auto rotated = defaults;
    rotated.format = format(16, 16);
    auto& layer = std::get<runtime::CompiledLayerOutput>(rotated.operations[1]);
    layer.position.source = document::Vec2d{8, 6};
    layer.anchor.source = document::Vec2d{1, -0.5};
    layer.scale.source = document::Vec2d{2, 1};
    layer.rotation.source = 90.0;
    const auto rotatedPlan = publishPlan(rotated);
    const auto result = evaluator.evaluate(rotatedPlan, requestFor(*rotatedPlan), {});
    expectations.expect(result.frame() != nullptr, "off-centre transformed solid evaluates");
    if (!result.frame())
        return;
    const auto& geometry = result.frame()->evaluatedBounds()[1];
    const std::array<document::Vec2d, 4> polygon{{{8.5, 0}, {8.5, 8}, {6.5, 8}, {6.5, 0}}};
    expectations.expect(
        geometry.polygon == polygon && geometry.anchor == document::Vec2d{8, 6},
        "scale then clockwise rotation about off-centre anchor has exact polygon and parent pivot");
    // Analytic coverage: x=[6.5,8.5], y=[0,8], half-covered edge columns 6 and 8.
    expectations.expect(boundsPixelDigest(*result.frame()) == 15538055505238530469ULL,
                        "off-centre rotated solid RGBA32F golden");

    const auto cached = evaluator.evaluate(rotatedPlan, requestFor(*rotatedPlan), {});
    const auto hit = evaluator.evaluate(rotatedPlan, requestFor(*rotatedPlan), {});
    expectations.expect(
        cached.frame() && hit.frame() &&
            hit.frame()->operationCacheStatistics().hits == rotatedPlan->operations().size() &&
            std::ranges::equal(cached.frame()->evaluatedBounds(), hit.frame()->evaluatedBounds()),
        "cache hits retain exact delivered-frame geometry");

    auto nested = twoSolidPlan()->copyDefinition();
    nested.format = format(12, 8);
    localSolid(nested, 0, 4, 2, 90);
    localSolid(nested, 2, 4, 2, 92);
    auto& a = std::get<runtime::CompiledLayerOutput>(nested.operations[1]);
    auto& b = std::get<runtime::CompiledLayerOutput>(nested.operations[3]);
    a.position.source = document::Vec2d{2, 1};
    b.position.source = document::Vec2d{7, 5};
    nested.operations.insert(
        nested.operations.begin() + 5,
        runtime::CompiledMerge{document::NodeId::fromRaw(100),
                               {{{}, {}, runtime::OperationIndex::fromRaw(4)}}});
    std::get<runtime::CompiledCompositionOutput>(nested.operations[6]).input =
        runtime::OperationIndex::fromRaw(5);
    nested.output = runtime::OperationIndex::fromRaw(6);
    const auto nestedPlan = publishPlan(std::move(nested));
    const auto unionResult = evaluator.evaluate(nestedPlan, requestFor(*nestedPlan), {});
    expectations.expect(
        unionResult.frame() &&
            unionResult.frame()->evaluatedBounds()[4].local == runtime::ContentBounds{0, 0, 9, 6} &&
            unionResult.frame()->evaluatedBounds()[5].local == runtime::ContentBounds{0, 0, 9, 6},
        "nested Merge bounds are the union of transformed input content");
}

void testContentBoundsEdgeCases(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    auto animated = oneSolidPlan()->copyDefinition();
    localSolid(animated, 0, 4, 2, 90);
    auto& solid = std::get<runtime::CompiledSolid>(animated.operations[0]);
    solid.width.source = runtime::ScalarCurveIndex::fromRaw(0);
    animated.scalarCurves.push_back(
        {kOpacityCurve,
         {{document::KeyframeId::fromRaw(60), core::RationalTime::fromInteger(0), 4.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(61), core::RationalTime::fromInteger(1), 5.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto animatedPlan = publishPlan(animated);
    auto request = requestFor(*animatedPlan);
    const auto half = core::RationalTime::create(1, 2);
    if (!half)
        throw std::runtime_error("half time");
    request.time = *half;
    const auto frame = evaluator.evaluate(animatedPlan, request, {});
    expectations.expect(frame.frame() && frame.frame()->evaluatedBounds()[0].local ==
                                             runtime::ContentBounds{0, 0, 4.5, 2},
                        "animated fractional dimensions retain exact local pixel-edge bounds");
    request.pixelStorageByteLimit = 1;
    expectations.expect(!evaluator.evaluate(animatedPlan, request, {}).frame(),
                        "cached content cannot bypass the request pixel budget");

    auto text = oneTextPlan({1, 1, 1, 1}, "Ab\ni", 16)->copyDefinition();
    auto& source = std::get<runtime::CompiledText>(text.operations[0]);
    source.layout = runtime::CompiledTextLayout{document::ParameterId::fromRaw(90),
                                                0,
                                                {document::ParameterId::fromRaw(91), 1.0},
                                                {document::ParameterId::fromRaw(92), 0.0}};
    const auto textPlan = publishPlan(text);
    const auto textFrame = evaluator.evaluate(textPlan, requestFor(*textPlan), {});
    expectations.expect(
        textFrame.frame() &&
            textFrame.frame()->evaluatedBounds()[0].local == runtime::ContentBounds{0, 2, 21, 31} &&
            textFrame.frame()->evaluatedBounds()[1].anchor == document::Vec2d{8, 10},
        "multiline text keeps its complete glyph box larger than the frame and centres its anchor");

    auto invalid = oneSolidPlan()->copyDefinition();
    localSolid(invalid, 0, 1000000, 1000000, 90);
    const auto oversized = publishPlan(invalid);
    expectations.expect(!evaluator.evaluate(oversized, requestFor(*oversized), {}).frame(),
                        "oversized local content is refused before allocation");
    std::get<runtime::CompiledSolid>(invalid.operations[0]).height.id = {};
    const auto incomplete = publishPlan(invalid);
    expectations.expect(!evaluator.evaluate(incomplete, requestFor(*incomplete), {}).frame(),
                        "hostile plans cannot omit a required dimension identity");
}

void testParentedBounds(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    auto definition = twoSolidPlan()->copyDefinition();
    definition.format = format(24, 24);
    auto& parent = std::get<runtime::CompiledLayerOutput>(definition.operations[1]);
    parent.position.source = document::Vec2d{8, 6};
    parent.rotation.source = 90.0;
    parent.opacity.source = 0.0;
    parent.inPoint = core::RationalTime::fromInteger(1);
    auto& child = std::get<runtime::CompiledLayerOutput>(definition.operations[3]);
    child.parent = runtime::OperationIndex::fromRaw(1);
    child.position.source = document::Vec2d{3, 2};
    auto& merge = std::get<runtime::CompiledMerge>(definition.operations[4]);
    merge.entries.erase(merge.entries.begin());
    const auto plan = publishPlan(definition);
    const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(result.frame() != nullptr,
                        "rotated parent evaluates outside its own range");
    if (!result.frame())
        return;
    const std::array<document::Vec2d, 4> polygon{{{8, 5}, {8, 9}, {6, 9}, {6, 5}}};
    expectations.expect(
        result.frame()->evaluatedBounds()[3].polygon == polygon &&
            result.frame()->evaluatedBounds()[3].anchor == document::Vec2d{7, 7} &&
            result.frame()->evaluatedBounds()[3].output == runtime::ContentBounds{6, 5, 8, 9},
        "parent times child matrix pins rotated polygon, anchor and bounds numerically");
    auto sampledPixel = render::Rgba32f::transparent();
    expectations.expect(
        pixel(result, 7, 6, sampledPixel) && sampledPixel.blue() == 1.0F &&
            sampledPixel.alpha() == 1.0F && pixel(result, 2, 2, sampledPixel) &&
            sampledPixel.alpha() == 0.0F,
        "child pixels land inside the composed rotated bounds without inheriting parent opacity");
    const auto cached = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(cached.frame() && cached.frame()->operationCacheStatistics().hits ==
                                              plan->operations().size(),
                        "cached parent transform still reaches the child");
    parent.rotation.source = 0.0;
    const auto changed = publishPlan(definition);
    const auto changedResult = evaluator.evaluate(changed, requestFor(*changed), {});
    expectations.expect(
        changedResult.frame() &&
            changedResult.frame()->evaluatedBounds()[3].anchor == document::Vec2d{9, 7} &&
            std::ranges::find(changedResult.frame()->operationCacheStatistics().evaluatedNodes,
                              child.sourceNodeId) !=
                changedResult.frame()->operationCacheStatistics().evaluatedNodes.end(),
        "changing invisible parent resolved rotation invalidates child cache entry");

    auto animated = definition;
    auto& animatedParent = std::get<runtime::CompiledLayerOutput>(animated.operations[1]);
    animatedParent.rotation.source = runtime::ScalarCurveIndex::fromRaw(0);
    animated.scalarCurves.push_back(
        {document::AnimationCurveId::fromRaw(900),
         {{document::KeyframeId::fromRaw(900), core::RationalTime{}, 0.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(901), core::RationalTime::fromInteger(1), 90.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto animatedPlan = publishPlan(animated);
    auto animatedRequest = requestFor(*animatedPlan);
    const auto first = evaluator.evaluate(animatedPlan, animatedRequest, {});
    animatedRequest.time = core::RationalTime::fromInteger(1);
    const auto last = evaluator.evaluate(animatedPlan, animatedRequest, {});
    expectations.expect(
        first.frame() && last.frame() &&
            first.frame()->evaluatedBounds()[3].anchor == document::Vec2d{9, 7} &&
            last.frame()->evaluatedBounds()[3].anchor == document::Vec2d{7, 7} &&
            animatedPlan->operationTimeDependent(runtime::OperationIndex::fromRaw(3)),
        "animated parent values and time dependence propagate through the child cache key");

    auto sheared = definition;
    auto& scaledParent = std::get<runtime::CompiledLayerOutput>(sheared.operations[1]);
    scaledParent.rotation.source = 90.0;
    scaledParent.scale.source = document::Vec2d{2, 1};
    std::get<runtime::CompiledLayerOutput>(sheared.operations[3]).rotation.source = 45.0;
    const auto shearedPlan = publishPlan(sheared);
    const auto shearResult = evaluator.evaluate(shearedPlan, requestFor(*shearedPlan), {});
    const double rootTwo = std::sqrt(2.0);
    const std::array<document::Vec2d, 4> shearPolygon{{{7 + 3 / rootTwo, 8 - rootTwo},
                                                       {7 - 1 / rootTwo, 8 + 3 * rootTwo},
                                                       {7 - 3 / rootTwo, 8 + rootTwo},
                                                       {7 + 1 / rootTwo, 8 - 3 * rootTwo}}};
    bool matchesShear = shearResult.frame() != nullptr;
    if (shearResult.frame())
        for (std::size_t i = 0; i < shearPolygon.size(); ++i) {
            const auto actual = shearResult.frame()->evaluatedBounds()[3].polygon[i];
            matchesShear = matchesShear && std::abs(actual.x - shearPolygon[i].x) < 1e-12 &&
                           std::abs(actual.y - shearPolygon[i].y) < 1e-12;
        }
    expectations.expect(matchesShear, "nonuniformly scaled parent retains composed shear");

    parent.rotation.source = 90.0;
    auto grandparent = parent;
    grandparent.sourceNodeId = document::NodeId::fromRaw(900);
    grandparent.layerId = document::LayerId::fromRaw(900);
    grandparent.position.source = document::Vec2d{12, 1};
    grandparent.rotation.source = 0.0;
    grandparent.position.id = document::ParameterId::fromRaw(900);
    grandparent.anchor.id = document::ParameterId::fromRaw(901);
    grandparent.scale.id = document::ParameterId::fromRaw(902);
    grandparent.rotation.id = document::ParameterId::fromRaw(903);
    grandparent.opacity.id = document::ParameterId::fromRaw(904);
    grandparent.blendModeParameterId = document::ParameterId::fromRaw(905);
    auto chainedParent = parent;
    chainedParent.parent = runtime::OperationIndex::fromRaw(1);
    auto chainedChild = child;
    chainedChild.input = runtime::OperationIndex::fromRaw(3);
    chainedChild.parent = runtime::OperationIndex::fromRaw(2);
    definition.operations = {
        definition.operations[0],
        grandparent,
        chainedParent,
        definition.operations[2],
        chainedChild,
        runtime::CompiledMerge{kStackNode,
                               {{kSlotB, kLayerB, runtime::OperationIndex::fromRaw(4)}}},
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(5)}};
    definition.output = runtime::OperationIndex::fromRaw(6);
    const auto chain = publishPlan(definition);
    const auto chainResult = evaluator.evaluate(chain, requestFor(*chain), {});
    expectations.expect(
        chainResult.frame() &&
            chainResult.frame()->evaluatedBounds()[4].anchor == document::Vec2d{17, 7} &&
            chainResult.frame()->evaluatedBounds()[4].output ==
                runtime::ContentBounds{16, 5, 18, 9},
        "grandparent translation composes with parent rotation and child local placement");
}
