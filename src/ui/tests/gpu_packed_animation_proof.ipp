// The ordinary packed Viewer must preserve animated text/solid pixels and real gesture bounds.
namespace {
void packedAnimationOracle(Expectations& checks, Fixture& fixture,
                           runtime::TaskScheduler& scheduler, ui::CompositionSession& session,
                           ui::ViewerEditor& viewer) {
    const auto frame = viewer.displayedFrameForTest();
    if (!frame) {
        checks.expect(false, "animation oracle has a real painted frame");
        return;
    }
    const auto& snapshot = session.snapshot();
    const auto overrides = session.transformInteractionOverrides();
    auto submission = scheduler.submit<bool>(
        runtime::TaskRequest("Packed animation CPU oracle",
                             {.kind = runtime::TaskOwnerKind::PanelRequest,
                              .id = runtime::TaskOwnerId::fromRaw(998)}),
        [&fixture, snapshot, overrides, frame](runtime::TaskContext&) {
            const auto& identity = frame->desiredIdentity();
            const auto compiled = fixture.compiler.compile({.snapshot = snapshot,
                                                            .compositionId = identity.compositionId,
                                                            .parameterOverrides = overrides},
                                                           {});
            const auto reference = compiled.plan ? planCpuReference(fixture, compiled.plan,
                                                                    identity, identity.resolution)
                                                 : nullptr;
            return runtime::TaskResult<bool>::succeeded(samePixels(frame, reference));
        });
    const auto result = awaitResult(submission.handle, 20s);
    checks.expect(result && result->value() && *result->value(),
                  "all animated/gesture packed pixels exactly match the worker CPU oracle");
    std::cout << "PACKED_ANIMATION_ORACLE time=" << frame->desiredIdentity().time.toSeconds()
              << " revision=" << frame->desiredIdentity().sourceRevision.value()
              << " overrides=" << overrides.size()
              << " exact=" << (result && result->value() && *result->value()) << '\n';
}

void exercisePackedAnimation(Expectations& checks, Fixture& fixture,
                             runtime::TaskScheduler& scheduler, ui::ViewerEditor& viewer,
                             QSplitter& splitter, QWidget& host, ui::CompositionSession& session,
                             ui::CompositionPreviewController& controller,
                             runtime::GpuPreviewDisplayService& service,
                             const std::shared_ptr<std::atomic_bool>& hold) {
    const auto current = [&] {
        const auto state = viewer.displayedPreviewState();
        const auto desired = controller.state().desiredIdentity;
        return state.freshness == ui::FrameFreshness::Current && state.frame && desired &&
               state.frame->desiredIdentity().requestGeneration == desired->requestGeneration &&
               state.frame->desiredIdentity().time == session.currentTime() &&
               state.frame->provenance().provider ==
                   runtime::PreviewDisplayProvider::GpuPreparedPacked;
    };
    checks.expect(waitUntil(current, 30s), "animated start is genuinely GPU-prepared and painted");
    const auto startPosition = session.effectiveVec2Value(document::kPositionParameterRole);
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("ANIM_START", viewer, splitter, host, service, controller);
    auto& playback = controller.playbackController();
    playback.play();
    bool visibleProgress = false;
    checks.expect(waitUntil(
                      [&] {
                          const auto shown = viewer.displayedFrameForTest();
                          if (shown &&
                              shown->desiredIdentity().time > core::RationalTime::fromInteger(0))
                              visibleProgress = true;
                          return session.currentTime() >= core::RationalTime::fromInteger(1);
                      },
                      5s),
                  "real default transport advances the animated scene");
    std::cout << "PACKED_ANIMATION_PLAYBACK visibleProgress=" << visibleProgress << " shownTime="
              << (viewer.displayedFrameForTest()
                      ? viewer.displayedFrameForTest()->desiredIdentity().time.toSeconds()
                      : -1)
              << " requestedTime=" << session.currentTime().toSeconds() << '\n';
    checks.expect(visibleProgress,
                  "actual animated GPU pixels progress while transport is playing");
    // Keep the actual producer briefly held, not the transport. The last genuinely completed
    // moving frame stays externally sampleable while newest desired time continues to advance.
    hold->store(true);
    checks.expect(
        waitUntil([&] { return service.status().counters.packedRetainedStagingBytes > 0; }, 3s),
        "short real final transfer hold makes changed playing output externally sampleable");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("ANIM_PLAYING", viewer, splitter, host, service, controller);
    hold->store(false);
    playback.pause();
    const auto settled = waitUntil(current, 10s);
    if (!settled) {
        std::cout << "PACKED_ANIMATION_FAILURE requestedTime=" << session.currentTime().toSeconds()
                  << " message=" << controller.state().message.toStdString() << '\n';
        for (const auto& diagnostic : controller.state().diagnostics)
            std::cout << "PACKED_ANIMATION_DIAGNOSTIC code=" << diagnostic.code
                      << " summary=" << diagnostic.summary << '\n';
    }
    checks.expect(settled, "animated transport sample settles at its exact real time");
    if (!settled)
        return;
    checks.expect(startPosition != session.effectiveVec2Value(document::kPositionParameterRole),
                  "authored solid position genuinely changes with real transport time");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("ANIM_PLAYED", viewer, splitter, host, service, controller);
    std::optional<document::LayerId> text;
    for (const auto& layer : session.composition()->graph().layerOutputs())
        if (layer.name == "Animated text")
            text = layer.layerId;
    checks.expect(text.has_value(), "real authored animated text exists");
    if (!text)
        return;
    session.selectLayer(*text);
    checks.expect(waitUntil([&] { return current() && viewer.mappingForTest().has_value(); }, 30s),
                  "text selection mapping is coupled to actual GPU-packed publication");
    const auto mapping = viewer.mappingForTest();
    const auto frame = viewer.displayedFrameForTest();
    const auto bounds = controller.selectedLayerBounds(frame);
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    if (!mapping || bounds.empty() || !position) {
        checks.expect(false, "text gesture has actual mapping, bounds and authored position");
        return;
    }
    const auto parameter = position->id;
    const auto base = session.constantVec2Value(parameter);
    if (!base) {
        checks.expect(false, "text position is constant before gesture");
        return;
    }
    const auto opacity = session.effectiveScalarValue(document::kOpacityParameterRole);
    checks.expect(opacity && *opacity < 1.0 && *opacity > 0.2,
                  "text opacity is genuinely sampled from animation");
    const auto& output = bounds.front().output;
    const auto point = mapping->toScreen({output.left + (output.right - output.left) * 0.25,
                                          output.top + (output.bottom - output.top) * 0.4});
    const QPointF delta{48.0, 28.0};
    const auto revision = session.snapshot().revision();
    const auto send = [&](QEvent::Type type, QPointF local) {
        QMouseEvent event(type, local, viewer.mapToGlobal(local),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&viewer, &event);
    };
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("TEXT_BASE", viewer, splitter, host, service, controller);
    send(QEvent::MouseButtonPress, point);
    checks.expect(session.transformInteractionActive(), "real text body press starts manipulation");
    send(QEvent::MouseMove, point + delta * 0.5);
    checks.expect(waitUntil([&] { return current() && session.transformInteractionActive(); }, 30s),
                  "held text move paints current coherent GPU image and matching bounds");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("TEXT_MID", viewer, splitter, host, service, controller);
    send(QEvent::MouseMove, point + delta);
    checks.expect(waitUntil([&] { return current() && session.transformInteractionActive(); }, 30s),
                  "second held move paints genuine newest GPU output");
    send(QEvent::MouseButtonRelease, point + delta);
    const auto before = mapping->toComposition(point);
    const auto after = mapping->toComposition(point + delta);
    const document::Vec2d expected{base->x + after.x - before.x, base->y + after.y - before.y};
    const auto actual = session.constantVec2Value(parameter);
    checks.expect(!session.transformInteractionActive() &&
                      session.snapshot().revision().value() == revision.value() + 1 && actual &&
                      std::abs(actual->x - expected.x) < 1e-8 &&
                      std::abs(actual->y - expected.y) < 1e-8,
                  "real text release commits one transaction with frozen-mapping position");
    checks.expect(waitUntil(current, 30s),
                  "committed text reaches genuine current packed publication");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("TEXT_COMMIT", viewer, splitter, host, service, controller);
    checks.expect(session.undo() && session.constantVec2Value(parameter) == base,
                  "one undo restores exact authored pre-drag text position");
    checks.expect(waitUntil(current, 30s), "undone image and bounds genuinely paint Current");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    observe("TEXT_UNDO", viewer, splitter, host, service, controller);
    checks.expect(service.status().counters.cpuFallbacks == 0,
                  "animation, live drag, commit and undo stay on required real GPU preparation");
}
} // namespace
