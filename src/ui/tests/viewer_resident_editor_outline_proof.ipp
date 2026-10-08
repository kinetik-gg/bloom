// Real ViewerEditor selection and native pointer movement. Phase markers coordinate external
// bounded compositor captures; the counter checks alone are not visible-pixel proof.
namespace {
class OutlineCancellationEvents final : public QObject {
  public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate ||
            event->type() == QEvent::Hide || event->type() == QEvent::Resize ||
            event->type() == QEvent::DevicePixelRatioChange) {
            if (events.size() < 32)
                events.emplace_back(watched->metaObject()->className(), event->type());
        }
        return false;
    }
    void observe(ui::ViewerEditor& viewer, QWidget& host, QWindow& native) {
        viewer.installEventFilter(this);
        host.installEventFilter(this);
        native.installEventFilter(this);
    }
    std::vector<std::pair<std::string, int>> events;
};
void reportOutlineFailure(ui::ViewerEditor& viewer, ui::CompositionPreviewController& controller,
                          const std::shared_ptr<ui::PreviewFrameTrace>& trace, QWindow& window,
                          const OutlineCancellationEvents& observer) {
    const auto frame = controller.state().frame;
    const auto geometry = frame ? ui::residentFrameGeometry(*frame) : std::nullopt;
    const auto ack = trace->lastNativeAcknowledgement();
    std::cout << "OUTLINE_FAILURE native_active=" << window.isActive()
              << " host_active=" << viewer.window()->isActiveWindow()
              << " viewer_focus=" << viewer.hasFocus() << " geometry=" << bool(geometry)
              << " lease_valid=" << (frame && ui::residentFrameGeometry(*frame).has_value());
    if (frame) {
        const auto desired = ui::PreviewFrameTrace::identity(frame->desiredIdentity());
        std::cout << " desired_revision=" << desired.revision
                  << " desired_generation=" << desired.generation;
    }
    if (ack)
        std::cout << " applied_revision=" << ack->identity.revision
                  << " applied_generation=" << ack->identity.generation;
    if (geometry)
        std::cout << " geometry_extent=" << geometry->displayExtent.width() << ','
                  << geometry->displayExtent.height();
    for (const auto& [object, type] : observer.events)
        std::cout << " event=" << object << ':' << type;
    std::cout << '\n' << std::flush;
}
void settleEditorOutlineAndHold(Expectations& checks, ui::ViewerEditor& viewer,
                                ui::CompositionPreviewController& controller,
                                const std::shared_ptr<ui::PreviewFrameTrace>& trace) {
    checks.expect(waitUntil(
                      [&] {
                          const auto ack = trace->lastNativeAcknowledgement();
                          const auto shown = controller.state().frame;
                          return ack && shown &&
                                 ack->identity ==
                                     ui::PreviewFrameTrace::identity(shown->desiredIdentity()) &&
                                 viewer.residentPresentationActiveForTest() &&
                                 !viewer.gpuCpuCoverVisibleForTest();
                      },
                      20s),
                  "the selected image/outline publication settles before visible drag proof");
    std::cout << "PHASE_EDITOR_BASE\n" << std::flush;
    holdEditorOutlinePhase();
}

void moveNativeOutlineSteps(Expectations& checks, ui::ViewerEditor& viewer,
                            ui::CompositionPreviewController& controller,
                            ui::CompositionSession& session,
                            const std::shared_ptr<ui::PreviewFrameTrace>& trace, QWindow& window,
                            QWidget& container, const QPointF centre, const QPointF delta) {
    for (int step = 1; step <= 4; ++step) {
        const auto sequenceBefore = viewer.gpuNativeAppliedSequenceForTest();
        sendNativeMouse(window, container, QEvent::MouseMove,
                        centre + delta * (static_cast<double>(step) / 4.0), Qt::NoButton,
                        Qt::LeftButton);
        checks.expect(
            waitUntil(
                [&] {
                    const auto ack = trace->lastNativeAcknowledgement();
                    const auto shown = controller.state().frame;
                    return ack && shown &&
                           ack->identity ==
                               ui::PreviewFrameTrace::identity(shown->desiredIdentity()) &&
                           viewer.gpuNativeAppliedSequenceForTest() > sequenceBefore &&
                           session.transformInteractionActive();
                },
                20s),
            "a real native drag makes coherent visible publication progress before release");
        std::cout << "PHASE_EDITOR_DRAG_" << step << '\n' << std::flush;
        holdEditorOutlinePhase();
    }
}

void exerciseNativeTextOutline(Expectations& checks, ui::ViewerEditor& viewer,
                               ui::CompositionPreviewController& controller,
                               ui::CompositionSession& session,
                               const std::shared_ptr<ui::PreviewFrameTrace>& trace, QWindow& window,
                               QWidget& container, QWidget& host) {
    std::optional<document::LayerId> text;
    for (const auto& layer : session.composition()->graph().layerOutputs())
        if (layer.name == "Baseline text")
            text = layer.layerId;
    checks.expect(text.has_value(), "native text proof uses the authored text layer");
    if (!text)
        return;
    session.selectLayer(*text);
    checks.expect(!viewer.mappingForTest(),
                  "a new selection cannot pick handles against the pending old native outline");
    checks.expect(
        waitUntil([&] { return viewer.mappingForTest().has_value(); }, 20s),
        "the text selected domain becomes passively pickable only after actual publication");
    const auto actual = viewer.actualDisplayDescriptor();
    const auto bounds = actual ? controller.selectedLayerBounds(actual->frame)
                               : std::vector<runtime::EvaluatedOperationBounds>{};
    const auto* position = session.parameterForSelection(document::kPositionParameterRole);
    checks.expect(actual && actual->mapping && !bounds.empty() && position,
                  "native text mapping and hit bounds come from the same applied frame");
    if (!actual || !actual->mapping || bounds.empty() || !position)
        return;
    const auto parameterId = position->id;
    const auto base = session.constantVec2Value(parameterId);
    if (!base) {
        checks.expect(false, "text position is a constant");
        return;
    }
    const auto& output = bounds.front().output;
    // Interior body hit away from the point-text anchor at the content centre.
    const auto centre =
        actual->mapping->toScreen({output.left + (output.right - output.left) * 0.25,
                                   output.top + (output.bottom - output.top) * 0.4});
    const QPointF delta(48.0, 28.0);
    const auto revision = session.snapshot().revision();
    OutlineCancellationEvents observer;
    observer.observe(viewer, host, window);
    std::cout << "PHASE_TEXT_BASE\n" << std::flush;
    holdEditorOutlinePhase();
    sendNativeMouse(window, container, QEvent::MouseButtonPress, centre, Qt::LeftButton,
                    Qt::LeftButton);
    checks.expect(session.transformInteractionActive(),
                  "real native text press begins the gesture");
    for (int step = 1; step <= 4; ++step) {
        const auto previous = viewer.gpuNativeAppliedSequenceForTest();
        sendNativeMouse(window, container, QEvent::MouseMove,
                        centre + delta * (static_cast<double>(step) / 4.0), Qt::NoButton,
                        Qt::LeftButton);
        checks.expect(waitUntil(
                          [&] {
                              return session.transformInteractionActive() &&
                                     viewer.gpuNativeAppliedSequenceForTest() > previous &&
                                     viewer.displayedPreviewState().freshness ==
                                         ui::FrameFreshness::Current;
                          },
                          20s),
                      "held text drag publishes another coherent actual text and outline pair");
        if (!session.transformInteractionActive())
            reportOutlineFailure(viewer, controller, trace, window, observer);
        std::cout << "PHASE_TEXT_DRAG_" << step << '\n' << std::flush;
        holdEditorOutlinePhase();
    }
    sendNativeMouse(window, container, QEvent::MouseButtonRelease, centre + delta, Qt::LeftButton,
                    Qt::NoButton);
    checks.expect(!session.transformInteractionActive() &&
                      session.snapshot().revision().value() == revision.value() + 1,
                  "text release makes exactly one undoable document transaction");
    const auto before = actual->mapping->toComposition(centre);
    const auto after = actual->mapping->toComposition(centre + delta);
    const document::Vec2d mapped{after.x - before.x, after.y - before.y};
    checks.expect(session.constantVec2Value(parameterId) ==
                      document::Vec2d{base->x + mapped.x, base->y + mapped.y},
                  "text movement uses the frozen actual mapping");
    checks.expect(session.undo() && session.constantVec2Value(parameterId) == *base,
                  "text undo restores the exact pre-drag value");
    checks.expect(waitForResidentRevision(controller, viewer, session.snapshot().revision(), 30s),
                  "text undo reaches real resident presentation");
    checks.expect(
        waitUntil(
            [&] {
                const auto shown = viewer.displayedPreviewState();
                return shown.frame &&
                       shown.frame->desiredIdentity().sourceRevision ==
                           session.snapshot().revision() &&
                       shown.freshness == ui::FrameFreshness::Current;
            },
            20s),
        "undone text phase waits for the exact owner-applied undone identity and outline");
    std::cout << "PHASE_TEXT_UNDONE\n" << std::flush;
    holdEditorOutlinePhase();
}
void exerciseNativeAdjustmentIntent(Expectations& checks, ui::ViewerEditor& viewer,
                                    ui::CompositionPreviewController& controller, QWidget& host,
                                    std::atomic<bool>& blocked, std::atomic<bool>& refuse,
                                    std::atomic<int>& calls) {
    auto* exposure = host.findChild<ui::kit::KValueField*>("viewerExposure");
    checks.expect(exposure != nullptr,
                  "the real native viewer exposes its hosted adjustment control");
    if (!exposure)
        return;
    const auto original = viewer.displayedFrameForTest();
    checks.expect(original && original->desiredIdentity().viewAdjust.neutral(),
                  "native adjustment proof starts with the actual neutral GPU intent");
    std::cout << "PHASE_INTENT_BASE\n" << std::flush;
    holdEditorOutlinePhase();
    blocked = true;
    exposure->stepBy(10);
    checks.expect(waitUntil([&] { return calls.load() == 1; }, 20s),
                  "the native viewer's explicit CPU analysis adjustment is held off-thread");
    checks.expect(viewer.displayedFrameForTest() == original &&
                      viewer.residentPresentationActiveForTest() &&
                      !viewer.gpuCpuCoverVisibleForTest() &&
                      viewer.displayedPreviewState().freshness == ui::FrameFreshness::Stale,
                  "no adjusted candidate retains actual native neutral pixels visibly stale");
    const auto& desiredBeforeResize = controller.state().desiredIdentity;
    const auto resolutionBeforeResize =
        desiredBeforeResize ? std::optional{desiredBeforeResize->resolution} : std::nullopt;
    const auto generationBeforeResize =
        desiredBeforeResize ? std::optional{desiredBeforeResize->requestGeneration} : std::nullopt;
    host.resize(host.width() + 32, host.height());
    QApplication::processEvents();
    const auto& desiredAfterResize = controller.state().desiredIdentity;
    checks.expect(resolutionBeforeResize && generationBeforeResize && desiredAfterResize &&
                      desiredAfterResize->resolution == *resolutionBeforeResize &&
                      desiredAfterResize->requestGeneration == *generationBeforeResize,
                  "resize during pending EV does not switch the shared GPU base to an Auto proxy");
    std::cout << "PHASE_INTENT_PENDING\n" << std::flush;
    holdEditorOutlinePhase();
    blocked = false;
    checks.expect(
        waitUntil(
            [&] { return viewer.displayedPreviewState().activity == ui::PreviewActivity::Failed; },
            20s),
        "the actual adjustment refusal is visible with the retained native frame");
    checks.expect(viewer.displayedFrameForTest() == original &&
                      viewer.residentPresentationActiveForTest() &&
                      viewer.displayedPreviewState().message.contains("adjustment was refused"),
                  "failed EV never retires or relabels the retained native neutral image");
    std::cout << "PHASE_INTENT_FAILED\n" << std::flush;
    holdEditorOutlinePhase();
    checks.expect(calls.load() == 1,
                  "native polling never retries the identical failed adjustment");
    refuse = false;
    exposure->stepBy(10);
    checks.expect(
        waitUntil(
            [&] {
                const auto shown = viewer.displayedPreviewState();
                return shown.frame && shown.frame->desiredIdentity().viewAdjust.exposure == 2.0 &&
                       shown.freshness == ui::FrameFreshness::Current &&
                       !viewer.hasLiveNativeTarget();
            },
            20s),
        "changed EV publishes a real qualified CPU adjustment after safe native retirement");
    const auto adjusted = viewer.actualDisplayDescriptor();
    checks.expect(adjusted && adjusted->frame && adjusted->frame->isOcioQualified() &&
                      !adjusted->native,
                  "the explicit CPU adjustment is honestly labelled CPU, never native GPU");
    std::cout << "PHASE_INTENT_ADJUSTED\n" << std::flush;
    holdEditorOutlinePhase();
    exposure->stepBy(-20);
    checks.expect(waitUntil(
                      [&] {
                          const auto shown = viewer.displayedPreviewState();
                          return shown.frame &&
                                 shown.frame->desiredIdentity().viewAdjust.neutral() &&
                                 shown.freshness == ui::FrameFreshness::Current &&
                                 viewer.residentPresentationActiveForTest();
                      },
                      20s),
                  "neutral controls recover through a new genuine native applied publication");
    bool recoveredCover = false;
    for (auto* window : QGuiApplication::allWindows()) {
        if (window->objectName() == "bloomViewerGpuCpuCoverWindow" && window->parent() &&
            window->parent()->surfaceType() == QSurface::VulkanSurface && window->isVisible())
            recoveredCover = true;
    }
    checks.expect(recoveredCover && !viewer.gpuCpuCoverVisibleForTest(),
                  "CPU retirement and fresh neutral recovery establish a new mapped transparent "
                  "raster child independently of the prior CPU concealment");
    std::cout << "PHASE_INTENT_RECOVERED\n" << std::flush;
    holdEditorOutlinePhase();
}
} // namespace
