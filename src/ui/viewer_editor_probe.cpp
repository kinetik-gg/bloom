#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>

#include <QTimer>

#include <cmath>

namespace bloom::ui {
namespace {
std::optional<core::Color4d> referencePixel(const runtime::PreparedPreviewFrame& frame,
                                            QPoint pixel) {
    if (!frame.hasProcessFrame())
        return std::nullopt;
    const auto value = frame.processImage().read(pixel.x(), pixel.y());
    if (!value)
        return std::nullopt;
    const auto p = *value.value();
    return core::Color4d{static_cast<double>(p.red()), static_cast<double>(p.green()),
                         static_cast<double>(p.blue()), static_cast<double>(p.alpha())};
}

std::optional<core::Color4d> workingPixel(const std::optional<core::Color4d>& reference) {
    if (!reference.has_value()) {
        return std::nullopt;
    }
    if (reference->alpha == 0.0) {
        return core::Color4d{0.0, 0.0, 0.0, 0.0};
    }
    return core::Color4d{reference->red / reference->alpha, reference->green / reference->alpha,
                         reference->blue / reference->alpha, reference->alpha};
}
bool containsPixel(const render::ImageWindow window, QPoint pixel) {
    return pixel.x() >= window.originX() && pixel.x() < window.maxXExclusive() &&
           pixel.y() >= window.originY() && pixel.y() < window.maxYExclusive();
}

// The packed display sample from a frame that actually has CPU pixels; nullopt for the resident arm
// (its sample arrives asynchronously from the 1x1 CPU analysis fallback, never a GPU readback).
std::optional<render::Rgba8> displayPixel(const runtime::PreparedPreviewFrame& frame,
                                          QPoint pixel) {
    const auto buffer = frame.displayBufferView();
    if (!buffer.has_value()) {
        return std::nullopt;
    }
    const auto window = buffer->displayWindow;
    if (!containsPixel(window, pixel)) {
        return std::nullopt;
    }
    const auto offset =
        static_cast<std::size_t>(pixel.y() - window.originY()) * window.extent().width() +
        static_cast<std::size_t>(pixel.x() - window.originX());
    if (offset >= buffer->pixels.size()) {
        return std::nullopt;
    }
    return buffer->pixels[offset];
}
} // namespace

void ViewerEditor::clearProbe() {
    probePosition_.reset();
    probeReadout_ = {};
    if (probeTask_)
        probeTask_->cancel();
    emit probeChanged(probeReadout_);
}

void ViewerEditor::leaveEvent(QEvent* event) {
    clearProbe();
    QWidget::leaveEvent(event);
}

void ViewerEditor::refreshProbe(QPointF position) {
    probePosition_ = position;
    const auto mapping = currentMapping();
    const auto base = previewController_.state().frame;
    const auto shown = displayedFrame();
    if (!mapping || !base || !shown || !mapping->displayRect.contains(position) ||
        !contentRect().contains(position) || previewController_.isShuttingDown()) {
        clearProbe();
        return;
    }
    const auto coordinate = mapping->toComposition(position);
    // Geometry comes from the frame's own storage-independent display window: the packed CPU buffer
    // when the arm has one, otherwise the resident lease metadata. It is never inferred from an
    // absent CPU span.
    std::optional<render::ImageWindow> window;
    if (const auto buffer = shown->displayBufferView(); buffer.has_value()) {
        window = buffer->displayWindow;
    } else if (const auto resident = residentFrameGeometry(*shown); resident.has_value()) {
        window = resident->displayWindow;
    }
    if (!window.has_value()) {
        clearProbe();
        return;
    }
    const auto x = static_cast<int>(
        std::floor(coordinate.x * window->extent().width() / mapping->compositionFormat.width()));
    const auto y = static_cast<int>(
        std::floor(coordinate.y * window->extent().height() / mapping->compositionFormat.height()));
    const QPoint pixel(x, y);
    if (!containsPixel(*window, pixel)) {
        clearProbe();
        return;
    }
    std::optional<render::Rgba8> display = displayPixel(*shown, pixel);
    if (!display.has_value() && probeCacheFrame_ == shown && probeCachePixel_ == pixel) {
        display = probeCacheDisplay_;
    }
    ProbeReadout readout{.valid = true,
                         .coordinate = coordinate,
                         .display = display.value_or(render::Rgba8{}),
                         .displayEncoded = display.value_or(render::Rgba8{}),
                         .normalized =
                             display.has_value()
                                 ? core::Color4d{display->red / 255.0, display->green / 255.0,
                                                 display->blue / 255.0, display->alpha / 255.0}
                                 : core::Color4d{}};
    readout.workingColorSpaceId =
        QString::fromStdString(std::string(session_.colorIntent().workingColorSpaceId));
    readout.displayName = base->desiredIdentity().displayName.empty()
                              ? tr("default display")
                              : QString::fromStdString(base->desiredIdentity().displayName);
    if (probeCacheFrame_ == base && probeCachePixel_ == pixel) {
        readout.reference = probeCacheReference_;
        readout.displayLinear = probeCacheDisplayLinear_;
    }
    if (!readout.reference &&
        (!base->desiredIdentity().roi || containsPixel(*base->desiredIdentity().roi, pixel)))
        readout.reference = referencePixel(*base, pixel);
    readout.working = workingPixel(readout.reference);
    const bool needsDisplayLinear = base->isOcioQualified() && !readout.displayLinear.has_value();
    if (readout.reference && !needsDisplayLinear) {
        probeCacheFrame_ = base;
        probeCachePixel_ = pixel;
        probeCacheReference_ = readout.reference;
        probeCacheDisplayLinear_ = readout.displayLinear;
        if (probeTask_)
            probeTask_->cancel();
    } else {
        auto desired = base->desiredIdentity();
        const auto roi = render::ImageWindow::create(x, y, 1, 1);
        desired.roi = *roi.value();
        desired.viewAdjust = {};
        readout.pending = true;
        if (probeTask_) {
            if (probeIdentity_ != desired)
                probeTask_->cancel();
        } else if (probeIdentity_ != desired) {
            probeIdentity_ = desired;
            probeTaskFrame_ = base;
            probeTaskPixel_ = pixel;
            const auto submission = previewController_.submitViewerAnalysis(desired);
            if (submission.accepted()) {
                probeTask_ = submission.handle;
                probeTimer_->start();
            } else {
                readout.pending = false;
                probeFailure_ = tr("Pixel probe could not start");
            }
        } else if (!probeFailure_.isEmpty()) {
            readout.pending = false;
        }
        if (!readout.pending)
            readout.diagnostic = probeFailure_;
    }
    probeReadout_ = readout;
    emit probeChanged(readout);
}

void ViewerEditor::consumeProbe() {
    if (!probeTask_)
        return;
    auto result = probeTask_->tryTakeResult();
    if (!result)
        return;
    probeTask_.reset();
    probeTimer_->stop();
    const auto& value = result->value();
    if (value && *value && (*value)->frame() &&
        (*value)->frame()->desiredIdentity() == probeIdentity_) {
        probeCacheReference_ = referencePixel(*(*value)->frame(), probeTaskPixel_);
        probeCacheDisplayLinear_ = (*value)->frame()->displayLinearProbe();
        probeCacheDisplay_ = displayPixel(*(*value)->frame(), probeTaskPixel_);
        probeCacheFrame_ = probeTaskFrame_;
        probeCachePixel_ = probeTaskPixel_;
        probeFailure_.clear();
    } else if (result->state() == runtime::TaskState::Failed) {
        probeFailure_ = result->diagnostics().empty()
                            ? tr("Pixel probe failed")
                            : QString::fromStdString(result->diagnostics().front().summary);
    } else {
        probeIdentity_.reset();
    }
    probeTaskFrame_.reset();
    if (probePosition_)
        refreshProbe(*probePosition_);
}
} // namespace bloom::ui
