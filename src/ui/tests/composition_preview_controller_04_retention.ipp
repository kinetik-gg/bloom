void testTemporalDeleteRetainsUnaffectedFrames(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Temporal Delete");
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
        "the deletion fixture has a background");
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Clip"), core::Color4d{0.9, 0.4, 0.1, 1.0}),
        "the deletion fixture has a foreground clip");
    const auto clipBoundary = session.composition()->graph().layerOutputs().back();
    commands::Transaction trim("Clip range", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, clipBoundary.layerId,
                                          core::RationalTime::fromInteger(4),
                                          core::RationalTime::fromInteger(8));
    expectations.expect(session.executeTransaction(std::move(trim)).changed(),
                        "the clip is finite [4,8)");
    const auto warm = [&](const std::int64_t second) {
        return session.setCurrentTime(core::RationalTime::fromInteger(second)) &&
               waitUntil([&] { return isReady(controller); });
    };
    expectations.expect(warm(1) && warm(5) && warm(9) && warm(1), "times 1, 5, 9 warm");
    const auto warmed = preparations.load();
    const auto retainedOutside = controller.state().frame;
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
    const auto oracleOneBefore = oracleAt(core::RationalTime::fromInteger(1));
    expectations.expect(oracleOneBefore != nullptr, "the live oracle at the outside time prepares");
    expectations.expect(sameDisplayPixels(retainedOutside, oracleOneBefore),
                        "the outside warmed frame already matches the live oracle");
    const auto retainedRevision = retainedOutside->desiredIdentity().sourceRevision;

    commands::Transaction remove("Delete clip", session.snapshot().revision());
    remove.emplace<commands::RemoveNodes>(compositionId,
                                          std::set<document::NodeId>{clipBoundary.nodeId});
    expectations.expect(session.executeTransaction(std::move(remove)).changed(),
                        "the clip is deleted through RemoveNodes");
    expectations.expect(preparations.load() == warmed &&
                            controller.state().frame == retainedOutside,
                        "the unaffected outside frame is preserved with no preparation");
    expectations.expect(
        session.evaluationSnapshotForTime(core::RationalTime::fromInteger(1)).revision() !=
                session.snapshot().revision() &&
            session.evaluationSnapshotForTime(core::RationalTime::fromInteger(5)).revision() ==
                session.snapshot().revision(),
        "only the clip span is invalidated");

    // A NEW live oracle captured AFTER the deletion. The retained outside frame keeps the older
    // genuine revision but must match the post-deletion live output, and it must carry no
    // selectable bounds for the deleted layer (an inactive layer never sets a layerId).
    const auto oracleOneAfter = oracleAt(core::RationalTime::fromInteger(1));
    expectations.expect(
        oracleOneAfter != nullptr &&
            retainedOutside->desiredIdentity().sourceRevision == retainedRevision &&
            retainedRevision != session.snapshot().revision() &&
            oracleOneAfter->desiredIdentity().sourceRevision == session.snapshot().revision() &&
            sameDisplayPixels(retainedOutside, oracleOneAfter),
        "the retained frame keeps the old revision yet matches the post-deletion live oracle");
    expectations.expect(boundForLayer(retainedOutside, clipBoundary.layerId) == nullptr,
                        "the deleted layer has no selectable bounds in the retained outside frame");

    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "the deleted span's time reaches ready");
    expectations.expect(preparations.load() == warmed + 1, "only the clip span re-prepares");
    expectations.expect(
        sameDisplayPixels(controller.state().frame, oracleAt(core::RationalTime::fromInteger(5))),
        "the re-derived clip-span frame matches the live oracle");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                            waitUntil([&] { return isReady(controller); }) &&
                            preparations.load() == warmed + 1 &&
                            sameDisplayPixels(controller.state().frame, oracleOneAfter),
                        "the outside frame is retained and still matches the post-deletion live "
                        "oracle");

    // Undo restores the clip; the outside time is unaffected, the clip span invalidates again.
    const auto beforeUndo = preparations.load();
    expectations.expect(session.undo() && preparations.load() == beforeUndo,
                        "undo preserves the unaffected outside frame with no preparation");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(5)) &&
                            waitUntil([&] { return isReady(controller); }) &&
                            preparations.load() == beforeUndo + 1,
                        "undo invalidates exactly the restored clip span");
    expectations.expect(session.redo(), "redo publishes");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// TEMPORAL-DELETE conservative regression: deleting a parent whose child survives invalidates
// everything, including times OUTSIDE the parent's own visible span. The parent is a finite [4,8)
// clip with a nonzero transform; parenting inherits neither visibility nor opacity, so its
// transform still moves the child at t1 (outside [4,8)). Deleting the parent unparents the child,
// changing its geometry and pixels at t1.
void testTemporalDeleteParentDependencyInvalidatesWhole(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Temporal Delete Parent");
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
        session.addSolidLayer(QStringLiteral("Parent"), core::Color4d{0.2, 0.3, 0.4, 1}),
        "parent layer");
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Child"), core::Color4d{0.8, 0.2, 0.2, 1}),
        "child layer");
    const auto outputs = session.composition()->graph().layerOutputs();
    const auto& parent = outputs.front();
    const auto& child = outputs.back();
    // The parent is a finite [4,8) clip; the child is parented to it. Parenting inherits neither
    // visibility nor opacity, so the parent's transform still applies to the child at t1, OUTSIDE
    // the parent's own visible span.
    commands::Transaction setup("Parent clip and child", session.snapshot().revision());
    setup.emplace<commands::SetLayerRange>(compositionId, parent.layerId,
                                           core::RationalTime::fromInteger(4),
                                           core::RationalTime::fromInteger(8));
    setup.emplace<commands::SetLayerParent>(compositionId, child.layerId, parent.layerId);
    expectations.expect(session.executeTransaction(std::move(setup)).changed(),
                        "the parent is a finite [4,8) clip and the child is parented to it");
    session.selectLayer(parent.layerId);
    expectations.expect(session.setSelectedPosition(3.0, 2.0),
                        "the parent carries a nonzero transform");
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "t1 outside the parent's span is warmed");
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
    const auto retainedBefore = controller.state().frame;
    const auto oracleBefore = oracleAt(core::RationalTime::fromInteger(1));
    expectations.expect(oracleBefore != nullptr && sameDisplayPixels(retainedBefore, oracleBefore),
                        "the warmed t1 frame matches the live oracle with the parent present");
    const auto* childBefore = boundForLayer(retainedBefore, child.layerId);
    expectations.expect(childBefore != nullptr && !childBefore->output.empty(),
                        "the child has geometry at t1");

    const auto before = preparations.load();
    commands::Transaction remove("Delete parent", session.snapshot().revision());
    remove.emplace<commands::RemoveNodes>(compositionId, std::set<document::NodeId>{parent.nodeId});
    expectations.expect(session.executeTransaction(std::move(remove)).changed(),
                        "the parent is deleted");
    expectations.expect(
        session.evaluationSnapshotForTime(core::RationalTime::fromInteger(1)).revision() ==
            session.snapshot().revision(),
        "a surviving child forces whole-render invalidation at t1");
    expectations.expect(waitUntil([&] { return isReady(controller); }) &&
                            preparations.load() > before &&
                            controller.state().frame != retainedBefore,
                        "t1 re-prepares after the parent deletion");
    const auto oracleAfter = oracleAt(core::RationalTime::fromInteger(1));
    const auto* childAfter = boundForLayer(controller.state().frame, child.layerId);
    expectations.expect(oracleAfter != nullptr &&
                            sameDisplayPixels(controller.state().frame, oracleAfter),
                        "the re-derived t1 matches the post-deletion live oracle");
    expectations.expect(childAfter != nullptr && childBefore != nullptr &&
                            *childAfter != *childBefore,
                        "deleting the parent actually changed the child's geometry at t1");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// TEMPORAL-2B fail-closed: a finite footprint whose retained snapshot no longer matches the live
// time base resets the whole provenance to live instead of reusing inconsistent evidence.
class DurationFootprintOp final : public bloom::commands::Operation {
  public:
    explicit DurationFootprintOp(bloom::document::CompositionId composition)
        : composition_(composition) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.test.duration-footprint";
    }
    [[nodiscard]] bloom::commands::OperationResult
    apply(bloom::document::Draft& draft) const override {
        auto* composition = draft.project().findComposition(composition_);
        if (composition == nullptr)
            return bloom::commands::OperationResult::rejected(
                bloom::commands::OperationIssueCode::InvalidTarget, "missing composition");
        static_cast<void>(composition->setDuration(bloom::core::RationalTime::fromInteger(20)));
        auto result = bloom::commands::OperationResult::applied();
        result.affectedTimes = bloom::commands::AffectedTimeFootprint{
            .compositionId = composition_,
            .intervals = {
                {bloom::core::RationalTime{}, bloom::core::RationalTime::fromInteger(10)}}};
        return result;
    }

  private:
    bloom::document::CompositionId composition_;
};

void testInconsistentTimeBaseResetsProvenance(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Inconsistent Time Base");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.3, 0.4, 1.0}),
        "the time-base fixture has a layer");
    const auto layerId = session.composition()->graph().layerOutputs().front().layerId;
    commands::Transaction trim("Trim", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, layerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(4));
    expectations.expect(session.executeTransaction(std::move(trim)).changed() &&
                            session.evaluationSnapshotRanges().size() == 2,
                        "the fixture has a retained segment before the time-base change");
    commands::Transaction duration("Duration", session.snapshot().revision());
    duration.emplace<DurationFootprintOp>(compositionId);
    expectations.expect(session.executeTransaction(std::move(duration)).changed(),
                        "the inconsistent finite edit publishes");
    const auto ranges = session.evaluationSnapshotRanges();
    expectations.expect(ranges.size() == 1 && ranges.front().start == core::RationalTime{} &&
                            ranges.front().end == core::RationalTime::fromInteger(20) &&
                            ranges.front().snapshot.revision() == session.snapshot().revision(),
                        "inconsistent retained time-base evidence resets to whole live");
}

// Optional benchmark (invoked with --benchmark-temporal). Warms one 1280x720 solid-graph frame,
// performs an UNAFFECTED trim, then times retained-frame cache lookups (request key + take +
// envelope rebuild) against a TRUE LIVE oracle: several CPU preparations submitted from
// session.snapshot() (current revision, same display/resolution), median reported, parity checked.
// Returns false on a missing frame, timeout, or parity mismatch so main() exits nonzero. No CI
// hardware threshold. Excludes GPU, decode, and warm-up; this is a solid graph, not a broad FPS
// figure.
bool runTemporalBenchmark() {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(1280, 720);
    if (!format.has_value())
        return false;
    auto newProject = document::makeNewProject("Temporal Benchmark", "Main",
                                               core::RationalTime::fromInteger(10), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);
    if (!session.addSolidLayer(QStringLiteral("Bench"), core::Color4d{0.3, 0.5, 0.7, 1.0}))
        return false;
    const auto layerId = session.composition()->graph().layerOutputs().front().layerId;
    static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(1)));
    if (!waitUntil([&] { return isReady(controller); }))
        return false;
    const auto retained = controller.state().frame;
    if (retained == nullptr)
        return false;
    commands::Transaction trim("Trim", session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(compositionId, layerId, core::RationalTime{},
                                          core::RationalTime::fromInteger(4));
    static_cast<void>(session.executeTransaction(std::move(trim)));
    if (session.evaluationSnapshotForTime(core::RationalTime::fromInteger(1)).revision() ==
        session.snapshot().revision())
        return false; // the benchmark's premise: the retained frame is genuinely older

    constexpr int kLookups = 200;
    constexpr int kSamples = 7;
    std::uint64_t generation = 1'000'000;
    QElapsedTimer lookupTimer;
    lookupTimer.start();
    for (int index = 0; index < kLookups; ++index) {
        auto identity = retained->desiredIdentity();
        identity.requestGeneration = ++generation;
        if (controller.frameCache().take(identity) == nullptr)
            return false;
    }
    const auto lookupNs = lookupTimer.nsecsElapsed();

    std::vector<double> samplesMs;
    for (int sample = 0; sample < kSamples; ++sample) {
        const runtime::PreviewRequestIdentity identity{
            .projectId = session.snapshot().project().id(),
            .compositionId = compositionId,
            .sourceRevision = session.snapshot().revision(),
            .requestGeneration = ++generation,
            .time = core::RationalTime::fromInteger(1),
            .output = runtime::PreviewOutput::Composition,
            .resolution = controller.resolution(),
            .quality = controller.settings().quality,
            .colorIntent = controller.settings().colorIntent,
            .resolutionPolicy = controller.settings().resolutionPolicy,
            .displayName = controller.settings().displayName,
            .viewName = controller.settings().viewName,
            .showLook = controller.settings().showLook,
        };
        QElapsedTimer timer;
        timer.start();
        const auto oracle =
            prepareLiveOracle(scheduler, fixture.pipeline, session.snapshot(), identity);
        const auto elapsedNs = timer.nsecsElapsed();
        if (oracle == nullptr || !sameDisplayPixels(retained, oracle)) {
            std::cout << "temporal benchmark: live-oracle parity FAILED\n";
            return false;
        }
        samplesMs.push_back(static_cast<double>(elapsedNs) / 1.0e6);
    }
    std::ranges::sort(samplesMs);
    const auto medianMs = samplesMs[samplesMs.size() / 2];
    std::cout << "temporal benchmark 1280x720 solid graph (display/resolution identical): retained "
                 "lookup (request key + take + envelope) "
              << static_cast<double>(lookupNs) / kLookups / 1.0e6
              << " ms/request; TRUE live-oracle CPU preparation median " << medianMs << " ms over "
              << kSamples << " samples; parity OK (excludes GPU, decode, warm-up)\n";
    controller.beginShutdown();
    bridge.beginShutdown();
    static_cast<void>(waitUntil([&] { return scheduler.isQuiescent(); }));
    return true;
}
