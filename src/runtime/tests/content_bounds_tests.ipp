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
    const auto legacy = oneSolidPlan();
    auto defaults = legacy->copyDefinition();
    localSolid(defaults, 0, 4, 2, 90);
    std::get<runtime::CompiledLayerOutput>(defaults.operations[1]).localBounds = true;
    std::get<runtime::CompiledMerge>(defaults.operations[2]).localBounds = true;
    const auto current = publishPlan(defaults);
    const auto before = evaluator.evaluate(legacy, requestFor(*legacy), {});
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
    expectations.expect(boundsPixelDigest(*result.frame()) == 9919536445715000421ULL,
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
    a.localBounds = b.localBounds = true;
    a.position.source = document::Vec2d{2, 1};
    b.position.source = document::Vec2d{7, 5};
    std::get<runtime::CompiledMerge>(nested.operations[4]).localBounds = true;
    nested.operations.insert(nested.operations.begin() + 5,
                             runtime::CompiledMerge{document::NodeId::fromRaw(100),
                                                    {{{}, {}, runtime::OperationIndex::fromRaw(4)}},
                                                    true});
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
    std::get<runtime::CompiledLayerOutput>(animated.operations[1]).localBounds = true;
    std::get<runtime::CompiledMerge>(animated.operations[2]).localBounds = true;
    auto& solid = std::get<runtime::CompiledSolid>(animated.operations[0]);
    if (!solid.width)
        throw std::runtime_error("width operand");
    solid.width->source = runtime::ScalarCurveIndex::fromRaw(0);
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
    std::get<runtime::CompiledLayerOutput>(text.operations[1]).localBounds = true;
    std::get<runtime::CompiledMerge>(text.operations[2]).localBounds = true;
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
    std::get<runtime::CompiledSolid>(invalid.operations[0]).height.reset();
    const auto incomplete = publishPlan(invalid);
    expectations.expect(!evaluator.evaluate(incomplete, requestFor(*incomplete), {}).frame(),
                        "hostile plans cannot specify only one solid dimension");
}
