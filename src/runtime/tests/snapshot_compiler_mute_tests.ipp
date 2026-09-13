[[nodiscard]] runtime::EvaluationResult
evaluateMuteProof(const runtime::SnapshotCompileResult& compiled) {
    require(compiled.plan != nullptr, "mute proof compiles");
    return runtime::CpuCompositionEvaluator{}.evaluate(
        compiled.plan,
        {.time = {},
         .output = compiled.plan->output(),
         .resolution = runtime::CompositionFormatResolution{},
         .quality = runtime::EvaluationQuality::Reference,
         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
         .pixelStorageByteLimit = 1U << 20U},
        runtime::CancellationToken{});
}

[[nodiscard]] document::Project muteProject(const bool twoLayers = false) {
    auto options = singleLayerOptions();
    options.secondLayer = twoLayers;
    options.format = requireValue(document::CompositionFormat::create(4, 2), "mute extent");
    auto project = makeProject(options);
    auto& parameters = project.findComposition(kCompositionId)->parameters();
    require(
        parameters.setSource(kFirstPosition, document::ConstantValueSource{document::Vec2d{2, 1}}),
        "center first layer");
    if (twoLayers)
        require(parameters.setSource(kSecondPosition,
                                     document::ConstantValueSource{document::Vec2d{2, 1}}),
                "center second layer");
    return project;
}

void testMuteKindsAndPixels(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "mute built-ins");
    registry.freeze();
    for (const auto id : {kFirstSolidNode, kFirstLayerNode, kStackNode, kOutputNode}) {
        auto project = muteProject();
        project.findComposition(kCompositionId)->nodeLayout().at(id).muted = true;
        const auto compiled = compile(std::move(project), registry);
        const auto evaluated = evaluateMuteProof(compiled);
        expectations.expect(evaluated.status() == runtime::EvaluationStatus::Evaluated &&
                                evaluated.frame(),
                            "muted built-in evaluates using unchanged CPU primitives");
        if (!evaluated.frame())
            continue;
        if (id == kFirstSolidNode || id == kFirstLayerNode) {
            expectations.expect(std::ranges::all_of(evaluated.frame()->processImage().pixels(),
                                                    [](const auto& pixel) {
                                                        return pixel ==
                                                               render::Rgba32f::transparent();
                                                    }),
                                "muted source/boundary contributes transparent pixels");
        } else {
            const auto expected = evaluateMuteProof(compile(muteProject(), registry));
            expectations.expect(expected.frame() &&
                                    std::ranges::equal(expected.frame()->processImage().pixels(),
                                                       evaluated.frame()->processImage().pixels()),
                                "muted single-input stack/endpoint passes exact pixels");
        }
    }
    auto two = muteProject(true);
    two.findComposition(kCompositionId)->nodeLayout().at(kStackNode).muted = true;
    const auto firstOnly = evaluateMuteProof(compile(std::move(two), registry));
    const auto single = evaluateMuteProof(compile(muteProject(), registry));
    expectations.expect(firstOnly.frame() && single.frame() &&
                            std::ranges::equal(firstOnly.frame()->processImage().pixels(),
                                               single.frame()->processImage().pixels()),
                        "muted stack passes only its first stable slot");

    // ADAPTED (task S3): a text source used to be the repository's only "recognized but
    // unevaluable" node, so this block proved the UnsupportedNode diagnostic was scoped to that
    // exact node and that muting it removed the need for a text renderer. Text now lowers and
    // evaluates, so what is proved here is the mute behavior itself against a text source: unmuted
    // it compiles and paints glyph coverage; muted it is transparent; and a muted Layer Output
    // still prunes it entirely. The diagnostic-scoping proof moves to the "test.bypass" schema
    // below, which is still an Unsupported lowering.
    auto text = muteProject();
    auto* composition = text.findComposition(kCompositionId);
    auto* node = composition->graph().findNode(kFirstSolidNode);
    node->typeId = std::string(document::kTextSourceNodeType);
    node->schemaVersion = document::kTextSourceNodeSchemaVersion;
    node->parameters = {{std::string(document::kTextParameterRole), kFirstColor},
                        {std::string(document::kTextSizeParameterRole), kTextSize},
                        {std::string(document::kTextColorParameterRole), kTextColor}};
    require(composition->parameters().erase(kFirstColor) &&
                composition->parameters().insert(
                    {kFirstColor, std::string(document::kTextParameterSchemaKey),
                     document::ConstantValueSource{std::string("Legacy text")}}) &&
                composition->parameters().insert(
                    {kTextSize, std::string(document::kTextSizeParameterSchemaKey),
                     document::ConstantValueSource{8.0}}) &&
                composition->parameters().insert(
                    {kTextColor, std::string(document::kTextColorParameterSchemaKey),
                     document::ConstantValueSource{core::Color4d{1.0, 1.0, 1.0, 1.0}}}),
            "text source");
    const auto drawn = compile(text, registry);
    expectations.expect(drawn.status == runtime::SnapshotCompileStatus::Compiled &&
                            drawn.diagnostics.empty(),
                        "an unmuted text source compiles with no diagnostics");
    const auto drawnFrame = evaluateMuteProof(drawn);
    expectations.expect(drawnFrame.frame() &&
                            std::ranges::any_of(drawnFrame.frame()->processImage().pixels(),
                                                [](const auto& pixel) {
                                                    return pixel != render::Rgba32f::transparent();
                                                }),
                        "and paints glyph coverage into the composed frame");
    composition->nodeLayout().at(kFirstSolidNode).muted = true;
    const auto emptyText = evaluateMuteProof(compile(text, registry));
    expectations.expect(emptyText.frame() &&
                            std::ranges::all_of(emptyText.frame()->processImage().pixels(),
                                                [](const auto& pixel) {
                                                    return pixel == render::Rgba32f::transparent();
                                                }),
                        "a muted text source contributes nothing, exactly like a muted solid");
    composition->nodeLayout().at(kFirstSolidNode).muted = false;
    composition->nodeLayout().at(kFirstLayerNode).muted = true;
    const auto omitted = compile(std::move(text), registry);
    expectations.expect(omitted.plan && omitted.diagnostics.empty() &&
                            omitted.plan->operations().size() == 2,
                        "muted Layer Output prunes its upstream text source entirely");
}

void testMuteFirstImageInput(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "bypass built-ins");
    require(registry.registerDefinition({{"test.bypass", 1},
                                         runtime::NodeLoweringKind::Unsupported,
                                         {{"scalar", runtime::SocketValueKind::Scalar, true},
                                          {"first", runtime::SocketValueKind::Image, true},
                                          {"second", runtime::SocketValueKind::Image, true}},
                                         {{"image", runtime::SocketValueKind::Image}},
                                         {},
                                         std::nullopt}) ==
                runtime::NodeRegistrationStatus::Registered,
            "bypass schema");
    registry.freeze();
    auto project = muteProject();
    auto* composition = project.findComposition(kCompositionId);
    const auto& original = composition->graph();
    const auto bridge = document::NodeId::fromRaw(99);
    document::CanonicalGraph graph(kStackNode);
    for (const auto& node : original.nodes())
        require(graph.addNode(node), "copy node");
    require(graph.addNode({bridge, "test.bypass", {}, 1}), "bridge node");
    for (const auto& boundary : original.layerOutputs())
        require(graph.addLayerOutput(boundary), "copy boundary");
    for (const auto& entry : original.layerStack().entries())
        require(graph.layerStack().append(entry), "copy slot");
    for (auto edge : original.edges()) {
        if (edge.id == kFirstSourceEdge)
            edge.destination = document::NodeInputRef{bridge, "first"};
        require(graph.addEdge(edge, registry), "copy edge");
    }
    require(graph.addEdge({document::EdgeId::fromRaw(99),
                           {bridge, "image"},
                           document::NodeInputRef{kFirstLayerNode, "image"}},
                          registry),
            "bridge output");
    graph.setCompositionOutput(requireValue(original.compositionOutput(), "original output"));
    composition->graph() = std::move(graph);
    composition->nodeLayout()[bridge].muted = true;
    const auto compiled = compile(std::move(project), registry);
    const auto evaluated = evaluateMuteProof(compiled);
    const auto reference = evaluateMuteProof(compile(muteProject(), registry));
    expectations.expect(
        evaluated.frame() && reference.frame() &&
            std::ranges::equal(evaluated.frame()->processImage().pixels(),
                               reference.frame()->processImage().pixels()),
        "mute selects the first Image input and ignores scalar and later required inputs");
}

void testLayerFlagsAndRangeLowering(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "timeline built-ins"); registry.freeze();
    const auto reference = evaluateMuteProof(compile(muteProject(), registry));
    auto project = muteProject(true);
    auto* composition = project.findComposition(kCompositionId);
    const auto firstId = composition->graph().layerOutputs().front().layerId;
    auto* layer = composition->graph().findLayer(firstId);
    layer->solo = true;
    const auto solo = compile(project, registry);
    const auto result = evaluateMuteProof(solo);
    expectations.expect(result.frame() && reference.frame() && std::ranges::equal(result.frame()->processImage().pixels(), reference.frame()->processImage().pixels()), "solo prunes other entries and preserves the chosen layer's pixels");
    layer->enabled = false;
    const auto hidden = compile(project, registry);
    const auto hiddenFrame = evaluateMuteProof(hidden);
    expectations.expect(hidden.plan && hidden.plan->operations().size() == 2 && hiddenFrame.frame() && std::ranges::all_of(hiddenFrame.frame()->processImage().pixels(), [](const auto& p) { return p == render::Rgba32f::transparent(); }), "disabled solo layer leaves no stack entry or upstream operations");
    layer->enabled = true; layer->inPoint = core::RationalTime::fromInteger(1); layer->outPoint = core::RationalTime::fromInteger(2);
    const auto ranged = compile(project, registry);
    const auto before = evaluateMuteProof(ranged);
    expectations.expect(before.frame() && std::ranges::all_of(before.frame()->processImage().pixels(), [](const auto& p) { return p == render::Rgba32f::transparent(); }), "compiler carries durable range into evaluator absence");
}
