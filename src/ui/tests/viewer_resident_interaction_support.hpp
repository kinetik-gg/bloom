#pragma once

// Test-only native input, overlay and harness option helpers.
namespace {

using namespace std::chrono_literals;
using namespace bloom;
using namespace bloom::ui::verticalproof;

void holdEditorOutlinePhase() {
    QEventLoop loop;
    QTimer::singleShot(2500, &loop, &QEventLoop::quit);
    loop.exec();
}

struct HarnessOptions final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
    bool baseline = false;
    bool outlineProof = false;
    bool failAfterPresent = false;
    bool transitions = false;
    bool general = false;
    bool shutdownInFlight = false;
    std::filesystem::path trace;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else if (argument == "--fail-after-present") {
            options.failAfterPresent = true;
        } else if (argument == "--shutdown-in-flight") {
            options.shutdownInFlight = true;
        } else if (argument == "--general") {
            options.general = true;
        } else if (argument == "--transitions") {
            options.transitions = true;
        } else if (argument == "--outline-proof") {
            options.outlineProof = true;
        } else if (argument == "--baseline") {
            options.baseline = true;
        } else if (argument == "--trace" && index + 1 < argc) {
            options.trace = argv[++index];
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

// 77 (CTest SKIP_RETURN_CODE) when the native route is genuinely unavailable; 1 hard fail under
// --require-device, exactly like the other native proofs.
[[nodiscard]] int unavailable(const HarnessOptions& options, const std::string& reason) {
    std::cout << "SKIP: " << reason << '\n';
    return options.requireDevice ? 1 : 77;
}

[[nodiscard]] bool isResidentFrame(const ui::PreparedPreviewFrameHandle& frame) {
    return frame != nullptr &&
           frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
}

// The rectangle the viewer's own frozen mapping uses: viewTransformedDisplayRect() over the
// composition's full extent and the viewer's active transform. This is geometry, not the code
// under test.
[[nodiscard]] QRectF viewerDisplayRect(const ui::ViewerEditor& viewer,
                                       const document::CompositionFormat format) {
    const auto extent = render::ImageExtent::create(format.width(), format.height());
    if (!extent.hasValue()) {
        return {};
    }
    return ui::viewTransformedDisplayRect(viewer.canvasRectForTest(), *extent.value(),
                                          format.pixelAspect(), viewer.viewTransformForTest());
}

// The presenter names its native container exactly once; finding it by that object name is the
// real native surface, not a test-only accessor.
[[nodiscard]] QWidget* nativeContainer(ui::ViewerEditor& viewer) {
    return viewer.findChild<QWidget*>(QStringLiteral("bloomViewerGpuContainer"));
}

// The presenter's REAL Vulkan QWindow: `container->windowHandle()` is the direct link, and
// `QGuiApplication::allWindows()` with a VulkanSurface/ancestry match is the fallback so a test
// event is delivered to the actual native QWindow (whose presenter event filter is the only path
// into forwardGpuInput) rather than to a sibling QWidget.
[[nodiscard]] QWindow* nativeVulkanWindow(QWidget* container, QWidget* host) {
    QWindow* handle = container->windowHandle();
    for (QWindow* candidate : QGuiApplication::allWindows()) {
        if (!candidate->isVisible() || candidate->surfaceType() != QSurface::VulkanSurface) {
            continue;
        }
        if (handle != nullptr && candidate == handle) {
            return candidate;
        }
        if (host != nullptr) {
            for (QWindow* ancestor = candidate->parent(); ancestor != nullptr;
                 ancestor = ancestor->parent()) {
                if (ancestor == host->windowHandle()) {
                    return candidate;
                }
            }
        }
    }
    return handle;
}

// Deliver a mouse event to the REAL native QWindow so the presenter's own event filter and the
// forwardGpuInput translation run. `viewerLocal` is converted to window-local (the window fills the
// container at device pixel ratio 1), exactly as a platform click arrives.
void sendNativeMouse(QWindow& window, QWidget& container, const QEvent::Type type,
                     const QPointF& viewerLocal, const Qt::MouseButton button,
                     const Qt::MouseButtons buttons) {
    const QPointF local = viewerLocal - QPointF(container.pos());
    const QPointF global(container.mapToGlobal(local.toPoint()));
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &event);
    QCoreApplication::processEvents();
}

void sendDeleteKey(QWidget& widget) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

struct OverlayProbe final {
    bool valid = false;
    QImage image;

    [[nodiscard]] std::size_t framePixels() const {
        if (image.isNull()) {
            return 0;
        }
        const QColor expected = ui::kit::color(ui::kit::Color::CompositionFrame);
        std::size_t count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) == expected) {
                    ++count;
                }
            }
        }
        return count;
    }

    // Non-transparent ink strictly inside the central region: only the empty-state invitation
    // draws there, so this isolates it from the composition border that traces the perimeter.
    [[nodiscard]] std::size_t centreInkPixels() const {
        if (image.isNull()) {
            return 0;
        }
        const int x0 = image.width() * 35 / 100;
        const int x1 = image.width() * 65 / 100;
        const int y0 = image.height() * 35 / 100;
        const int y1 = image.height() * 65 / 100;
        std::size_t count = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (image.pixelColor(x, y).alpha() > 0) {
                    ++count;
                }
            }
        }
        return count;
    }
};

// Replays the ACTUAL overlay QPicture the viewer recorded for the resident present request into an
// image, exactly as the native presenter would raster it (device-free, no readback).
[[nodiscard]] OverlayProbe probeResidentOverlay(ui::ViewerEditor& viewer) {
    ui::ResidentPresentRequest request = viewer.buildResidentPresentRequestForTest();
    OverlayProbe probe;
    probe.valid = request.overlay.valid;
    const int width = static_cast<int>(std::ceil(request.overlay.logicalSize.width()));
    const int height = static_cast<int>(std::ceil(request.overlay.logicalSize.height()));
    if (width <= 0 || height <= 0 || request.overlay.picture.isNull()) {
        return probe;
    }
    probe.image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    probe.image.fill(Qt::transparent);
    QPainter painter(&probe.image);
    request.overlay.picture.play(&painter);
    painter.end();
    return probe;
}

[[nodiscard]] bool waitForResidentRevision(ui::CompositionPreviewController& controller,
                                           ui::ViewerEditor& viewer,
                                           const document::Revision revision,
                                           const std::chrono::milliseconds timeout) {
    return waitUntil(
        [&] {
            const auto frame = controller.state().frame;
            const auto actual = viewer.actualDisplayDescriptor();
            return isResidentFrame(frame) && viewer.residentPresentationActiveForTest() &&
                   frame->desiredIdentity().sourceRevision == revision && actual &&
                   actual->native && actual->frame &&
                   actual->frame->desiredIdentity() == frame->desiredIdentity() &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current &&
                   !viewer.gpuCpuCoverVisibleForTest() &&
                   controller.state().activity == ui::PreviewActivity::Ready;
        },
        timeout);
}

void verifyAndExportTrace(Expectations& checks, const std::shared_ptr<ui::PreviewFrameTrace>& trace,
                          const std::filesystem::path& tracePath) {
    // Check the REAL trace produced above, not a manufactured record sequence. Every
    // acknowledged identity must match an accepted submission for that presenter/sequence.
    const auto events = trace->events();
    for (const auto& ack : events) {
        if (ack.stage != ui::PreviewTraceStage::PresentAcknowledged)
            continue;
        const auto matching = std::ranges::find_if(events, [&](const auto& submitted) {
            return submitted.stage == ui::PreviewTraceStage::PresentSubmitted &&
                   submitted.outcome == ui::PreviewTraceOutcome::Accepted &&
                   submitted.presenter == ack.presenter && submitted.sequence == ack.sequence &&
                   submitted.identity == ack.identity;
        });
        checks.expect(matching != events.end(),
                      "native acknowledgement preserves submitted frame identity");
    }
    bool exported = false;
    std::thread writer([&] {
        std::ofstream output(tracePath);
        trace->writeJson(output);
        exported = output.good();
    });
    writer.join();
    checks.expect(exported, "the bounded baseline trace was exported off the UI thread");
}

} // namespace
