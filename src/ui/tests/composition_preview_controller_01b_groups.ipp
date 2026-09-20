// LAYOUT-2 group-command retention: grouping only changes node-group presentation state, so a
// real session+controller keeps the retained evaluation revision and the warm frame across
// GroupNodes/UngroupNodes and their undo/redo. Extracted from the layout .ipp to keep each file
// within the cohesive size budget; it shares the same fixture and helpers via the including TU.

// LAYOUT-2, group commands: grouping only changes node-group presentation state, so a real
// session+controller keeps the retained evaluation revision and the warm frame across
// GroupNodes/UngroupNodes and their undo/redo. This is the F2 classification proven end to end.
void testGroupCommandsRetainEvaluationWork(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Group Retain Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "group-retain fixture registers node definitions");
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
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the initial frame is ready and warm in the cache");
    const auto evalRevision = session.evaluationSnapshot().revision();
    const auto frameBefore = controller.state().frame;
    const auto invocationsBefore = invocationCount.load();
    const auto compilesBefore = planCache->statistics().compiles;
    const auto cacheSizeBefore = controller.frameCache().size();

    const auto nodes = session.composition()->graph().nodes();
    expectations.expect(nodes.size() >= 2, "the composition holds two nodes to group");
    if (nodes.size() < 2) {
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const std::set<document::NodeId> members{nodes[0].id, nodes[1].id};

    commands::Transaction group("Group Nodes", session.snapshot().revision());
    group.emplace<commands::GroupNodes>(compositionId, members);
    const auto grouped = session.executeNodeTransaction(std::move(group));
    expectations.expect(grouped.changed() && !grouped.renderAffecting,
                        "GroupNodes is presentation-only for the controller");
    const auto groupId = grouped.outputId<document::NodeGroupId>(commands::kGroupNodesOutput);
    expectations.expect(groupId.has_value(), "GroupNodes reports the created group id");

    expectations.expect(session.snapshot().revision() != evalRevision,
                        "the live document revision advanced across grouping");
    expectations.expect(session.evaluationSnapshot().revision() == evalRevision,
                        "GroupNodes retained the evaluation snapshot");
    expectations.expect(invocationCount.load() == invocationsBefore &&
                            controller.state().frame == frameBefore,
                        "grouping invoked no preparation and kept the same frame");
    expectations.expect(planCache->statistics().compiles == compilesBefore,
                        "grouping compiled no plan");
    const auto warmKey = controller.cacheKeyForTime(session.currentTime());
    expectations.expect(warmKey.has_value() && warmKey->sourceRevision == evalRevision &&
                            controller.frameCache().contains(*warmKey) &&
                            controller.frameCache().size() == cacheSizeBefore,
                        "the warmed frame stays reachable after grouping");

    expectations.expect(
        session.undo() && session.evaluationSnapshot().revision() == evalRevision &&
            session.snapshot().revision() != evalRevision,
        "undo of GroupNodes keeps the evaluation snapshot and reverts the document");
    expectations.expect(session.redo() && session.evaluationSnapshot().revision() == evalRevision,
                        "redo of GroupNodes keeps the evaluation snapshot");
    expectations.expect(invocationCount.load() == invocationsBefore,
                        "group undo/redo invoked no preparation");

    if (groupId.has_value()) {
        commands::Transaction ungroup("Ungroup Nodes", session.snapshot().revision());
        ungroup.emplace<commands::UngroupNodes>(compositionId, *groupId);
        const auto ungrouped = session.executeNodeTransaction(std::move(ungroup));
        expectations.expect(ungrouped.changed() && !ungrouped.renderAffecting,
                            "UngroupNodes is presentation-only for the controller");
        expectations.expect(session.evaluationSnapshot().revision() == evalRevision,
                            "UngroupNodes retained the evaluation snapshot");
        expectations.expect(invocationCount.load() == invocationsBefore &&
                                controller.state().frame == frameBefore,
                            "ungrouping invoked no preparation and kept the same frame");
    }

    reachQuiescence(controller, bridge, scheduler, expectations);
}
