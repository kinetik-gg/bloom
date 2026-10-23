void testImageEffectLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform"}) {
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
                        (std::string_view(type) == "bloom.ocio-colour-space-transform"
                             ? std::holds_alternative<runtime::CstKernel>(lowered.kernel)
                             : std::holds_alternative<runtime::FileTransformKernel>(
                                   lowered.kernel)),
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

void testCanvasLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.crop", "bloom.pad"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(940);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr && definition->parameters.size() == 4,
                "canvas definition has four margins");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 940;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{1.0}}),
                    "canvas margin is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "canvas node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds canvas effect");
        require(graph.addEdge({EdgeId::fromRaw(940), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "canvas effect feeds Layer");
        expectations.expect(project.validate().ok(), "canvas graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "canvas graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& op) {
            return std::holds_alternative<runtime::CompiledImageEffect>(op);
        });
        expectations.expect(effect != result.plan->operations().end(),
                            "canvas node lowers through the image-effect operation");
        if (effect == result.plan->operations().end())
            continue;
        const auto* kernel = std::get_if<runtime::CanvasImageKernel>(
            &std::get<runtime::CompiledImageEffect>(*effect).kernel);
        expectations.expect(kernel &&
                                kernel->effect ==
                                    (std::string_view(type) == "bloom.crop"
                                         ? runtime::CanvasImageEffect::Crop
                                         : runtime::CanvasImageEffect::Pad) &&
                                kernel->margins[0].id == ParameterId::fromRaw(940) &&
                                kernel->margins[3].id == ParameterId::fromRaw(943),
                            "canvas lowering retains mode and editable margin identities");
        const auto effectIndex = static_cast<std::size_t>(
            std::distance(result.plan->operations().begin(), effect));
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)),
                            "constant canvas margins remain time invariant");
        auto animatedDefinition = result.plan->copyDefinition();
        auto& animatedKernel = std::get<runtime::CanvasImageKernel>(
            std::get<runtime::CompiledImageEffect>(
                animatedDefinition.operations[effectIndex]).kernel);
        animatedKernel.margins[0].source = runtime::ScalarCurveIndex::fromRaw(0);
        animatedDefinition.scalarCurves.push_back(
            runtime::CompiledScalarCurve{AnimationCurveId::fromRaw(940),
                                         {{KeyframeId::fromRaw(940),
                                           core::RationalTime::fromInteger(0), 0.0},
                                          {KeyframeId::fromRaw(941),
                                           core::RationalTime::fromInteger(1), 1.0}}});
        const auto animated = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(animatedDefinition));
        expectations.expect(animated->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)) &&
                                animated->operationTimeDependent(animated->output()),
                            "animated canvas margin invalidates itself and downstream output");
        auto drivenDefinition = result.plan->copyDefinition();
        auto& drivenKernel = std::get<runtime::CanvasImageKernel>(
            std::get<runtime::CompiledImageEffect>(drivenDefinition.operations[effectIndex]).kernel);
        drivenKernel.margins[3].source = runtime::ValueOutputIndex::fromRaw(0);
        drivenDefinition.valueOperations = {
            {NodeId::fromRaw(950), runtime::ValueOutputIndex::fromRaw(0), 2,
             runtime::CompiledValueTime{}}};
        drivenDefinition.valueOutputCount = 2;
        const auto driven = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(drivenDefinition));
        expectations.expect(driven->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)) &&
                                driven->operationTimeDependent(driven->output()),
                            "driven canvas margin invalidates itself and downstream output");
    }
}

void testBlurLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.gaussian-blur", "bloom.box-blur"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(960);
        const auto radius = ParameterId::fromRaw(960);
        const auto edge = ParameterId::fromRaw(961);
        require(composition->parameters().insert(
                    {radius, "bloom.blur.radius", ConstantValueSource{2.5}}),
                "blur radius is valid");
        require(composition->parameters().insert(
                    {edge, "bloom.blur.edge", ConstantValueSource{std::int64_t{1}}}),
                "blur edge is valid");
        require(graph.addNode({node, type, {{"radius", radius}, {"edge", edge}}, 1}),
                "blur node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters blur");
        require(graph.addEdge(
                    {EdgeId::fromRaw(960),
                     {node, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "blur enters Layer");
        expectations.expect(project.validate().ok(), "blur document validates");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "blur graph compiles");
        if (!result.plan)
            continue;
        const auto operation =
            std::ranges::find_if(result.plan->operations(), [](const auto& value) {
                return std::holds_alternative<runtime::CompiledImageEffect>(value);
            });
        require(operation != result.plan->operations().end(), "blur operation exists");
        const auto* blur = std::get_if<runtime::SeparableBlurKernel>(
            &std::get<runtime::CompiledImageEffect>(*operation).kernel);
        expectations.expect(blur && blur->radius.id == radius &&
                                blur->edge == runtime::BlurEdgeMode::Clamp &&
                                blur->kind == (std::string_view(type) == "bloom.gaussian-blur"
                                                   ? runtime::SeparableBlurKind::Gaussian
                                                   : runtime::SeparableBlurKind::Box),
                            "typed blur lowering retains parameter identity, kind and edge policy");
    }
}

void testImageCombineChannelLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "combine built-ins register");
    registry.freeze();
    for (const auto* type : {"bloom.set-alpha", "bloom.multiply-alpha", "bloom.image-mix"}) {
        // Scenarios: 0 constant, 1 linked Integer, 2 linked frame number, 3 empty matte source,
        // 4 unconnected matte (#361), 5 constant inverted selector 7 = Green Inverted (#361).
        for (int scenario = 0; scenario != 6; ++scenario) {
            const bool mix = std::string_view(type) == "bloom.image-mix";
            if (mix && scenario != 3)
                continue;
            auto project = makeProject();
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            const auto effectId = NodeId::fromRaw(900);
            const auto driverId = NodeId::fromRaw(910);
            const auto* definition = builtInNodeDefinitions().find(type, 1);
            require(definition != nullptr, "combine definition exists");
            std::vector<ParameterBinding> bindings;
            ParameterId channel;
            std::uint64_t next = 900;
            for (const auto& parameter : definition->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                        "combine parameter is valid");
                bindings.push_back({parameter.role, id});
                if (parameter.role == "channel")
                    channel = id;
            }
            require(graph.addNode({effectId, type, std::move(bindings), 1}),
                    "combine node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}), "base feeds combine");
            if (scenario != 4)
                require(graph.addEdge({EdgeId::fromRaw(900), {kSecondSolidNode, "image"},
                                       NodeInputRef{effectId, mix ? "second" : "matte"}}),
                        "second image feeds combine");
            if (scenario == 5)
                require(composition->parameters().setSource(
                            channel, ConstantValueSource{std::int64_t{7}}),
                        "the closed matte selector admits an inverted channel");
            require(graph.addEdge({EdgeId::fromRaw(901), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "combine feeds layer");
            if (scenario == 1) {
                attachValueDriver(project, driverId, ParameterId::fromRaw(910),
                                  kIntegerValueNodeType, kIntegerValueParameterSchemaKey,
                                  std::int64_t{4}, channel);
            } else if (scenario == 2) {
                require(graph.addNode({driverId, std::string(kFrameNumberNodeType), {},
                                       kValueNodeSchemaVersion}), "frame driver is accepted");
                require(composition->parameters().setSource(
                            channel, DriverBindingSource{driverId, std::string(kResultPortName)}),
                        "frame driver connects to matte channel");
            } else if (scenario == 3) {
                composition->nodeLayout().at(kSecondSolidNode).muted = true;
            }
            expectations.expect(project.validate().ok(), "combine document remains valid");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "constant, linked and empty-matte combine compiles");
            if (!result.plan)
                continue;
            const auto operation = std::ranges::find_if(result.plan->operations(),
                [effectId](const auto& item) {
                    const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
                    return effect && effect->sourceNodeId == effectId;
                });
            require(operation != result.plan->operations().end(), "combine operation exists");
            const auto& effect = std::get<runtime::CompiledImageEffect>(*operation);
            const auto& kernel = std::get<runtime::ImageCombineKernel>(effect.kernel);
            expectations.expect(kernel.drivenChannel.has_value() == (scenario == 1 || scenario == 2),
                                "linked matte channel retains its value output");
            const auto index = runtime::OperationIndex::fromRaw(static_cast<std::size_t>(
                std::distance(result.plan->operations().begin(), operation)));
            expectations.expect(result.plan->operationTimeDependent(index) == (scenario == 2) &&
                                    result.plan->operationTimeDependent(result.plan->output()) ==
                                        (scenario == 2),
                                "only a time-dependent matte channel invalidates downstream frames");
            if (scenario == 5)
                expectations.expect(kernel.channel == render::ImageCoverageChannel::Green &&
                                        kernel.invert,
                                    "stored selector 7 lowers to the inverted green matte");
            else
                expectations.expect(!kernel.invert, "selectors 0-4 lower without invert");
            if (scenario == 3 || scenario == 4) {
                const auto* empty = effect.secondaryInput
                    ? std::get_if<runtime::CompiledMerge>(
                          &result.plan->operations()[effect.secondaryInput->value()])
                    : nullptr;
                expectations.expect(empty && empty->entries.empty(),
                                    "empty matte materializes a transparent image operation");
            }
        }
    }
}
