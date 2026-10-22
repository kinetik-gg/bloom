void testImageEffectLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform",
                             "bloom.flip", "bloom.rotate-90", "bloom.resize", "bloom.fit"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(900);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr, "CST definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 900;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "CST parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "CST node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(
            graph.addEdge(
                {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
            "source feeds effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(900),
                     {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "effect feeds Layer");
        expectations.expect(project.validate().ok(), "source/effect/Layer is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "CST graph compiles");
        if (result.plan) {
            const auto effect =
                std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
                    return std::holds_alternative<runtime::CompiledImageEffect>(operation);
                });
            expectations.expect(effect != result.plan->operations().end(),
                                "CST lowers through the reusable image-effect operation");
            if (effect != result.plan->operations().end()) {
                const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
                expectations.expect(
                    lowered.sourceNodeId == effectId &&
                        ((std::string_view(type) == "bloom.flip" ||
                          std::string_view(type) == "bloom.rotate-90")
                             ? std::holds_alternative<runtime::DiscreteImageKernel>(lowered.kernel)
                         : std::string_view(type) == "bloom.ocio-colour-space-transform"
                             ? std::holds_alternative<runtime::CstKernel>(lowered.kernel)
                         : std::string_view(type) == "bloom.ocio-file-transform"
                             ? std::holds_alternative<runtime::FileTransformKernel>(lowered.kernel)
                             : std::holds_alternative<runtime::ResizeFitKernel>(lowered.kernel)),
                    "lowering preserves CST node identity and typed kernel");
            }
        }
    }
}

void testMultiplyInvertLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.multiply", "bloom.invert"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(900);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr, "pointwise definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 900;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "pointwise parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "pointwise node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
                "source feeds pointwise effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(900), {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "pointwise effect feeds Layer");
        expectations.expect(project.validate().ok(), "pointwise graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "pointwise graph compiles");
        if (!result.plan)
            continue;
        const auto operation = std::ranges::find_if(result.plan->operations(), [](const auto& item) {
            return std::holds_alternative<runtime::CompiledImageEffect>(item);
        });
        expectations.expect(operation != result.plan->operations().end(),
                            "pointwise node lowers through the image-effect operation");
        if (operation == result.plan->operations().end())
            continue;
        const auto& lowered = std::get<runtime::CompiledImageEffect>(*operation);
        const auto* kernel = std::get_if<runtime::PointwiseImageKernel>(&lowered.kernel);
        const bool isMultiply = std::string_view(type) == "bloom.multiply";
        expectations.expect(
            lowered.sourceNodeId == effectId && kernel &&
                kernel->effect == (isMultiply ? runtime::PointwiseImageEffect::Multiply
                                             : runtime::PointwiseImageEffect::Invert) &&
                (isMultiply ? kernel->amount &&
                                  std::get_if<double>(&kernel->amount->source) &&
                                  *std::get_if<double>(&kernel->amount->source) == 1.0
                            : !kernel->amount),
            "Multiply and Invert retain typed operation, neutral factor and node identity");
    }
}

void testExposureOffsetLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.exposure", "bloom.offset"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(920);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr && definition->parameters.size() == 1,
                "pointwise definition exists with one scalar control");
        const auto parameterId = ParameterId::fromRaw(920);
        require(composition->parameters().insert(
                    {parameterId, definition->parameters.front().schemaKey,
                     ConstantValueSource{1.25}}),
                "pointwise amount is valid");
        require(graph.addNode({effectId, definition->key.typeId, {{"amount", parameterId}}, 1}),
                "pointwise node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
                "source feeds pointwise effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(920), {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "pointwise effect feeds Layer");
        expectations.expect(project.validate().ok(), "pointwise graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "pointwise graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledImageEffect>(operation);
        });
        expectations.expect(effect != result.plan->operations().end(),
                            "pointwise lowers through image-effect operation");
        if (effect == result.plan->operations().end())
            continue;
        const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
        const auto* kernel = std::get_if<runtime::PointwiseImageKernel>(&lowered.kernel);
        expectations.expect(kernel && kernel->amount &&
                                kernel->effect ==
                                    (std::string_view(type) == "bloom.exposure"
                                         ? runtime::PointwiseImageEffect::Exposure
                                         : runtime::PointwiseImageEffect::Offset) &&
                                kernel->amount->id == parameterId,
                            "pointwise lowering retains mode and editable parameter identity");
    }
}
