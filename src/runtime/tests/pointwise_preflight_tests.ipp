// Included by image_effect_tests.cpp inside its anonymous namespace.
void testPointwiseAnimatedPreflight() {
    using runtime::CompiledScalarParameter;
    using runtime::PointwiseImageEffect;
    using runtime::PointwiseImageKernel;
    const auto parameterId = document::ParameterId::fromRaw(90);
    const auto curveId = document::AnimationCurveId::fromRaw(91);
    for (const auto kind : {PointwiseImageEffect::Exposure, PointwiseImageEffect::Offset,
                            PointwiseImageEffect::Multiply}) {
        const auto base =
            planWith({PointwiseImageKernel{kind, CompiledScalarParameter{parameterId, 0.0}}});
        auto definition = base->copyDefinition();
        definition.duration = core::RationalTime::fromInteger(2);
        auto& amount = std::get<PointwiseImageKernel>(
                           std::get<runtime::CompiledImageEffect>(definition.operations[1]).kernel)
                           .amount;
        amount->source = runtime::ScalarCurveIndex::fromRaw(0);
        definition.scalarCurves.push_back(
            {curveId,
             {{document::KeyframeId::fromRaw(92), core::RationalTime::fromInteger(0), -1.0,
               runtime::CompiledKeyframeInterpolation::Linear},
              {document::KeyframeId::fromRaw(93), core::RationalTime::fromInteger(1), 1.0,
               runtime::CompiledKeyframeInterpolation::Linear}}});
        const auto animated =
            std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
        runtime::CpuCompositionEvaluator evaluator;
        auto request = requestFor(*animated);
        const auto start = evaluator.evaluate(animated, request, {});
        request.time = core::RationalTime::fromInteger(1);
        const auto end = evaluator.evaluate(animated, request, {});
        const auto warm = evaluator.evaluate(animated, request, {});
        expect(start.frame() && end.frame() && start.diagnostics().empty() &&
                   end.diagnostics().empty(),
               "pointwise amount owns its animation curve and admits negative keys");
        expect(start.frame() && end.frame() && !samePixels(start, end),
               "keyframed pointwise amount changes the evaluated pixels");
        expect(samePixels(end, warm), "keyframed pointwise amount retains exact warm pixels");

        auto duplicate = animated->copyDefinition();
        std::get<PointwiseImageKernel>(
            std::get<runtime::CompiledImageEffect>(duplicate.operations[1]).kernel)
            .amount->id = std::get<runtime::CompiledSolid>(duplicate.operations[0]).color.id;
        const auto invalid =
            std::make_shared<const runtime::CompiledCompositionPlan>(std::move(duplicate));
        const auto rejected = evaluator.evaluate(invalid, requestFor(*invalid), {});
        expect(!rejected.frame() && !rejected.diagnostics().empty() &&
                   rejected.diagnostics().front().code ==
                       runtime::EvaluationDiagnosticCode::InvalidPlan &&
                   rejected.diagnostics().front().subject.parameterId ==
                       std::get<runtime::CompiledSolid>(invalid->operations()[0]).color.id,
               "pointwise amounts participate in global parameter identity validation");
    }
}
