namespace {
void exercisePackedPlaying(Expectations& checks, ui::ViewerEditor& viewer, ui::EditorArea& area,
                           QSplitter& splitter, QWidget& host, ui::CompositionSession& session,
                           ui::CompositionPreviewController& controller,
                           runtime::GpuPreviewDisplayService& service) {
    auto& playback = controller.playbackController();
    playback.installWindowShortcut(host);
    const auto start = session.currentTime();
    playback.play();
    checks.expect(waitUntil([&] { return session.currentTime() != start; }, 3s),
                  "real production transport advances while ordinary Viewer paints");
    host.resize(1100, 700);
    splitter.setSizes({120, 720, 260});
    QApplication::processEvents();
    observe("PLAY_GROW", viewer, splitter, host, service, controller);
    for (int step = 0; step < 8; ++step) {
        splitter.setSizes({100 + 20 * step, 700 - 40 * step, 250 + 20 * step});
        QApplication::processEvents();
    }
    host.resize(650, 450);
    QApplication::processEvents();
    observe("PLAY_SHRINK", viewer, splitter, host, service, controller);
    const auto width = area.width();
    splitter.setSizes({200, width, splitter.width() - width - 200 - 2 * splitter.handleWidth()});
    QApplication::processEvents();
    observe("PLAY_MOVE", viewer, splitter, host, service, controller);
    playback.pause();
    checks.expect(
        waitUntil(
            [&] { return viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current; },
            30s),
        "paused newest time becomes truly painted Current");
    host.resize(1000, 640);
    splitter.setSizes({120, 620, 260});
    QApplication::processEvents();
    std::size_t activations = 0;
    const auto activationConnection =
        QObject::connect(&area, &ui::EditorArea::activationRequested, &area,
                         [&](ui::EditorArea*) { ++activations; });
    const auto mapping = viewer.mappingForTest();
    checks.expect(mapping.has_value(), "actual current painted mapping is available for input");
    if (mapping) {
        const auto point = mapping->displayRect.center();
        const auto before = std::chrono::steady_clock::now();
        QMouseEvent press(QEvent::MouseButtonPress, point, viewer.mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, viewer.mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &release);
        checks.expect(
            activations > 0 &&
                std::holds_alternative<document::LayerId>(session.selection().primary),
            "real widget input activates EditorArea and keeps selected scene hit coherent");
        std::cout << "PACKED_INPUT eventMicros="
                  << std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - before)
                         .count()
                  << " activations=" << activations
                  << " currentTime=" << session.currentTime().toSeconds() << '\n';
    }
    observe("INPUT", viewer, splitter, host, service, controller);
    auto* exposure = area.findChild<ui::kit::KValueField*>("viewerExposure");
    checks.expect(exposure != nullptr, "real exposure control exists");
    if (exposure) {
        exposure->stepBy(10);
        checks.expect(waitUntil(
                          [&] {
                              const auto state = viewer.displayedPreviewState();
                              return state.freshness == ui::FrameFreshness::Current &&
                                     state.frame &&
                                     state.frame->desiredIdentity().viewAdjust.exposure == 1.0;
                          },
                          30s),
                      "Viewer exposure is genuinely GPU-prepared before widget paint");
        checks.expect(viewer.displayedFrameForTest() &&
                          viewer.displayedFrameForTest()->provenance().provider ==
                              runtime::PreviewDisplayProvider::GpuPreparedPacked,
                      "adjustment preserves genuine GPU-packed provenance");
        observe("GPU_ADJUSTED", viewer, splitter, host, service, controller);
    }
    checks.expect(service.status().counters.cpuFallbacks == 0,
                  "playing/input/adjustment fixture has zero silently substituted CPU evaluations");
    QObject::disconnect(activationConnection);
}
} // namespace
