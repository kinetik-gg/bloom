// #399: a presentation edit -- card position, width and collapse, a group frame, a layer's name and
// label color, the composition and project names -- compiles to the same plan content, and the
// operation cache answers every operation of the next frame without evaluating anything.
[[nodiscard]] document::Project presentationProject() {
    auto options = singleLayerOptions();
    options.format = requireValue(document::CompositionFormat::create(4, 2), "layout extent");
    auto project = makeProject(options);
    require(project.findComposition(kCompositionId)
                ->parameters()
                .setSource(kFirstPosition, document::ConstantValueSource{document::Vec2d{2, 1}}),
            "center the layer");
    return project;
}

void testPresentationEditKeepsPlanAndOperationCache(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "presentation built-ins");
    registry.freeze();
    document::Document document(presentationProject());
    const runtime::SnapshotCompiler compiler(registry);
    const auto before = document.snapshot();
    const auto first = compiler.compile({before, kCompositionId}, runtime::CancellationToken{});
    require(first.plan != nullptr, "presentation fixture compiles");

    auto draft = document.draft(before);
    auto* composition = draft.project().findComposition(kCompositionId);
    auto& card = composition->nodeLayout().at(kFirstSolidNode);
    card.position = {640.0, -32.0};
    card.width = 310.0;
    card.collapsed = true;
    const auto groupId = document::NodeGroupId::fromRaw(900);
    composition->nodeGroups().emplace(
        groupId,
        document::NodeGroupRecord{
            .id = groupId, .name = "Frame", .members = {kFirstSolidNode}, .padding = {48.0, 48.0}});
    auto* layer = composition->graph().findLayer(kFirstLayer);
    layer->name = "Renamed";
    layer->labelColor = std::array<std::uint8_t, 3>{10, 200, 90};
    composition->setName("Renamed composition");
    draft.project().setName("Renamed project");
    const auto committed = document.commit(before.revision(), std::move(draft));
    require(committed.committed() && committed.snapshot.has_value(), "presentation edit commits");
    const auto& after = *committed.snapshot;
    expectations.expect(after.revision() != before.revision() &&
                            document::sameEvaluationInputs(before.project(), after.project()),
                        "a presentation edit advances the document but not its evaluation inputs");

    const auto second = compiler.compile({after, kCompositionId}, runtime::CancellationToken{});
    require(second.plan != nullptr, "presentation edit compiles");
    auto firstDefinition = first.plan->copyDefinition();
    auto secondDefinition = second.plan->copyDefinition();
    secondDefinition.sourceRevision = firstDefinition.sourceRevision;
    expectations.expect(firstDefinition == secondDefinition,
                        "the plan differs from the pre-edit plan only in its source revision");

    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::EvaluationRequest request{.time = {},
                                             .output = first.plan->output(),
                                             .resolution = runtime::CompositionFormatResolution{},
                                             .quality = runtime::EvaluationQuality::Reference,
                                             .colorIntent =
                                                 runtime::EvaluationColorIntent::LinearRec709Scene,
                                             .pixelStorageByteLimit = 1U << 20U};
    const auto warm = evaluator.evaluate(first.plan, request, runtime::CancellationToken{});
    runtime::OperationCacheStatistics statistics;
    const auto next = evaluator.evaluate(second.plan, request, runtime::CancellationToken{}, {},
                                         nullptr, &statistics);
    expectations.expect(warm.frame() && next.frame() && statistics.misses == 0 &&
                            statistics.hits > 0 && statistics.evaluatedNodes.empty(),
                        "the next frame after a presentation edit is an operation-cache hit");
    if (warm.frame() && next.frame())
        expectations.expect(std::ranges::equal(warm.frame()->processImage().pixels(),
                                               next.frame()->processImage().pixels()),
                            "the cached frame is the pre-edit frame");

    // A mute is the one node-layout field evaluation reads, and it does invalidate.
    auto muteDraft = document.draft(after);
    muteDraft.project().findComposition(kCompositionId)->nodeLayout().at(kFirstSolidNode).muted =
        true;
    const auto muted = document.commit(after.revision(), std::move(muteDraft));
    require(muted.committed() && muted.snapshot.has_value(), "mute commits");
    expectations.expect(!document::sameEvaluationInputs(after.project(), muted.snapshot->project()),
                        "a mute changes the evaluation inputs");
    const auto mutedPlan =
        compiler.compile({*muted.snapshot, kCompositionId}, runtime::CancellationToken{});
    expectations.expect(mutedPlan.plan != nullptr && !(*mutedPlan.plan == *first.plan),
                        "a mute compiles a different plan");
    auto mutedRequest = request;
    if (mutedPlan.plan)
        mutedRequest.output = mutedPlan.plan->output();
    const auto mutedFrame =
        evaluator.evaluate(mutedPlan.plan, mutedRequest, runtime::CancellationToken{});
    expectations.expect(mutedFrame.frame() && warm.frame() &&
                            !std::ranges::equal(warm.frame()->processImage().pixels(),
                                                mutedFrame.frame()->processImage().pixels()),
                        "the muted frame is not served from the pre-mute cache");
}
