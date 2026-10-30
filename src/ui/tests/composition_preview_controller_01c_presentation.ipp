// #399: presentation edits the operations never declared neutral -- a layer rename (the Layer
// Output card's display name), a label color, a lock, a composition name, a group frame and its
// name -- are proven render-neutral by the command stack from the document's field classification,
// so a real session+controller keeps the retained evaluation snapshot, compiles nothing, prepares
// nothing, and the next frame request is a frame-cache hit. Selection is session state and never
// reaches the document. Undo and redo of a presentation edit stay neutral, and an evaluation edit
// (enable, a parameter) still re-prepares the frame.
void testPresentationEditsRetainEvaluationWork(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Presentation Retain Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "presentation fixture registers node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto planCache = std::make_shared<runtime::CompiledPlanCache>();
    auto pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                       qualifiedProvider, planCache);
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&invocationCount,
         pipeline](const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& desiredIdentity,
                   const std::size_t pixelStorageByteLimit,
                   const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
                   runtime::TaskContext& context) mutable {
            ++invocationCount;
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the presentation fixture has a layer");
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the layer's frame is ready and warm in the cache");
    const auto layers = session.composition()->graph().layerOutputs();
    expectations.expect(!layers.empty(), "the fixture exposes a layer boundary");
    if (layers.empty()) {
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const auto layerId = layers.front().layerId;
    const auto layerNode = layers.front().nodeId;
    const auto evalRevision = session.evaluationSnapshot().revision();
    const auto frameBefore = controller.state().frame;
    const auto invocationsBefore = invocationCount.load();
    const auto compilesBefore = planCache->statistics().compiles;
    const auto cacheSizeBefore = controller.frameCache().size();
    const auto retained = [&](const std::string_view what) {
        expectations.expect(session.evaluationSnapshot().revision() == evalRevision, what);
        expectations.expect(invocationCount.load() == invocationsBefore &&
                                controller.state().frame == frameBefore &&
                                planCache->statistics().compiles == compilesBefore,
                            "a presentation edit prepares nothing and compiles nothing");
    };
    const auto edit = [&](const char* label, auto&& emplace) {
        commands::Transaction transaction(label, session.snapshot().revision());
        emplace(transaction);
        const auto result = session.executeTransaction(std::move(transaction));
        expectations.expect(result.changed() && !result.renderAffecting,
                            "a presentation edit publishes a render-neutral result");
        return result;
    };

    session.selectNode(layerNode);
    session.selectLayer(layerId);
    expectations.expect(session.snapshot().revision() == evalRevision,
                        "selection is session state and never advances the document");
    retained("selection keeps the evaluation snapshot");

    edit("Rename Layer", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::RenameLayer>(compositionId, layerId, "Hero");
    });
    retained("a layer rename keeps the evaluation snapshot");
    edit("Label Color", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetLayerLabelColor>(compositionId, layerId,
                                                          std::array<std::uint8_t, 3>{9, 99, 199});
    });
    retained("a label color keeps the evaluation snapshot");
    edit("Lock Layer", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetLayerLocked>(compositionId, layerId, true);
    });
    edit("Unlock Layer", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetLayerLocked>(compositionId, layerId, false);
    });
    retained("lock and unlock keep the evaluation snapshot");
    edit("Rename Composition", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetCompositionName>(compositionId, "Renamed");
    });
    retained("a composition rename keeps the evaluation snapshot");
    const auto grouped = edit("Group Nodes", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::GroupNodes>(compositionId, std::set{layerNode});
    });
    if (const auto groupId = grouped.outputId<document::NodeGroupId>(commands::kGroupNodesOutput)) {
        edit("Rename Group", [&](commands::Transaction& transaction) {
            transaction.emplace<commands::RenameGroup>(compositionId, *groupId, "Frame");
        });
    }
    edit("Move Group", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::MoveNodes>(
            compositionId, std::map<document::NodeId, document::Vec2d>{{layerNode, {40.0, 50.0}}});
    });
    edit("Collapse Node", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetNodeCollapsed>(compositionId, layerNode, true);
    });
    retained("group framing, naming, moving and collapse keep the evaluation snapshot");

    expectations.expect(session.undo() && session.undo() && session.redo(),
                        "presentation edits undo and redo");
    retained("undo and redo of presentation edits keep the evaluation snapshot");

    const auto warmKey = controller.cacheKeyForTime(session.currentTime());
    expectations.expect(warmKey.has_value() && warmKey->sourceRevision == evalRevision &&
                            controller.frameCache().contains(*warmKey) &&
                            controller.frameCache().size() == cacheSizeBefore,
                        "the warm frame stays reachable under the retained revision");
    const auto hitsBefore = controller.frameCache().statistics().hits;
    const auto other = core::RationalTime::create(1, 2);
    expectations.expect(other.has_value() && session.setCurrentTime(*other) &&
                            waitUntil([&] { return isReady(controller); }),
                        "a second time reaches a ready frame");
    const auto afterSecond = invocationCount.load();
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "returning to the warm time reaches a ready frame");
    expectations.expect(invocationCount.load() == afterSecond &&
                            controller.frameCache().statistics().hits > hitsBefore,
                        "the next request for the warm time is a frame-cache hit");

    // Evaluation edits still invalidate.
    const auto pixelEdit = [&](const char* label, auto&& emplace) {
        const auto invocations = invocationCount.load();
        commands::Transaction transaction(label, session.snapshot().revision());
        emplace(transaction);
        const auto result = session.executeTransaction(std::move(transaction));
        expectations.expect(result.changed() && result.renderAffecting &&
                                session.evaluationSnapshot().revision() ==
                                    session.snapshot().revision(),
                            "an evaluation edit advances the evaluation snapshot");
        expectations.expect(
            waitUntil([&] { return isReady(controller) && invocationCount.load() > invocations; }),
            "an evaluation edit re-prepares the frame");
    };
    pixelEdit("Disable Layer", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetLayerEnabled>(compositionId, layerId, false);
    });
    pixelEdit("Enable Layer", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetLayerEnabled>(compositionId, layerId, true);
    });
    std::optional<document::ParameterId> opacity;
    if (const auto* node = session.composition()->graph().findNode(layerNode))
        for (const auto& binding : node->parameters)
            if (binding.role == document::kOpacityParameterRole)
                opacity = binding.parameterId;
    expectations.expect(opacity.has_value(), "the layer binds an opacity parameter");
    if (opacity.has_value())
        pixelEdit("Fade Layer", [&](commands::Transaction& transaction) {
            transaction.emplace<commands::SetParameterSource>(
                compositionId, *opacity,
                document::ParameterSource{document::ConstantValueSource{0.5}});
        });

    reachQuiescence(controller, bridge, scheduler, expectations);
}
