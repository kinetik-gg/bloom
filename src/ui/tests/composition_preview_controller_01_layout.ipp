// LAYOUT-2: a verified layout-only command updates the live document and the UI, but the retained
// evaluation snapshot -- and therefore the prepared frame, the frame-cache key and the compiled
// plan -- do not move. The three opt-out commands are covered, warm-cache take is covered, and an
// explicit refresh is shown to re-derive rather than serve the cache.
void testLayoutEditRetainsEvaluationWork(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Layout Retain Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "layout-retain fixture registers node definitions");
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
    const auto nodeId = firstGraphNode(session);
    expectations.expect(nodeId.isValid(), "the composition exposes a node to lay out");

    const auto nodeEdit = [&](const char* label, auto&& emplace) {
        commands::Transaction transaction(label, session.snapshot().revision());
        emplace(transaction);
        return session.executeNodeTransaction(std::move(transaction));
    };
    const auto moved = nodeEdit("Move Nodes", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::MoveNodes>(
            compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {5.0, 6.0}}});
    });
    expectations.expect(moved.changed() && !moved.renderAffecting, "MoveNodes is layout-only");
    const auto collapsed = nodeEdit("Collapse Node", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetNodeCollapsed>(compositionId, nodeId, true);
    });
    expectations.expect(collapsed.changed() && !collapsed.renderAffecting,
                        "SetNodeCollapsed is layout-only");
    const auto widened = nodeEdit("Widen Node", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::SetNodeWidth>(compositionId, nodeId, 260.0);
    });
    expectations.expect(widened.changed() && !widened.renderAffecting,
                        "SetNodeWidth is layout-only");

    expectations.expect(session.snapshot().revision() != evalRevision,
                        "the live document revision advanced across the layout edits");
    expectations.expect(session.evaluationSnapshot().revision() == evalRevision,
                        "the three layout edits retained the evaluation snapshot");
    expectations.expect(invocationCount.load() == invocationsBefore &&
                            controller.state().frame == frameBefore,
                        "layout-only edits invoked no preparation and kept the same frame");
    expectations.expect(planCache->statistics().compiles == compilesBefore,
                        "layout-only edits compiled no plan");
    const auto warmKey = controller.cacheKeyForTime(session.currentTime());
    expectations.expect(warmKey.has_value() && warmKey->sourceRevision == evalRevision &&
                            controller.frameCache().contains(*warmKey) &&
                            controller.frameCache().size() == cacheSizeBefore,
                        "the warmed frame is still reachable under the retained revision");

    const auto other = core::RationalTime::create(1, 2);
    expectations.expect(other.has_value() && session.setCurrentTime(*other) &&
                            waitUntil([&] { return isReady(controller); }),
                        "a second time reaches a ready frame");
    const auto afterSecond = invocationCount.load();
    const auto again = nodeEdit("Move Nodes Again", [&](commands::Transaction& transaction) {
        transaction.emplace<commands::MoveNodes>(
            compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {9.0, 2.0}}});
    });
    expectations.expect(again.changed(), "a second layout edit publishes");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "returning to the first time reaches a ready frame");
    expectations.expect(invocationCount.load() == afterSecond,
                        "the warmed frame is taken from the cache, not evaluated again");

    controller.requestRefresh();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the explicit refresh reaches ready");
    expectations.expect(invocationCount.load() > afterSecond,
                        "an explicit refresh re-derives the frame rather than serving the cache");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// LAYOUT-2: a pixel-affecting or mixed command advances the evaluation snapshot immediately, and
// undo/redo replay the stored impact. Rebind replaces it even when the numeric revision is equal.
void testPixelMixedAndRebindAdvanceEvaluation(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Evaluation Advance Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);

    expectations.expect(waitUntil([&] { return isReady(controller); }), "initial frame ready");
    const auto nodeId = firstGraphNode(session);
    const auto baseRevision = session.snapshot().revision();

    commands::Transaction mute("Mute Node", session.snapshot().revision());
    mute.emplace<commands::SetNodeMuted>(compositionId, nodeId, true);
    const auto muteResult = session.executeNodeTransaction(std::move(mute));
    expectations.expect(muteResult.changed() && muteResult.renderAffecting &&
                            session.evaluationSnapshot().revision() ==
                                session.snapshot().revision() &&
                            session.evaluationSnapshot().revision() != baseRevision,
                        "SetNodeMuted advances the evaluation snapshot");
    expectations.expect(waitUntil([&] { return isReady(controller); }), "pixel frame ready");

    expectations.expect(session.undo() && session.evaluationSnapshot().revision() ==
                                              session.snapshot().revision(),
                        "undo advances the evaluation snapshot");
    expectations.expect(session.redo() && session.evaluationSnapshot().revision() ==
                                              session.snapshot().revision(),
                        "redo advances the evaluation snapshot");

    commands::Transaction mixed("Move and unmute", session.snapshot().revision());
    mixed.emplace<commands::MoveNodes>(
        compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {7.0, 8.0}}});
    mixed.emplace<commands::SetNodeMuted>(compositionId, nodeId, false);
    const auto mixedResult = session.executeNodeTransaction(std::move(mixed));
    expectations.expect(mixedResult.changed() && mixedResult.renderAffecting &&
                            session.evaluationSnapshot().revision() ==
                                session.snapshot().revision(),
                        "a mixed layout+pixel command advances the evaluation snapshot");
    expectations.expect(session.undo() && session.evaluationSnapshot().revision() ==
                                              session.snapshot().revision(),
                        "undo of a mixed command advances the evaluation snapshot");
    expectations.expect(session.redo() && session.evaluationSnapshot().revision() ==
                                              session.snapshot().revision(),
                        "redo of a mixed command advances the evaluation snapshot");

    // Rebind: a new document with the same numeric revision must still replace the evaluation
    // snapshot, keyed by project identity rather than revision number.
    auto secondProject = makeTestProject("Evaluation Rebind Test");
    const auto secondCompositionId = secondProject.initialCompositionId;
    document::Document secondDocument(std::move(secondProject.project));
    commands::CommandStack secondCommands(secondDocument);
    expectations.expect(secondDocument.snapshot().revision() == baseRevision,
                        "the rebound document deliberately shares the initial numeric revision");
    session.rebind(secondDocument, secondCommands, secondCompositionId);
    expectations.expect(
        session.evaluationSnapshot().project().id() == secondDocument.snapshot().project().id() &&
            session.evaluationSnapshot().revision() == secondDocument.snapshot().revision(),
        "rebind adopts the new document's evaluation snapshot despite equal revisions");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// LAYOUT-2: a committed frame already in flight when a layout-only edit lands still publishes as
// Current, because its honest retained-evaluation identity is accepted by the live-session guard.
void testInFlightFrameSurvivesLayoutEdit(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("In Flight Layout Edit Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate gate;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            gate.enterAndWait();
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "the first frame is in flight on the worker");
    const auto evalRevision = session.evaluationSnapshot().revision();
    const auto nodeId = firstGraphNode(session);
    commands::Transaction move("Move Nodes", session.snapshot().revision());
    move.emplace<commands::MoveNodes>(
        compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {3.0, 4.0}}});
    expectations.expect(session.executeNodeTransaction(std::move(move)).changed(),
                        "the layout edit publishes while the frame is in flight");
    expectations.expect(session.evaluationSnapshot().revision() == evalRevision &&
                            session.snapshot().revision() != evalRevision,
                        "the in-flight frame's evaluation snapshot was retained");
    gate.release();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the in-flight frame still arrives as Current after the layout edit");
    expectations.expect(controller.state().frame != nullptr &&
                            controller.state().frame->desiredIdentity().sourceRevision ==
                                evalRevision &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "the committed frame keeps its real retained revision and is Current");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// LAYOUT-2: a delayed result from before a pixel edit can never become Current. The completed task
// is for an evaluation the session has left behind, so the controller re-requests instead of
// publishing it, and only the frame for the advanced evaluation reaches Ready.
void testInFlightFrameRejectedAfterPixelEdit(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("In Flight Pixel Edit Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate gate;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            gate.enterAndWait();
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "the first frame is in flight on the worker");
    const auto preEditRevision = session.snapshot().revision();
    std::vector<document::Revision> readyRevisions;
    QObject::connect(&controller, &ui::CompositionPreviewController::stateChanged, &controller,
                     [&] {
                         if (isReady(controller) && controller.state().frame != nullptr)
                             readyRevisions.push_back(
                                 controller.state().frame->desiredIdentity().sourceRevision);
                     });
    const auto nodeId = firstGraphNode(session);
    commands::Transaction mute("Mute Node", session.snapshot().revision());
    mute.emplace<commands::SetNodeMuted>(compositionId, nodeId, true);
    expectations.expect(session.executeNodeTransaction(std::move(mute)).changed() &&
                            session.evaluationSnapshot().revision() ==
                                session.snapshot().revision(),
                        "the pixel edit advances the evaluation snapshot immediately");
    gate.release();
    expectations.expect(waitUntil([&] {
                            return isReady(controller) && controller.state().frame != nullptr &&
                                   controller.state().frame->desiredIdentity().sourceRevision ==
                                       session.snapshot().revision();
                        }),
                        "only a frame for the advanced evaluation reaches Ready");
    for (const auto revision : readyRevisions) {
        expectations.expect(revision != preEditRevision,
                            "a delayed pre-pixel result never publishes as Ready");
    }

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// LAYOUT-2 follow-up: a viewer-analysis request derived from the retained displayed frame must
// carry the RETAINED snapshot, not the live one -- makeCompositionPreviewPipeline rejects a request
// whose identity and snapshot revisions disagree. After a layout edit the analysis still prepares a
// real frame with its ROI/view-adjust identity intact; after a pixel edit that retained identity is
// refused, and an active override frozen against live cannot ride a retained-frame request.
void testViewerAnalysisUsesRetainedProvenance(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Analysis Provenance Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the analysis fixture has content and a selected layer");
    expectations.expect(waitUntil([&] { return isReady(controller); }), "initial frame ready");

    const auto makeIdentity = [&](const std::uint64_t generation) {
        auto identity = controller.state().frame->desiredIdentity();
        identity.requestGeneration = generation;
        identity.viewAdjust = runtime::ViewAdjust{0.5, 1.2};
        if (const auto roi = render::ImageWindow::create(0, 0, 1, 1))
            identity.roi = *roi.value();
        return identity;
    };
    const auto nodeId = firstGraphNode(session);
    commands::Transaction move("Move Nodes", session.snapshot().revision());
    move.emplace<commands::MoveNodes>(
        compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {6.0, 7.0}}});
    expectations.expect(session.executeNodeTransaction(std::move(move)).changed() &&
                            session.evaluationSnapshot().revision() !=
                                session.snapshot().revision(),
                        "the layout edit retains the evaluation snapshot");

    const auto retainedIdentity = makeIdentity(1001);
    const auto submission = controller.submitViewerAnalysis(retainedIdentity);
    expectations.expect(submission.accepted(),
                        "a retained-frame analysis request is accepted after a layout edit");
    std::optional<runtime::TaskResult<ui::PreviewPreparationResultHandle>> analysisResult;
    expectations.expect(waitUntil([&] {
                            analysisResult = submission.handle.tryTakeResult();
                            return analysisResult.has_value();
                        }),
                        "the analysis request reaches a terminal result");
    expectations.expect(
        analysisResult.has_value() && analysisResult->state() == runtime::TaskState::Succeeded &&
            analysisResult->value() && *analysisResult->value() &&
            (*analysisResult->value())->status() == runtime::PreviewPreparationStatus::Prepared &&
            (*analysisResult->value())->frame() != nullptr &&
            (*analysisResult->value())->frame()->desiredIdentity() == retainedIdentity,
        "the retained provenance prepares a real frame with its ROI/view-adjust identity intact");

    commands::Transaction mute("Mute Node", session.snapshot().revision());
    mute.emplace<commands::SetNodeMuted>(compositionId, nodeId, true);
    expectations.expect(session.executeNodeTransaction(std::move(mute)).changed() &&
                            session.evaluationSnapshot().revision() ==
                                session.snapshot().revision(),
                        "the pixel edit advances the evaluation snapshot");
    expectations.expect(!controller.submitViewerAnalysis(retainedIdentity).accepted(),
                        "a stale retained identity is refused after a pixel edit");
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the post-pixel frame becomes ready before the next layout edit");

    commands::Transaction moveAgain("Move Nodes", session.snapshot().revision());
    moveAgain.emplace<commands::MoveNodes>(
        compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {8.0, 9.0}}});
    expectations.expect(session.executeNodeTransaction(std::move(moveAgain)).changed() &&
                            session.evaluationSnapshot().revision() !=
                                session.snapshot().revision(),
                        "a second layout edit retains the evaluation snapshot again");
    const auto retainedAfterSecond = makeIdentity(1002);
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    expectations.expect(position != nullptr, "the selected layer exposes a position parameter");
    if (position != nullptr) {
        expectations.expect(session.beginValueEdit(position->id), "a value edit is active");
        expectations.expect(!controller.submitViewerAnalysis(retainedAfterSecond).accepted(),
                            "an active live override cannot ride a retained-frame request");
        session.cancelValueEdit();
    }

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// LAYOUT-2 follow-up: the new-document gate keyed by identity, not by numeric revision. Every
// makeNewProject document carries ProjectId 1 and CompositionId 1, so two documents with different
// pixels can collide in project/composition/revision. Warming the first document's frame and plan
// and rebinding to the second must display the second document's pixels. NOTE: this is a bounded
// regression for a PRE-EXISTING identity collision; the CompiledPlanCache keys only
// project/composition/revision and has no reset hook, so this may currently fail. It is left
// failing as evidence, and the provenance correction is a separate package.
void testRebindWithCollidingIdentitiesRendersNewPixels(Expectations& expectations) {
    using namespace bloom;
    auto firstProject = makeTestProject("Rebind Collision First");
    const auto compositionId = firstProject.initialCompositionId;
    document::Document firstDocument(std::move(firstProject.project));
    commands::CommandStack firstCommands(firstDocument);
    commands::Transaction firstAdd("Add Solid", firstDocument.snapshot().revision());
    firstAdd.emplace<commands::AddSolidLayer>(compositionId, std::string("Solid"),
                                              core::Color4d{1.0, 0.0, 0.0, 1.0});
    expectations.expect(firstCommands.execute(std::move(firstAdd)).changed(),
                        "the first document gets a red solid");

    auto secondProject = makeTestProject("Rebind Collision Second");
    const auto secondCompositionId = secondProject.initialCompositionId;
    document::Document secondDocument(std::move(secondProject.project));
    commands::CommandStack secondCommands(secondDocument);
    commands::Transaction secondAdd("Add Solid", secondDocument.snapshot().revision());
    secondAdd.emplace<commands::AddSolidLayer>(secondCompositionId, std::string("Solid"),
                                               core::Color4d{0.0, 0.0, 1.0, 1.0});
    expectations.expect(secondCommands.execute(std::move(secondAdd)).changed(),
                        "the second document gets a blue solid");
    expectations.expect(
        firstDocument.snapshot().revision() == secondDocument.snapshot().revision() &&
            firstDocument.snapshot().project().id() == secondDocument.snapshot().project().id() &&
            compositionId == secondCompositionId,
        "the two documents deliberately collide in project/composition/revision");

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionSession session(firstDocument, firstCommands, compositionId);
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);
    expectations.expect(waitUntil([&] { return isReady(controller); }), "first frame ready");
    const auto firstFrame = controller.state().frame;
    expectations.expect(firstFrame != nullptr, "the first document produced a frame");
    if (firstFrame == nullptr) {
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const auto firstBuffer = firstFrame->displayBufferView();
    expectations.expect(firstBuffer.has_value() && !firstBuffer->pixels.empty(),
                        "the first frame has a display buffer");
    if (!firstBuffer.has_value() || firstBuffer->pixels.empty()) {
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const auto firstCentre = firstBuffer->pixels[firstBuffer->pixels.size() / 2];
    expectations.expect(firstCentre.red > firstCentre.blue,
                        "the first document's pixels are the red solid");
    const auto firstPlan = firstFrame->processIdentity().plan;

    session.rebind(secondDocument, secondCommands, secondCompositionId);
    expectations.expect(waitUntil([&] {
                            return isReady(controller) && controller.state().frame != nullptr &&
                                   controller.state().frame != firstFrame;
                        }),
                        "a freshly prepared frame for the rebound document reaches the viewer");
    const auto secondFrame = controller.state().frame;
    expectations.expect(secondFrame->processIdentity().plan != firstPlan,
                        "the rebound document must not reuse the previous document's compiled "
                        "plan (colliding project/composition/revision key)");
    const auto secondBuffer = secondFrame->displayBufferView();
    expectations.expect(secondBuffer.has_value() && !secondBuffer->pixels.empty(),
                        "the rebound frame has a display buffer");
    if (secondBuffer.has_value() && !secondBuffer->pixels.empty()) {
        const auto secondCentre = secondBuffer->pixels[secondBuffer->pixels.size() / 2];
        expectations.expect(secondCentre.blue > secondCentre.red,
                            "the rebound document displays its own blue pixels, not the previous "
                            "document's red ones");
    }

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// PROVENANCE-1: a rebind while the previous document's frame is still in flight must not let that
// completion publish or cache old pixels after it is released. The rebind notification detaches the
// old handle and clears the frame cache before the new request is built, so only the new document's
// frame can become Current.
void testRebindInFlightFrameCannotPublishOldPixels(Expectations& expectations) {
    using namespace bloom;
    auto firstProject = makeTestProject("Rebind In Flight First");
    const auto compositionId = firstProject.initialCompositionId;
    document::Document firstDocument(std::move(firstProject.project));
    commands::CommandStack firstCommands(firstDocument);
    commands::Transaction firstAdd("Add Solid", firstDocument.snapshot().revision());
    firstAdd.emplace<commands::AddSolidLayer>(compositionId, std::string("Solid"),
                                              core::Color4d{1.0, 0.0, 0.0, 1.0});
    expectations.expect(firstCommands.execute(std::move(firstAdd)).changed(),
                        "the in-flight fixture gets a red solid");
    auto secondProject = makeTestProject("Rebind In Flight Second");
    const auto secondCompositionId = secondProject.initialCompositionId;
    document::Document secondDocument(std::move(secondProject.project));
    commands::CommandStack secondCommands(secondDocument);
    commands::Transaction secondAdd("Add Solid", secondDocument.snapshot().revision());
    secondAdd.emplace<commands::AddSolidLayer>(secondCompositionId, std::string("Solid"),
                                               core::Color4d{0.0, 0.0, 1.0, 1.0});
    expectations.expect(secondCommands.execute(std::move(secondAdd)).changed(),
                        "the in-flight fixture gets a blue solid");

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate gate;
    std::atomic<bool> blockFirst{true};
    ui::CompositionSession session(firstDocument, firstCommands, compositionId);
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate, &blockFirst, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (blockFirst.exchange(false))
                gate.enterAndWait();
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });
    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "the first document's frame is in flight");
    session.rebind(secondDocument, secondCommands, secondCompositionId);
    gate.release();
    expectations.expect(
        waitUntil([&] { return isReady(controller) && controller.state().frame != nullptr; }),
        "the rebound document's frame becomes ready");
    const auto buffer = controller.state().frame->displayBufferView();
    expectations.expect(buffer.has_value() && !buffer->pixels.empty(),
                        "the rebound frame has a display buffer");
    if (buffer.has_value() && !buffer->pixels.empty()) {
        const auto centre = buffer->pixels[buffer->pixels.size() / 2];
        expectations.expect(centre.blue > centre.red,
                            "the released old in-flight frame never publishes its red pixels");
    }
    const auto key = controller.cacheKeyForTime(session.currentTime());
    expectations.expect(key.has_value() &&
                            key->sourceRevision == session.evaluationSnapshot().revision() &&
                            controller.frameCache().contains(*key),
                        "the rebound frame is cached under the new document's own snapshot");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// WORKAREA-1: a range edit is render-neutral. It prunes out-of-range cache entries and bounds
// later insertions, but it neither refreshes the displayed frame nor advances evaluation, and a
// frame outside the range can still be displayed. A late in-flight completion after a trim cannot
// resurrect an out-of-range entry.
void testWorkAreaEditDoesNotRefreshOrResurrect(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Work Area Foreground Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate gate;
    std::atomic<int> invocationCount = 0;
    std::atomic<bool> blockNext{false};
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate, &invocationCount, &blockNext, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            ++invocationCount;
            if (blockNext.exchange(false))
                gate.enterAndWait();
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the work-area foreground fixture has content");
    expectations.expect(waitUntil([&] { return isReady(controller); }), "initial frame ready");
    const auto evalRevision = session.evaluationSnapshot().revision();
    const auto frameBefore = controller.state().frame;

    // Set a range that excludes the current time (0). No refresh and no evaluation advance.
    const auto invocationsBefore = invocationCount.load();
    commands::Transaction range("Set Work Area", session.snapshot().revision());
    const auto rangeStart = core::RationalTime::create(2, 25);
    const auto rangeEnd = core::RationalTime::create(5, 25);
    if (!rangeStart.has_value() || !rangeEnd.has_value()) {
        expectations.expect(false, "the work-area range fixture times are valid");
        return;
    }
    range.emplace<commands::SetWorkArea>(compositionId, *rangeStart, *rangeEnd);
    expectations.expect(session.executeTransaction(std::move(range)).changed(),
                        "the range edit publishes");
    expectations.expect(invocationCount.load() == invocationsBefore &&
                            session.evaluationSnapshot().revision() == evalRevision &&
                            controller.state().frame == frameBefore &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "a range edit does not refresh, re-evaluate, or stale the display");
    const auto time0Key = controller.cacheKeyForTime(session.currentTime());
    expectations.expect(time0Key && !controller.frameCache().contains(*time0Key),
                        "the out-of-range frame was pruned from the cache");
    expectations.expect(controller.state().frame != nullptr,
                        "a frame outside the work area can still be displayed");

    // A late in-flight completion for the out-of-range time cannot reinsert it.
    blockNext.store(true);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "the forced refresh is in flight");
    const auto rangeDropsBefore = controller.frameCache().statistics().rangeDrops;
    gate.release();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the in-flight frame completes");
    expectations.expect(time0Key && !controller.frameCache().contains(*time0Key) &&
                            controller.frameCache().statistics().rangeDrops > rangeDropsBefore,
                        "a late completion cannot resurrect an out-of-range cache entry");
    expectations.expect(controller.state().frame != nullptr &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "the late completion still displays even though it is not retained");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// TEMPORAL-2A. An operation that carries a chosen changed-time footprint without touching the
// document, so session provenance can be driven directly and deterministically.
class SessionFootprintOp final : public bloom::commands::Operation {
  public:
    explicit SessionFootprintOp(bloom::commands::AffectedTimeFootprint footprint)
        : footprint_(std::move(footprint)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.session-footprint";
    }
    [[nodiscard]] bloom::commands::OperationResult apply(bloom::document::Draft&) const override {
        auto result = bloom::commands::OperationResult::applied();
        result.affectedTimes = footprint_;
        return result;
    }

  private:
    bloom::commands::AffectedTimeFootprint footprint_;
};

class ForeignFootprintOp final : public bloom::commands::Operation {
  public:
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.foreign-footprint";
    }
    [[nodiscard]] bloom::commands::OperationResult apply(bloom::document::Draft&) const override {
        auto result = bloom::commands::OperationResult::applied();
        result.affectedTimes = bloom::commands::AffectedTimeFootprint{
            .compositionId = bloom::document::CompositionId::fromRaw(999),
            .intervals = {{bloom::core::RationalTime::fromInteger(1),
                           bloom::core::RationalTime::fromInteger(2)}}};
        return result;
    }
};
