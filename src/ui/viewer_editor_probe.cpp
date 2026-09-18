#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>

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
    const auto buffer = shown->displayBufferView();
    if (!buffer) {
        clearProbe();
        return;
    }
    const auto coordinate = mapping->toComposition(position);
    const auto window = buffer->displayWindow;
    const auto x = static_cast<int>(
        std::floor(coordinate.x * window.extent().width() / mapping->compositionFormat.width()));
    const auto y = static_cast<int>(
        std::floor(coordinate.y * window.extent().height() / mapping->compositionFormat.height()));
    const QPoint pixel(x, y);
    if (!containsPixel(window, pixel)) {
        clearProbe();
        return;
    }
    const auto offset = static_cast<std::size_t>(y - window.originY()) * window.extent().width() +
                        static_cast<std::size_t>(x - window.originX());
    const auto display = buffer->pixels[offset];
    ProbeReadout readout{.valid = true,
                         .coordinate = coordinate,
                         .display = display,
                         .displayEncoded = display,
                         .normalized = {display.red / 255.0, display.green / 255.0,
                                        display.blue / 255.0, display.alpha / 255.0}};
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
