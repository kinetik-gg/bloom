// Issue #355: reachability-proven edits keep RAM-preview frames, and switching compositions keeps
// each composition's retained spans.

[[nodiscard]] bloom::document::ParameterId
layerParameter(const bloom::ui::CompositionSession& session, const bloom::document::NodeId node,
               const std::string_view role) {
    const auto* record = session.composition()->graph().findNode(node);
    if (record != nullptr)
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
    throw std::logic_error("fixture layer parameter");
}

void testReachabilityProvenEditsKeepFrames(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Reachability");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    std::atomic<int> preparations{0};
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&preparations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            ++preparations;
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Background"), core::Color4d{0.1, 0.2, 0.3, 1.0}) &&
            session.addSolidLayer(QStringLiteral("Clip"), core::Color4d{0.9, 0.4, 0.1, 1.0}),
        "the reachability fixture has a background and a clip");
    const auto clip = session.composition()->graph().layerOutputs().back();
    commands::Transaction trim("Clip range", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, clip.layerId,
                                          core::RationalTime::fromInteger(4),
                                          core::RationalTime::fromInteger(8));
    expectations.expect(session.executeTransaction(std::move(trim)).changed(),
                        "the clip is finite [4,8)");
    const auto warm = [&](const std::int64_t seconds) {
        static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(seconds)));
        return waitUntil([&] { return isReady(controller); });
    };
    expectations.expect(warm(1) && warm(5) && warm(9) && warm(1), "times 1, 5, 9 warm");
    const auto warmed = preparations.load();
    const auto retainedOutside = controller.state().frame;

    // An unconnected node added and deleted keeps every frame, with no preparation.
    commands::Transaction add("Add loose node", session.snapshot().revision());
    add.emplace<commands::AddNode>(compositionId, std::string(document::kSolidSourceNodeType),
                                   document::Vec2d{});
    const auto added = session.executeTransaction(std::move(add));
    const auto loose = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    expectations.expect(added.changed() && loose.has_value() && preparations.load() == warmed &&
                            controller.state().frame == retainedOutside,
                        "adding an unconnected node keeps the displayed frame unprepared");
    commands::Transaction remove("Delete loose node", session.snapshot().revision());
    remove.emplace<commands::RemoveNodes>(compositionId, std::set<document::NodeId>{*loose});
    expectations.expect(session.executeTransaction(std::move(remove)).changed() &&
                            preparations.load() == warmed,
                        "deleting an unconnected node keeps every frame");
    expectations.expect(warm(5) && warm(9) && warm(1) && preparations.load() == warmed,
                        "every warmed time is still served from the RAM preview");

    // A clip parameter edit re-derives only the clip span. Re-warming published a re-stamped
    // cache hit, so the displayed frame is captured again here.
    const auto displayedOutside = controller.state().frame;
    commands::Transaction opacity("Clip opacity", session.snapshot().revision());
    opacity.emplace<commands::SetParameterSource>(
        compositionId, layerParameter(session, clip.nodeId, document::kOpacityParameterRole),
        document::ConstantValueSource{0.5});
    const auto opacityResult = session.executeTransaction(std::move(opacity));
    expectations.expect(opacityResult.changed() && preparations.load() == warmed &&
                            controller.state().frame == displayedOutside,
                        "an edit inside the clip keeps the outside frame unprepared");
    expectations.expect(warm(9) && preparations.load() == warmed,
                        "the far outside time is still served from the RAM preview");
    expectations.expect(warm(5) && preparations.load() == warmed + 1,
                        "only the clip span re-prepares");
    const runtime::PreviewRequestIdentity identity{
        .projectId = session.snapshot().project().id(),
        .compositionId = compositionId,
        .sourceRevision = session.snapshot().revision(),
        .requestGeneration = 1,
        .time = core::RationalTime::fromInteger(1),
        .output = runtime::PreviewOutput::Composition,
        .resolution = controller.resolution(),
        .quality = controller.settings().quality,
        .colorIntent = controller.settings().colorIntent,
        .resolutionPolicy = controller.settings().resolutionPolicy,
        .roi = controller.regionOfInterest(),
        .displayName = controller.settings().displayName,
        .viewName = controller.settings().viewName,
        .showLook = controller.settings().showLook,
    };
    const auto oracle =
        prepareLiveOracle(scheduler, fixture.pipeline, session.snapshot(), identity);
    expectations.expect(warm(1) && preparations.load() == warmed + 1 &&
                            sameDisplayPixels(controller.state().frame, oracle),
                        "the retained outside frame matches the post-edit live oracle");
    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testCompositionSwitchRetainsUnaffectedCache(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Switch Retention");
    const auto first = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, first);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    std::atomic<int> preparations{0};
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&preparations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            ++preparations;
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Background"), core::Color4d{0.1, 0.2, 0.3, 1.0}),
        "the first composition has a layer");
    commands::Transaction duplicate("Duplicate", session.snapshot().revision());
    duplicate.emplace<commands::DuplicateComposition>(first);
    const auto copy = session.executeTransaction(std::move(duplicate))
                          .outputId<document::CompositionId>(commands::kDuplicateCompositionOutput);
    expectations.expect(copy.has_value(), "a second composition exists");
    if (!copy)
        return;
    const auto second = *copy;
    const auto warm = [&](const std::int64_t seconds) {
        static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(seconds)));
        return waitUntil([&] { return isReady(controller); });
    };
    // A switch requests time zero first, so each composition warms it too.
    const auto enter = [&](const document::CompositionId id) {
        return session.setComposition(id) && waitUntil([&] { return isReady(controller); });
    };
    expectations.expect(warm(0) && warm(1) && warm(5), "the first composition warms");
    expectations.expect(enter(second) && warm(1), "the second composition warms");
    const auto warmed = preparations.load();
    expectations.expect(enter(first) && preparations.load() == warmed && warm(1) && warm(5) &&
                            preparations.load() == warmed,
                        "switching back reuses the first composition's cached frames");

    // A whole-render edit confined to the second composition leaves the first one cached.
    expectations.expect(enter(second), "back to the second composition");
    const auto secondLayer = session.composition()->graph().layerOutputs().front();
    commands::Transaction add("Add source", session.snapshot().revision());
    add.emplace<commands::AddNode>(second, std::string(document::kSolidSourceNodeType),
                                   document::Vec2d{});
    const auto source = session.executeTransaction(std::move(add))
                            .outputId<document::NodeId>(commands::kAddNodeOutput);
    commands::Transaction rewire("Rewire", session.snapshot().revision());
    rewire.emplace<commands::ConnectPorts>(
        second, document::OutputPortRef{*source, "image"},
        document::InputPortRef{document::NodeInputRef{secondLayer.nodeId, "image"}});
    const auto generationBeforeRewire = generation(controller.state());
    const auto rewired = session.executeTransaction(std::move(rewire));
    expectations.expect(rewired.changed() && !rewired.affectedTimes.has_value() &&
                            rewired.affectedComposition == second,
                        "a reachable rewire is a whole render scoped to its composition");
    // The rewire re-renders the second composition in the background. Count its preparation
    // before the baseline, or it lands inside the measured window on a loaded machine.
    expectations.expect(waitUntil([&] {
                            return generation(controller.state()) > generationBeforeRewire &&
                                   isReady(controller);
                        }),
                        "the second composition finishes its whole render");
    const auto beforeReturn = preparations.load();
    expectations.expect(enter(first) && warm(1) && warm(5) && preparations.load() == beforeReturn,
                        "an unrelated composition's whole render keeps this one cached");

    // Once the first composition nests the second, an edit there invalidates it on return.
    const auto mergeId = session.composition()->graph().layerStack().nodeId();
    commands::Transaction nest("Nest", session.snapshot().revision());
    nest.emplace<commands::AddNode>(first, std::string(document::kCompositionSourceNodeType),
                                    document::Vec2d{});
    const auto nested = session.executeTransaction(std::move(nest))
                            .outputId<document::NodeId>(commands::kAddNodeOutput);
    commands::Transaction layer("Nest layer", session.snapshot().revision());
    layer.emplace<commands::AddNode>(first, std::string(document::kLayerOutputNodeType),
                                     document::Vec2d{});
    const auto nestLayer = session.executeTransaction(std::move(layer))
                               .outputId<document::NodeId>(commands::kAddNodeOutput);
    expectations.expect(nested.has_value() && nestLayer.has_value(), "the nesting nodes exist");
    if (!nested || !nestLayer)
        return;
    commands::Transaction wire("Wire nest", session.snapshot().revision());
    wire.emplace<commands::SetParameterSource>(
        first, layerParameter(session, *nested, "composition"),
        document::ConstantValueSource{static_cast<std::int64_t>(second.value())});
    wire.emplace<commands::ConnectPorts>(
        first, document::OutputPortRef{*nested, "image"},
        document::InputPortRef{document::NodeInputRef{*nestLayer, "image"}});
    wire.emplace<commands::ConnectPorts>(
        first, document::OutputPortRef{*nestLayer, "image"},
        document::InputPortRef{document::LayerStackInputRef{
            mergeId, document::LayerSlotId{}, std::string(document::kLayerStackContentInputRole)}});
    expectations.expect(session.executeTransaction(std::move(wire)).changed() && warm(0) && warm(1),
                        "the first composition now nests the second");
    expectations.expect(enter(second), "edit inside the nested composition");
    commands::Transaction inner("Inner opacity", session.snapshot().revision());
    inner.emplace<commands::SetParameterSource>(
        second, layerParameter(session, secondLayer.nodeId, document::kOpacityParameterRole),
        document::ConstantValueSource{0.25});
    expectations.expect(session.executeTransaction(std::move(inner)).changed(),
                        "the nested composition changes");
    const auto beforeNestedReturn = preparations.load();
    // Exact counts depend on scheduling; what matters is that the return was not served from the
    // dropped retention.
    expectations.expect(enter(first) && preparations.load() > beforeNestedReturn,
                        "a nesting composition re-derives after its nested composition changed");
    reachQuiescence(controller, bridge, scheduler, expectations);
}
