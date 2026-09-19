void testDocumentEditRebuildsChangedTargetWhileDisplayedUnchanged(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Target Repair");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate gate;
    std::atomic<bool> blockNext{false};
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate, &blockNext, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (blockNext.exchange(false))
                gate.enterAndWait();
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the repair fixture has a solid layer");
    const auto layerId = session.composition()->graph().layerOutputs().front().layerId;
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "the unaffected time 1 is displayed");
    const auto displayed = controller.state().frame;
    // Gate the t5 request: it is a miss, t5 was never warmed.
    blockNext.store(true);
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return gate.entered(); }),
                        "the t5 request is in flight while t1 is still displayed");
    expectations.expect(controller.state().frame == displayed,
                        "the last-good frame is still the unaffected t1");
    // The edit changes t5 but not t1.
    commands::Transaction trim("Trim", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, layerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(4));
    expectations.expect(session.executeTransaction(std::move(trim)).changed(),
                        "the trim changes t5 but not t1");
    gate.release();
    expectations.expect(waitUntil([&] {
                            return isReady(controller) && controller.state().frame != nullptr &&
                                   controller.state().frame->desiredIdentity().time ==
                                       core::RationalTime::fromInteger(5);
                        }),
                        "the viewer reaches Ready at the actual target t5");
    const auto frame = controller.state().frame;
    expectations.expect(frame->desiredIdentity().sourceRevision == session.snapshot().revision(),
                        "the rebuilt target carries live provenance, not the stale retained one");
    const runtime::PreviewRequestIdentity identity{
        .projectId = session.snapshot().project().id(),
        .compositionId = compositionId,
        .sourceRevision = session.snapshot().revision(),
        .requestGeneration = 1,
        .time = core::RationalTime::fromInteger(5),
        .output = runtime::PreviewOutput::Composition,
        .resolution = controller.resolution(),
        .quality = controller.settings().quality,
        .colorIntent = controller.settings().colorIntent,
        .resolutionPolicy = controller.settings().resolutionPolicy,
        .displayName = controller.settings().displayName,
        .viewName = controller.settings().viewName,
        .showLook = controller.settings().showLook,
    };
    const auto oracle =
        prepareLiveOracle(scheduler, fixture.pipeline, session.snapshot(), identity);
    expectations.expect(sameDisplayPixels(frame, oracle),
                        "the rebuilt target matches the live oracle (no stale frame)");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// SPLIT-2: an output-equivalent split keeps BOTH halves' cached pixels (zero preparations) while
// the retained frame's exposed geometry is translated to the current live layer/node IDs, and the
// translated geometry matches a fresh LIVE oracle.
void testSplitReusesBothHalvesWithCurrentLayerMetadata(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Split Reuse");
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
        session.addSolidLayer(QStringLiteral("Background"), core::Color4d{0.1, 0.2, 0.3, 1.0}),
        "the split fixture has a background");
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Foreground"), core::Color4d{0.9, 0.4, 0.1, 1.0}),
        "the split fixture has a foreground");
    const auto boundary = session.composition()->graph().layerOutputs().back();
    expectations.expect(session.currentTime() == core::RationalTime{},
                        "the fixture starts at time 0");
    expectations.expect(session.toggleKeyframe("position"), "key the foreground at time 0");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(8)) &&
                            session.setSelectedPosition(30.0, 20.0),
                        "key the foreground at time 8");
    const auto warm = [&](const std::int64_t second) {
        return session.setCurrentTime(core::RationalTime::fromInteger(second)) &&
               waitUntil([&] { return isReady(controller); });
    };
    expectations.expect(warm(2) && warm(5) && warm(2), "times 2 and 5 warm");
    const auto warmed = preparations.load();
    const auto retained = controller.state().frame;
    const auto retainedRevision = retained->desiredIdentity().sourceRevision;
    const auto retainedProject = retained->desiredIdentity().projectId;
    const auto cachedBytes = controller.frameCache().residentBytes();
    const auto cachedSize = controller.frameCache().size();

    // Count stateChanged emissions during the split: a metadata-only split must publish geometry so
    // viewer paint/selection re-reads currentLayerBounds, without any pixel preparation.
    int stateChangedCount = 0;
    QObject::connect(&controller, &ui::CompositionPreviewController::stateChanged, &controller,
                     [&stateChangedCount] { ++stateChangedCount; });

    // Split at t=4: [0,4) head, [4,8+ ) tail.
    commands::Transaction split("Split", session.snapshot().revision());
    split.emplace<commands::SplitLayerAtTime>(compositionId, boundary.layerId,
                                              core::RationalTime::fromInteger(4));
    const auto splitResult = session.executeTransaction(std::move(split));
    const auto tail = splitResult.outputId<document::LayerId>("layer");
    expectations.expect(splitResult.changed() && splitResult.affectedTimes.has_value() &&
                            splitResult.affectedTimes->intervals.empty() &&
                            splitResult.layerIdentityRemaps.has_value() && tail.has_value(),
                        "the split publishes empty pixel footprint plus remaps");
    if (!tail.has_value()) {
        return;
    }
    expectations.expect(preparations.load() == warmed && controller.state().frame == retained,
                        "the displayed frame survives the split with no preparation");
    expectations.expect(stateChangedCount > 0,
                        "a metadata-only split still publishes state for geometry consumers");
    expectations.expect(controller.frameCache().residentBytes() == cachedBytes &&
                            controller.frameCache().size() == cachedSize,
                        "no cache payload was copied or dropped");
    expectations.expect(controller.state().frame.get() == retained.get(),
                        "the retained frame payload object is the very same allocation");
    expectations.expect(retained->desiredIdentity().sourceRevision == retainedRevision &&
                            retainedRevision != session.snapshot().revision(),
                        "the retained frame keeps its original genuine provenance");
    // The retained time 2 is in the HEAD span, so its mapping stays the head; time 5's segment maps
    // the original layer to the tail. The retained frame's provenance gates the mapping.
    expectations.expect(session.currentLayerForRetained(retainedRevision, retainedProject,
                                                        core::RationalTime::fromInteger(2),
                                                        boundary.layerId) == boundary.layerId &&
                            session.currentLayerForRetained(retainedRevision, retainedProject,
                                                            core::RationalTime::fromInteger(5),
                                                            boundary.layerId) == *tail &&
                            session.currentLayerForRetained(session.snapshot().revision(),
                                                            session.snapshot().project().id(),
                                                            core::RationalTime::fromInteger(5),
                                                            boundary.layerId) == boundary.layerId,
                        "only the retained tail span maps; a live-provenance query passes through");

    const auto oracleAt = [&](const core::RationalTime time) {
        const runtime::PreviewRequestIdentity identity{
            .projectId = session.snapshot().project().id(),
            .compositionId = compositionId,
            .sourceRevision = session.snapshot().revision(),
            .requestGeneration = 1,
            .time = time,
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
        return prepareLiveOracle(scheduler, fixture.pipeline, session.snapshot(), identity);
    };
    // Both halves: the displayed time 2 still matches the live oracle and exposes geometry that
    // EQUALS the live oracle's head-layer bounds (not merely an existing ID).
    const auto oracleTwo = oracleAt(core::RationalTime::fromInteger(2));
    expectations.expect(oracleTwo != nullptr && sameDisplayPixels(retained, oracleTwo),
                        "the retained head-time frame matches the live oracle");
    const auto currentBounds = controller.currentLayerBounds();
    const auto headBound = std::ranges::find(currentBounds, boundary.layerId,
                                             &runtime::EvaluatedOperationBounds::layerId);
    const auto* oracleTwoBound = boundForLayer(oracleTwo, boundary.layerId);
    expectations.expect(headBound != currentBounds.end() && oracleTwoBound != nullptr &&
                            *headBound == *oracleTwoBound,
                        "the retained frame's geometry equals the live oracle's head bounds");

    // The tail half is served from cache (no preparation) with geometry translated to the new tail.
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "the tail time reaches ready");
    expectations.expect(preparations.load() == warmed,
                        "the tail half is served from cache with no preparation");
    const auto oracleFive = oracleAt(core::RationalTime::fromInteger(5));
    expectations.expect(sameDisplayPixels(controller.state().frame, oracleFive),
                        "the retained tail-time frame matches the live oracle");
    const auto tailBounds = controller.currentLayerBounds();
    const auto tailBound =
        std::ranges::find(tailBounds, *tail, &runtime::EvaluatedOperationBounds::layerId);
    const auto* oracleFiveTail = boundForLayer(oracleFive, *tail);
    expectations.expect(tailBound != tailBounds.end() && oracleFiveTail != nullptr &&
                            *tailBound == *oracleFiveTail,
                        "the retained tail frame's geometry equals the live oracle's tail bounds");

    // Undo removes the split; the head identity is restored for the retained provenance, and the
    // retained tail-time frame's geometry targets the head again.
    expectations.expect(session.undo(), "the split undoes");
    expectations.expect(session.currentLayerForRetained(retainedRevision, retainedProject,
                                                        core::RationalTime::fromInteger(5),
                                                        boundary.layerId) == boundary.layerId,
                        "undo clears the tail mapping for the retained provenance");
    const auto afterUndoBounds = controller.currentLayerBounds();
    expectations.expect(std::ranges::find(afterUndoBounds, boundary.layerId,
                                          &runtime::EvaluatedOperationBounds::layerId) !=
                            afterUndoBounds.end(),
                        "the retained frame's geometry targets the head again after undo");

    // Redo restores the split mapping; a repeated split of the tail composes original->final rather
    // than accumulating aliases, and undo/redo of the repeated split leaves no stale mapping.
    expectations.expect(session.redo(), "the split redoes");
    const auto tailAfterRedo = session.currentLayerForRetained(
        retainedRevision, retainedProject, core::RationalTime::fromInteger(5), boundary.layerId);
    expectations.expect(tailAfterRedo == *tail, "redo restores the tail mapping");
    commands::Transaction splitTail("Split tail", session.snapshot().revision());
    splitTail.emplace<commands::SplitLayerAtTime>(compositionId, *tail,
                                                  core::RationalTime::fromInteger(6));
    const auto splitTailResult = session.executeTransaction(std::move(splitTail));
    const auto tailTail = splitTailResult.outputId<document::LayerId>("layer");
    expectations.expect(splitTailResult.changed() && tailTail.has_value(), "the tail splits again");
    if (tailTail.has_value()) {
        // The second split at t=6 remaps [6,10) tail->tailTail; [4,6) still maps original->tail.
        expectations.expect(
            session.currentLayerForRetained(retainedRevision, retainedProject,
                                            core::RationalTime::fromInteger(7),
                                            boundary.layerId) == *tailTail &&
                session.currentLayerForRetained(retainedRevision, retainedProject,
                                                core::RationalTime::fromInteger(5),
                                                boundary.layerId) == *tail,
            "a repeated tail split composes original->final in one hop over the new span");
        expectations.expect(session.undo(), "the repeated split undoes");
        expectations.expect(session.currentLayerForRetained(retainedRevision, retainedProject,
                                                            core::RationalTime::fromInteger(7),
                                                            boundary.layerId) == *tail,
                            "undo of the repeated split restores the single-hop tail mapping");
        expectations.expect(session.redo() && session.undo() && session.undo(),
                            "repeated split/undo/redo settles");
    }

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// A layout edit retains the source snapshot, then an equivalent split over that retained snapshot
// must still map the retained frame's geometry to the current tail.
void testSplitAfterLayoutEditMapsRetainedSource(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Split After Layout");
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
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.3, 0.4, 0.5, 1.0}),
        "the fixture has a layer");
    const auto boundary = session.composition()->graph().layerOutputs().back();
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "the tail time is warmed");
    // A layout edit retains the source snapshot (render-neutral).
    const auto nodeId = session.composition()->graph().nodes().front().id;
    commands::Transaction move("Move Nodes", session.snapshot().revision());
    move.emplace<commands::MoveNodes>(
        compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {4, 5}}});
    expectations.expect(session.executeNodeTransaction(std::move(move)).changed(),
                        "a layout edit publishes");
    const auto retained = controller.state().frame;
    const auto retainedRevision = retained->desiredIdentity().sourceRevision;
    const auto retainedProject = retained->desiredIdentity().projectId;
    const auto warmed = preparations.load();
    commands::Transaction split("Split", session.snapshot().revision());
    split.emplace<commands::SplitLayerAtTime>(compositionId, boundary.layerId,
                                              core::RationalTime::fromInteger(3));
    const auto splitResult = session.executeTransaction(std::move(split));
    const auto tail = splitResult.outputId<document::LayerId>("layer");
    expectations.expect(splitResult.changed() && tail.has_value() &&
                            preparations.load() == warmed &&
                            controller.state().frame.get() == retained.get(),
                        "the split over a retained source snapshot prepares nothing");
    if (!tail.has_value()) {
        return;
    }
    expectations.expect(session.currentLayerForRetained(retainedRevision, retainedProject,
                                                        core::RationalTime::fromInteger(5),
                                                        boundary.layerId) == *tail,
                        "the retained source snapshot's tail geometry maps to the new tail");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// SPLIT-2 mixed finite command: a single transaction that extends the layer to [0,10) (finite
// pixel footprint [8,10)) AND splits it at t=4. The [8,10) interval becomes the NEW LIVE snapshot,
// whose active boundary there is the split tail B; it must carry NO mappings. A later split of B at
// t=9 must therefore map B->C freshly rather than composing through a stale A->B.
void testMixedFiniteExtendAndSplitLeavesLiveIntervalUnmapped(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Mixed Finite Split");
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
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.3, 0.4, 0.5, 1.0}),
        "the fixture has a layer");
    const auto boundary = session.composition()->graph().layerOutputs().back();
    // Initial range [0,8).
    commands::Transaction init("Initial range", session.snapshot().revision());
    init.emplace<commands::SetLayerRange>(compositionId, boundary.layerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(8));
    expectations.expect(session.executeTransaction(std::move(init)).changed(),
                        "the initial [0,8) range is set");
    // Mixed transaction: extend to [0,10) and split at 4.
    commands::Transaction mixed("Extend and split", session.snapshot().revision());
    mixed.emplace<commands::SetLayerRange>(compositionId, boundary.layerId, core::RationalTime{},
                                           core::RationalTime::fromInteger(10));
    mixed.emplace<commands::SplitLayerAtTime>(compositionId, boundary.layerId,
                                              core::RationalTime::fromInteger(4));
    const auto mixedResult = session.executeTransaction(std::move(mixed));
    // The split is the SECOND operation in this transaction.
    const auto tail = mixedResult.outputId<document::LayerId>("layer", 1);
    expectations.expect(mixedResult.changed() && tail.has_value(),
                        "the mixed extend+split publishes");
    if (!tail.has_value())
        return;
    // The [8,10) interval is newly live and must carry no mappings: its snapshot already names B.
    const auto ranges = session.evaluationSnapshotRanges();
    for (const auto& range : ranges) {
        if (range.start == core::RationalTime::fromInteger(8) &&
            range.end == core::RationalTime::fromInteger(10)) {
            expectations.expect(range.snapshot.revision() == session.snapshot().revision() &&
                                    range.mappings.empty(),
                                "the newly live [8,10) interval has no mappings");
        }
    }
    // Warm t=9.5 with the ACTUAL live B active there.
    const auto nineAndHalf = core::RationalTime::create(19, 2);
    expectations.expect(nineAndHalf.has_value() && session.setCurrentTime(*nineAndHalf) &&
                            waitUntil([&] { return isReady(controller); }),
                        "t9.5 warms with the live tail");
    if (!nineAndHalf.has_value()) {
        return;
    }
    const auto retained = controller.state().frame;
    const auto retainedRevision = retained->desiredIdentity().sourceRevision;
    const auto retainedProject = retained->desiredIdentity().projectId;
    const auto cachedBytes = controller.frameCache().residentBytes();
    const auto warmed = preparations.load();
    // Split B at t=9. The [9,10) interval maps B->C; t9.5 is in [9,10).
    commands::Transaction splitTail("Split tail", session.snapshot().revision());
    splitTail.emplace<commands::SplitLayerAtTime>(compositionId, *tail,
                                                  core::RationalTime::fromInteger(9));
    const auto splitTailResult = session.executeTransaction(std::move(splitTail));
    const auto tailTail = splitTailResult.outputId<document::LayerId>("layer");
    expectations.expect(splitTailResult.changed() && tailTail.has_value(), "the tail splits at 9");
    if (!tailTail.has_value())
        return;
    expectations.expect(preparations.load() == warmed &&
                            controller.frameCache().residentBytes() == cachedBytes &&
                            controller.state().frame.get() == retained.get(),
                        "the tail split preserves cached bytes and prepares nothing");
    // The retained t9.5 frame's live B boundary maps freshly to C, not through a stale A->B.
    expectations.expect(session.currentLayerForRetained(retainedRevision, retainedProject,
                                                        *nineAndHalf, *tail) == *tailTail,
                        "the retained live-B frame maps B->C freshly at t9.5");
    const auto oracleAt = [&](const core::RationalTime time) {
        const runtime::PreviewRequestIdentity identity{
            .projectId = session.snapshot().project().id(),
            .compositionId = compositionId,
            .sourceRevision = session.snapshot().revision(),
            .requestGeneration = 1,
            .time = time,
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
        return prepareLiveOracle(scheduler, fixture.pipeline, session.snapshot(), identity);
    };
    const auto bounds = controller.currentLayerBounds();
    const auto bound =
        std::ranges::find(bounds, *tailTail, &runtime::EvaluatedOperationBounds::layerId);
    const auto liveOracle = oracleAt(*nineAndHalf);
    const auto* oracleBound = boundForLayer(liveOracle, *tailTail);
    expectations.expect(bound != bounds.end() && oracleBound != nullptr && *bound == *oracleBound,
                        "the retained frame's geometry maps to C and matches the live oracle");
    // Undo/redo.
    expectations.expect(session.undo() &&
                            session.currentLayerForRetained(retainedRevision, retainedProject,
                                                            *nineAndHalf, *tail) == *tail,
                        "undo restores B at t9.5");
    expectations.expect(session.redo() &&
                            session.currentLayerForRetained(retainedRevision, retainedProject,
                                                            *nineAndHalf, *tail) == *tailTail,
                        "redo restores B->C at t9.5");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// TEMPORAL-DELETE: deleting an ordinary finite clip through the same RemoveNodes path the Timeline
// uses retains unaffected cached frames and re-derives only the clip's span.
