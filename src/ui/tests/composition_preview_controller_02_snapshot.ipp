void testEvaluationSnapshotTimeIndexedProvenance(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Time Indexed Provenance");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the provenance fixture has a layer");
    const auto layerId = session.composition()->graph().layerOutputs().front().layerId;
    const auto duration = core::RationalTime::fromInteger(10);
    const auto setLayerRange = [&](const std::int64_t in, const std::int64_t out) {
        commands::Transaction transaction("Set Range", session.snapshot().revision());
        transaction.emplace<commands::SetLayerRange>(compositionId, layerId,
                                                     core::RationalTime::fromInteger(in),
                                                     core::RationalTime::fromInteger(out));
        return session.executeTransaction(std::move(transaction));
    };
    static_cast<void>(setLayerRange(0, 10));

    const auto initialRevision = session.snapshot().revision();
    expectations.expect(session.evaluationSnapshotRanges().size() == 1 &&
                            session.evaluationSnapshotRanges().front().start ==
                                core::RationalTime{} &&
                            session.evaluationSnapshotRanges().front().end == duration &&
                            session.evaluationSnapshot().revision() == initialRevision,
                        "initial provenance is one full-cover live span");

    // Trim right [0,10) -> [0,4): [0,4) retains, [4,10) advances.
    expectations.expect(setLayerRange(0, 4).changed(), "trim right publishes");
    const auto trimRevision = session.snapshot().revision();
    const auto afterTrim = session.evaluationSnapshotRanges();
    expectations.expect(afterTrim.size() == 2 && afterTrim[0].start == core::RationalTime{} &&
                            afterTrim[0].end == core::RationalTime::fromInteger(4) &&
                            afterTrim[0].snapshot.revision() == initialRevision &&
                            afterTrim[1].start == core::RationalTime::fromInteger(4) &&
                            afterTrim[1].end == duration &&
                            afterTrim[1].snapshot.revision() == trimRevision,
                        "trim retains the old span and advances the changed span");
    expectations.expect(
        session.evaluationSnapshotForTime(core::RationalTime::fromInteger(1)).revision() ==
                initialRevision &&
            session.evaluationSnapshotForTime(core::RationalTime::fromInteger(5)).revision() ==
                trimRevision &&
            session.evaluationSnapshotForTime(duration).revision() == trimRevision &&
            session.evaluationSnapshotForTime(core::RationalTime::fromInteger(-1)).revision() ==
                trimRevision,
        "for-time provenance is correct inside, at the boundary, and outside");
    expectations.expect(session.evaluationSnapshot().revision() == trimRevision,
                        "the compatibility accessor is conservative when provenance is mixed");

    // Extend [0,4) -> [0,7): [4,7) advances; three genuine revisions are retained.
    expectations.expect(setLayerRange(0, 7).changed(), "extend publishes");
    const auto extendRevision = session.snapshot().revision();
    const auto afterExtend = session.evaluationSnapshotRanges();
    expectations.expect(afterExtend.size() == 3 &&
                            afterExtend[1].start == core::RationalTime::fromInteger(4) &&
                            afterExtend[1].end == core::RationalTime::fromInteger(7) &&
                            afterExtend[1].snapshot.revision() == extendRevision &&
                            afterExtend[2].snapshot.revision() == trimRevision,
                        "extend retains several genuine revisions");

    // Undo/redo replay the symmetric stored footprint and only advance the changed range.
    expectations.expect(session.undo(), "undo publishes");
    const auto undoRevision = session.snapshot().revision();
    const auto afterUndo = session.evaluationSnapshotRanges();
    expectations.expect(afterUndo.size() == 3 &&
                            afterUndo[0].snapshot.revision() == initialRevision &&
                            afterUndo[1].snapshot.revision() == undoRevision &&
                            afterUndo[2].snapshot.revision() == trimRevision,
                        "undo advances only the changed range");
    expectations.expect(session.redo(), "redo publishes");
    expectations.expect(session.evaluationSnapshotRanges().size() == 3,
                        "redo restores the three genuine spans");

    // A neutral work-area edit preserves every span owner exactly.
    const auto beforeNeutral = session.evaluationSnapshotRanges();
    commands::Transaction neutral("Work Area", session.snapshot().revision());
    neutral.emplace<commands::SetWorkArea>(compositionId, core::RationalTime::fromInteger(1),
                                           core::RationalTime::fromInteger(5));
    expectations.expect(session.executeTransaction(std::move(neutral)).changed(),
                        "a neutral edit publishes");
    const auto afterNeutral = session.evaluationSnapshotRanges();
    expectations.expect(afterNeutral.size() == beforeNeutral.size(),
                        "a neutral edit preserves the provenance shape");
    for (std::size_t index = 0; index < afterNeutral.size(); ++index) {
        expectations.expect(afterNeutral[index].start == beforeNeutral[index].start &&
                                afterNeutral[index].end == beforeNeutral[index].end &&
                                afterNeutral[index].snapshot.revision() ==
                                    beforeNeutral[index].snapshot.revision() &&
                                &afterNeutral[index].snapshot.project() ==
                                    &beforeNeutral[index].snapshot.project(),
                            "a neutral edit preserves every span owner");
    }

    // A whole pixel edit resets all provenance to the live snapshot.
    commands::Transaction whole("Disable layer", session.snapshot().revision());
    whole.emplace<commands::SetLayerEnabled>(compositionId, layerId, false);
    expectations.expect(session.executeTransaction(std::move(whole)).changed() &&
                            session.evaluationSnapshotRanges().size() == 1 &&
                            session.evaluationSnapshotRanges().front().snapshot.revision() ==
                                session.snapshot().revision(),
                        "a whole pixel edit resets provenance to live");

    // A finite footprint with two disjoint intervals over the new live base.
    {
        commands::Transaction transaction("Two spans", session.snapshot().revision());
        commands::AffectedTimeFootprint footprint;
        footprint.compositionId = compositionId;
        footprint.intervals = {
            {core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(2)},
            {core::RationalTime::fromInteger(7), core::RationalTime::fromInteger(9)}};
        transaction.emplace<SessionFootprintOp>(footprint);
        const auto liveRevision = session.snapshot().revision();
        expectations.expect(session.executeTransaction(std::move(transaction)).changed(),
                            "a two-interval footprint publishes");
        const auto ranges = session.evaluationSnapshotRanges();
        expectations.expect(
            ranges.size() == 4 &&
                session.evaluationSnapshotForTime(core::RationalTime::fromInteger(3)).revision() ==
                    liveRevision &&
                session.evaluationSnapshotForTime(core::RationalTime::fromInteger(0)).revision() !=
                    liveRevision,
            "a two-interval footprint retains the gaps and advances both ranges");
    }
    // A footprint scoped to another composition resets to whole live.
    commands::Transaction foreign("Foreign footprint", session.snapshot().revision());
    foreign.emplace<ForeignFootprintOp>();
    expectations.expect(session.executeTransaction(std::move(foreign)).changed() &&
                            session.evaluationSnapshotRanges().size() == 1,
                        "a foreign-composition footprint resets to whole live");
}

void testEvaluationSnapshotRebindAndSwitch(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Provenance Rebind");
    const auto firstCompositionId = newProject.initialCompositionId;
    const auto secondCompositionId = document::CompositionId::fromRaw(2);
    expectations.expect(newProject.project.addComposition(makeSecondComposition()),
                        "the switch fixture adds a second composition");
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, firstCompositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the rebind fixture has a layer");
    const auto layerId = session.composition()->graph().layerOutputs().front().layerId;
    commands::Transaction trim("Set Range", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(firstCompositionId, layerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(4));
    expectations.expect(session.executeTransaction(std::move(trim)).changed() &&
                            session.evaluationSnapshotRanges().size() == 2,
                        "the first composition has mixed provenance");
    const auto* const firstProjectAddress = &session.snapshot().project();

    expectations.expect(session.setComposition(secondCompositionId), "composition switch");
    const auto switched = session.evaluationSnapshotRanges();
    expectations.expect(switched.size() == 1 && switched.front().start == core::RationalTime{} &&
                            switched.front().end == core::RationalTime::fromInteger(10) &&
                            switched.front().snapshot.revision() == session.snapshot().revision(),
                        "a composition switch resets provenance to the new composition live");

    // Rebind to a different document with the same numeric ProjectId/CompositionId/revision.
    auto secondProject = makeTestProject("Provenance Rebind");
    const auto secondInitialCompositionId = secondProject.initialCompositionId;
    document::Document secondDocument(std::move(secondProject.project));
    commands::CommandStack secondCommands(secondDocument);
    const auto secondRevision = secondDocument.snapshot().revision();
    session.rebind(secondDocument, secondCommands, secondInitialCompositionId);
    const auto rebound = session.evaluationSnapshotRanges();
    expectations.expect(
        rebound.size() == 1 && rebound.front().snapshot.revision() == secondRevision &&
            &rebound.front().snapshot.project() != firstProjectAddress &&
            &rebound.front().snapshot.project() == &secondDocument.snapshot().project(),
        "rebind adopts the new document and never reuses colliding numeric IDs");
}

void testEvaluationSnapshotCapFallback(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Provenance Cap");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    bool grew = false;
    bool resetObserved = false;
    std::size_t previous = session.evaluationSnapshotRanges().size();
    for (int edit = 0; edit < 10; ++edit) {
        commands::AffectedTimeFootprint footprint;
        footprint.compositionId = compositionId;
        for (int index = 0; index < 8; ++index) {
            const auto start = core::RationalTime::create(index * 100 + edit * 8, 1000);
            const auto end = core::RationalTime::create(index * 100 + edit * 8 + 1, 1000);
            expectations.expect(start.has_value() && end.has_value(),
                                "the cap fixture times are valid");
            if (!start.has_value() || !end.has_value())
                return;
            footprint.intervals.push_back({*start, *end});
        }
        commands::Transaction transaction("Footprint sweep", session.snapshot().revision());
        transaction.emplace<SessionFootprintOp>(footprint);
        expectations.expect(session.executeTransaction(std::move(transaction)).changed(),
                            "a multi-interval footprint edit publishes");
        const auto size = session.evaluationSnapshotRanges().size();
        if (size > previous)
            grew = true;
        if (previous > 1 && size == 1)
            resetObserved = true;
        expectations.expect(size <= ui::CompositionSession::kMaxEvaluationSnapshotRanges,
                            "the range count never exceeds the cap");
        previous = size;
    }
    expectations.expect(grew, "the provenance grew before the cap was reached");
    expectations.expect(resetObserved, "the cap deterministically falls back to full live");
}

// TEMPORAL-2B: a finite clip-range edit reuses unaffected cached frames, re-derives only the
// changed interval, and keeps the retained frame's pixels identical to a fresh current-snapshot
// evaluation. Also proves a non-document evaluation transition still forces a refresh.
// A TRUE oracle: prepare directly from the LIVE document snapshot (its revision is current, not the
// session's retained choice), independent of the controller's cache and provenance selection. This
// is what "fresh current CPU output" means; the explicit-refresh path is NOT an oracle because it
// also resolves evaluationSnapshotForTime and can serve a retained snapshot.
[[nodiscard]] bloom::ui::PreparedPreviewFrameHandle
prepareLiveOracle(bloom::runtime::TaskScheduler& scheduler,
                  const bloom::ui::PreviewPreparationFunction& pipeline,
                  const bloom::document::Snapshot& liveSnapshot,
                  const bloom::runtime::PreviewRequestIdentity& identity) {
    bloom::runtime::TaskRequest request(
        "Live oracle",
        {.kind = bloom::runtime::TaskOwnerKind::Composition,
         .id = bloom::runtime::TaskOwnerId::fromRaw(identity.compositionId.value())},
        bloom::runtime::TaskPriority::Visible);
    request.sourceVersion = {.documentRevision = identity.sourceRevision.value(),
                             .requestGeneration = identity.requestGeneration};
    auto submission = scheduler.submit<bloom::ui::PreviewPreparationResultHandle>(
        std::move(request),
        [liveSnapshot, identity, pipeline](bloom::runtime::TaskContext& context) {
            return pipeline(liveSnapshot, identity, bloom::ui::kDefaultPreviewPixelStorageByteLimit,
                            {}, context);
        });
    if (!submission.accepted())
        return nullptr;
    std::optional<bloom::runtime::TaskResult<bloom::ui::PreviewPreparationResultHandle>> result;
    if (!waitUntil([&] {
            result = submission.handle.tryTakeResult();
            return result.has_value();
        }))
        return nullptr;
    if (!result.has_value()) {
        return nullptr;
    }
    const auto& carried = result->value();
    if (!carried.has_value() || *carried == nullptr) {
        return nullptr;
    }
    return (*carried)->frame();
}

[[nodiscard]] bool sameDisplayPixels(const bloom::ui::PreparedPreviewFrameHandle& left,
                                     const bloom::ui::PreparedPreviewFrameHandle& right) {
    if (left == nullptr || right == nullptr)
        return false;
    const auto leftView = left->displayBufferView();
    const auto rightView = right->displayBufferView();
    return leftView.has_value() && rightView.has_value() &&
           leftView->displayWindow == rightView->displayWindow &&
           leftView->pixels.size() == rightView->pixels.size() &&
           std::ranges::equal(leftView->pixels, rightView->pixels);
}

[[nodiscard]] const bloom::runtime::EvaluatedOperationBounds*
boundForLayer(const bloom::ui::PreparedPreviewFrameHandle& frame,
              const bloom::document::LayerId layer) {
    if (frame == nullptr)
        return nullptr;
    const auto bounds = frame->evaluatedBounds();
    const auto found =
        std::ranges::find(bounds, layer, &bloom::runtime::EvaluatedOperationBounds::layerId);
    return found == bounds.end() ? nullptr : &*found;
}

void testTemporalRangeReuseKeepsUnaffectedFrames(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Temporal Reuse");
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
    // An opaque background and a translucent foreground whose position is animated over [0,8], so
    // samples inside the animation interval have distinct, nonempty blended pixels and distinct
    // foreground bounds.
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Background"), core::Color4d{0.1, 0.2, 0.3, 1.0}),
        "the temporal fixture has a background");
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Foreground"), core::Color4d{0.9, 0.4, 0.1, 1.0}),
        "the temporal fixture has a translucent foreground");
    const auto foregroundLayerId = session.composition()->graph().layerOutputs().back().layerId;
    expectations.expect(session.setSelectedOpacity(0.5), "the foreground is translucent");
    expectations.expect(session.currentTime() == core::RationalTime{},
                        "the fixture starts at time 0");
    expectations.expect(session.toggleKeyframe("position"), "key the foreground position at 0");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(8)) &&
                            session.setSelectedPosition(30.0, 20.0),
                        "key the foreground position at 8");
    const auto warm = [&](const std::int64_t second) {
        return session.setCurrentTime(core::RationalTime::fromInteger(second)) &&
               waitUntil([&] { return isReady(controller); });
    };
    expectations.expect(warm(1) && warm(2) && warm(5) && warm(1), "times 1, 2, 5 warm");
    const auto warmed = preparations.load();
    const auto retainedFrame = controller.state().frame;
    expectations.expect(retainedFrame != nullptr, "a warmed frame is displayed");

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

    // Trim [0,10) -> [0,4): only [4,10) changes. The displayed time 1 is unaffected.
    commands::Transaction trim("Trim", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, foregroundLayerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(4));
    expectations.expect(session.executeTransaction(std::move(trim)).changed(),
                        "the trim publishes");
    expectations.expect(preparations.load() == warmed && controller.state().frame == retainedFrame,
                        "an unaffected displayed frame is preserved with no preparation");
    const auto retainedRevision =
        session.evaluationSnapshotForTime(core::RationalTime::fromInteger(1)).revision();
    expectations.expect(retainedRevision != session.snapshot().revision(),
                        "time 1 keeps an older retained revision");

    // TRUE oracle parity at unaffected time 1: bytes AND foreground geometry keyed by layerId.
    const auto oracleOne = oracleAt(core::RationalTime::fromInteger(1));
    expectations.expect(oracleOne != nullptr &&
                            oracleOne->desiredIdentity().sourceRevision ==
                                session.snapshot().revision() &&
                            retainedFrame->desiredIdentity().sourceRevision == retainedRevision,
                        "the oracle is prepared from the live revision, the retained frame is not");
    const auto* retainedBound = boundForLayer(retainedFrame, foregroundLayerId);
    const auto* oracleBound = boundForLayer(oracleOne, foregroundLayerId);
    expectations.expect(sameDisplayPixels(retainedFrame, oracleOne) && retainedBound != nullptr &&
                            oracleBound != nullptr && *retainedBound == *oracleBound &&
                            !retainedBound->output.empty(),
                        "the retained frame matches the live oracle in pixels and layer geometry");

    // The changed interval's time 5 is re-derived; time 1 is then served from cache again.
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "the changed time reaches ready");
    expectations.expect(preparations.load() == warmed + 1, "only the changed interval re-prepares");
    const auto oracleFive = oracleAt(core::RationalTime::fromInteger(5));
    expectations.expect(oracleFive != nullptr &&
                            sameDisplayPixels(controller.state().frame, oracleFive),
                        "the re-derived changed frame matches the live oracle");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "time 1 is revisited");
    expectations.expect(preparations.load() == warmed + 1 &&
                            sameDisplayPixels(controller.state().frame, oracleOne),
                        "the unrelated cached frame is served without preparing, still matching");

    // Undo restores [0,10); the unaffected time 1 is preserved and still matches the live oracle.
    const auto beforeUndo = preparations.load();
    expectations.expect(session.undo(), "the trim undoes");
    expectations.expect(preparations.load() == beforeUndo &&
                            sameDisplayPixels(controller.state().frame,
                                              oracleAt(core::RationalTime::fromInteger(1))),
                        "undo preserves the unaffected frame and it matches the live oracle");

    // Shift to [3,10): the displayed time 1 is now inside the changed interval [0,3) and the
    // handler must proactively rebuild it (the session is already at time 1).
    const auto beforeShift = preparations.load();
    commands::Transaction shift("Shift", session.snapshot().revision());
    shift.emplace<commands::SetLayerRange>(compositionId, foregroundLayerId,
                                           core::RationalTime::fromInteger(3),
                                           core::RationalTime::fromInteger(10));
    expectations.expect(session.executeTransaction(std::move(shift)).changed() &&
                            waitUntil([&] { return isReady(controller); }) &&
                            preparations.load() == beforeShift + 1,
                        "a shift re-derives exactly the newly changed interval");
    expectations.expect(
        sameDisplayPixels(controller.state().frame, oracleAt(core::RationalTime::fromInteger(1))),
        "the shifted interval's frame matches the live oracle");

    // A non-document evaluation transition still forces a refresh even though revisions are equal.
    auto settings = document::makeBloomNeutralColorSettingsV1(core::Sha256Digest{});
    const auto beforeColor = preparations.load();
    session.setColorSettings(settings);
    expectations.expect(waitUntil([&] { return preparations.load() > beforeColor; }),
                        "a non-document evaluation transition forces a fresh derivation");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// TEMPORAL-2B target repair: a finite edit that changes the PENDING/ACTIVE target time must rebuild
// that target even when the last-good displayed frame sits at an unaffected earlier time. Warming
// t5 is gated so the edit lands while the t5 request is in flight and t1 is displayed.
