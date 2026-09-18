void testImageEffectLowering(Expectations& expectations) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    const auto effectId = NodeId::fromRaw(900);
    const auto* definition = builtInNodeDefinitions().find("bloom.ocio-colour-space-transform", 1);
    require(definition != nullptr, "CST definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 900;
    for (const auto& parameter : definition->parameters) {
        const auto id = ParameterId::fromRaw(next++);
        require(composition->parameters().insert({id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                "CST parameter is valid");
        bindings.push_back({parameter.role, id});
    }
    require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}), "CST node is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}), "source feeds effect");
    require(graph.addEdge({EdgeId::fromRaw(900), {effectId, "image"}, NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}), "effect feeds Layer");
    expectations.expect(project.validate().ok(), "source/effect/Layer is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto result = compile(std::move(project), registry);
    expectations.expect(result.plan != nullptr, "CST graph compiles");
    if (result.plan) {
        const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledImageEffect>(operation);
        });
        expectations.expect(effect != result.plan->operations().end(), "CST lowers through the reusable image-effect operation");
        if (effect != result.plan->operations().end()) {
            const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
            expectations.expect(lowered.sourceNodeId == effectId && std::holds_alternative<runtime::CstKernel>(lowered.kernel),
                                "lowering preserves CST node identity and typed kernel");
        }
    }
}
