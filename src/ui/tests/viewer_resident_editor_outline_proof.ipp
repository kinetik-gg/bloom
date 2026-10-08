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
} // namespace
