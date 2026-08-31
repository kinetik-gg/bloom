#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>

#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>

#include <algorithm>
#include <limits>
#include <optional>
#include <variant>

namespace bloom::ui {
namespace {

const document::LayerOutputBoundary* layerBoundary(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto boundaries = composition.graph().layerOutputs();
    const auto found = std::ranges::find_if(
        boundaries, [layerId](const auto& candidate) { return candidate.layerId == layerId; });
    return found == boundaries.end() ? nullptr : &*found;
}

QString layerName(const document::Composition& composition, const document::LayerId layerId) {
    const auto* boundary = layerBoundary(composition, layerId);
    if (boundary == nullptr || boundary->name.empty()) {
        return QStringLiteral("Layer %1").arg(layerId.value());
    }
    return QString::fromStdString(boundary->name);
}

QString previewStatusText(const CompositionPreviewState& state) {
    switch (state.activity) {
    case PreviewActivity::Rendering:
        return state.freshness == FrameFreshness::Stale
                   ? ViewerEditor::tr("Rendering current frame · Previous frame shown")
                   : ViewerEditor::tr("Rendering current frame");
    case PreviewActivity::Ready:
        return ViewerEditor::tr("Current frame ready");
    case PreviewActivity::Unsupported:
        return state.freshness == FrameFreshness::Stale
                   ? ViewerEditor::tr("Preview unsupported · Previous frame shown")
                   : ViewerEditor::tr("Preview unsupported");
    case PreviewActivity::Cancelled:
        return state.freshness == FrameFreshness::Stale
                   ? ViewerEditor::tr("Preview cancelled · Previous frame shown")
                   : ViewerEditor::tr("Preview cancelled");
    case PreviewActivity::Failed:
        return state.freshness == FrameFreshness::Stale
                   ? ViewerEditor::tr("Preview failed · Previous frame shown")
                   : ViewerEditor::tr("Preview failed");
    }
    return ViewerEditor::tr("Preview unavailable");
}

QColor previewStatusColor(const PreviewActivity activity) {
    switch (activity) {
    case PreviewActivity::Rendering:
        return QColor(49, 137, 202);
    case PreviewActivity::Ready:
        return QColor(67, 154, 92);
    case PreviewActivity::Unsupported:
        return QColor(194, 142, 51);
    case PreviewActivity::Cancelled:
        return QColor(128, 132, 142);
    case PreviewActivity::Failed:
        return QColor(194, 68, 73);
    }
    return QColor(128, 132, 142);
}

void drawCheckerboard(QPainter& painter, const QRectF& bounds) {
    constexpr qreal tileSize = 12.0;
    painter.save();
    painter.setClipRect(bounds);
    painter.fillRect(bounds, QColor(49, 51, 56));
    const int columns = static_cast<int>(bounds.width() / tileSize) + 2;
    const int rows = static_cast<int>(bounds.height() / tileSize) + 2;
    for (int row = 0; row < rows; ++row) {
        for (int column = row % 2; column < columns; column += 2) {
            painter.fillRect(QRectF(bounds.left() + column * tileSize,
                                    bounds.top() + row * tileSize, tileSize, tileSize),
                             QColor(61, 63, 68));
        }
    }
    painter.restore();
}

void drawStatusChip(QPainter& painter, const QRectF& available, QString text, const QColor color,
                    const Qt::Alignment horizontalAlignment) {
    constexpr qreal horizontalPadding = 8.0;
    constexpr qreal chipHeight = 24.0;
    const qreal maximumWidth = std::max<qreal>(0.0, available.width() - 24.0);
    text = painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                            static_cast<int>(maximumWidth - 2 * horizontalPadding));
    const qreal width = std::min<qreal>(
        maximumWidth, painter.fontMetrics().horizontalAdvance(text) + 2 * horizontalPadding);
    const qreal left = horizontalAlignment.testFlag(Qt::AlignRight)
                           ? available.right() - width - 12.0
                           : available.left() + 12.0;
    const QRectF chip(left, available.top() + 10.0, width, chipHeight);
    painter.setPen(QPen(color.lighter(125), 1.0));
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), 218));
    painter.drawRoundedRect(chip, 4.0, 4.0);
    painter.setPen(QColor(245, 246, 248));
    painter.drawText(chip.adjusted(horizontalPadding, 0.0, -horizontalPadding, 0.0),
                     Qt::AlignVCenter | Qt::AlignLeft, text);
}

void drawDiagnosticBanner(QPainter& painter, const QRectF& available, const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    const QRectF banner =
        available.adjusted(24.0, available.height() * 0.38, -24.0, -available.height() * 0.38);
    painter.setPen(QPen(QColor(121, 124, 132), 1.0));
    painter.setBrush(QColor(22, 23, 26, 224));
    painter.drawRoundedRect(banner, 5.0, 5.0);
    painter.setPen(QColor(226, 228, 233));
    const QString visible = painter.fontMetrics().elidedText(
        message, Qt::ElideRight, static_cast<int>(std::max<qreal>(0.0, banner.width() - 20.0)));
    painter.drawText(banner.adjusted(10.0, 0.0, -10.0, 0.0), Qt::AlignCenter, visible);
}

// The composed-frame rectangle paintEvent() draws pixels into (see the identical expression
// there); shared with currentMapping() so direct manipulation maps screen deltas against exactly
// the rectangle the user sees.
QRectF viewerFrame(const QWidget& widget) {
    return QRectF(widget.rect()).adjusted(28.0, 28.0, -28.0, -28.0);
}

} // namespace

QRectF fitDisplayRect(const QRectF& available, const render::ImageExtent extent,
                      const core::PixelAspectRatio pixelAspect) noexcept {
    if (available.isEmpty()) {
        return {};
    }

    const long double displayWidth = static_cast<long double>(extent.width()) *
                                     pixelAspect.numerator() / pixelAspect.denominator();
    const long double displayHeight = extent.height();
    if (displayWidth <= 0.0L || displayHeight <= 0.0L) {
        return {};
    }

    const long double scale =
        std::min(static_cast<long double>(available.width()) / displayWidth,
                 static_cast<long double>(available.height()) / displayHeight);
    const qreal fittedWidth = static_cast<qreal>(displayWidth * scale);
    const qreal fittedHeight = static_cast<qreal>(displayHeight * scale);
    return QRectF(available.center().x() - fittedWidth / 2.0,
                  available.center().y() - fittedHeight / 2.0, fittedWidth, fittedHeight);
}

ViewerEditor::ViewerEditor(CompositionSession& session,
                           CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    setMinimumSize(220, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // StrongFocus lets a press-to-drag gesture also receive the Escape key that cancels it.
    setFocusPolicy(Qt::StrongFocus);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        updatePreviewAccessibility();
        // A newly delivered frame may carry a format/proxy/pixel-aspect/display-descriptor change
        // (docs/architecture/animation-and-time.md); a mid-drag mapping change cancels the gesture
        // rather than silently mis-mapping the rest of it.
        if (dragActive_ && !mappingStillValid()) {
            endDrag(false);
        }
        update();
    });
    updatePreviewAccessibility();
}

void ViewerEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(15, 16, 18));

    const QRectF frame = viewerFrame(*this);
    painter.fillRect(frame, QColor(39, 41, 45));
    painter.setPen(QPen(QColor(77, 80, 87), 1.0));
    painter.drawRect(frame.adjusted(0.0, 0.0, -1.0, -1.0));

    constexpr int gridStep = 32;
    painter.setPen(QPen(QColor(46, 48, 53), 1.0));
    for (int offset = gridStep; static_cast<qreal>(offset) < frame.width(); offset += gridStep) {
        const qreal x = frame.left() + static_cast<qreal>(offset);
        painter.drawLine(QPointF(x, frame.top()), QPointF(x, frame.bottom()));
    }
    for (int offset = gridStep; static_cast<qreal>(offset) < frame.height(); offset += gridStep) {
        const qreal y = frame.top() + static_cast<qreal>(offset);
        painter.drawLine(QPointF(frame.left(), y), QPointF(frame.right(), y));
    }

    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle displayedFrame = preview.frame;
    bool drewPixels = false;
    bool drewQualifiedPixels = false;
    if (displayedFrame != nullptr) {
        // displayBufferView() normalizes both display-product alternatives (reference and
        // qualified) to the same packed-RGBA8 shape (design decision 2) -- the viewer draws
        // pixels identically either way; isOcioQualified is the only distinguishing bit, used
        // below only for the corner provenance label, never to change how pixels are drawn.
        const auto bufferView = displayedFrame->displayBufferView();
        if (bufferView.has_value()) {
            const auto extent = bufferView->displayWindow.extent();
            const auto& layout = bufferView->layout;
            if (extent.width() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                extent.height() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                layout.rowStrideBytes <=
                    static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
                const auto pixels = bufferView->pixels;
                // displayedFrame owns the immutable bytes for this entire paint. The const-data
                // QImage constructor borrows them, so presentation does not copy or convert a
                // full frame on the UI thread.
                const QImage image(
                    reinterpret_cast<const uchar*>(pixels.data()), static_cast<int>(extent.width()),
                    static_cast<int>(extent.height()),
                    static_cast<qsizetype>(layout.rowStrideBytes), QImage::Format_RGBA8888);
                if (!image.isNull()) {
                    const QRectF displayRect =
                        fitDisplayRect(frame, extent, bufferView->pixelAspect);
                    drawCheckerboard(painter, displayRect);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(displayRect, image, QRectF(image.rect()));
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.setPen(QPen(QColor(104, 107, 114), 1.0));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(displayRect.adjusted(0.0, 0.0, -1.0, -1.0));
                    drewPixels = true;
                    drewQualifiedPixels = bufferView->isOcioQualified;
                }
            }
        }
    }

    const auto* composition = session_.composition();
    painter.setPen(QColor(201, 204, 210));
    painter.drawText(QRectF(frame.left(), 2.0, frame.width(), 22.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     composition == nullptr ? tr("No composition")
                                            : QString::fromStdString(composition->name()));
    painter.setPen(QColor(169, 165, 219));
    // Never a silent relabel: this reads the envelope's own isOcioQualified flag every paint,
    // rather than assuming qualified once readiness is reached (design decision 2 / the "reference
    // path is never silently relabeled as qualified" contract). A frame drawn before the qualified
    // processor is ready -- or the last-good frame retained through a later qualification failure
    // -- keeps reading as "Reference display (unqualified)" here for exactly as long as it is.
    painter.drawText(QRectF(frame.left(), 2.0, frame.width(), 22.0),
                     Qt::AlignRight | Qt::AlignVCenter,
                     drewPixels ? (drewQualifiedPixels ? tr("Qualified display")
                                                       : tr("Reference display (unqualified)"))
                                : tr("Reference display"));

    if (!drewPixels) {
        painter.setPen(QColor(178, 182, 190));
        painter.drawText(frame.adjusted(24.0, 44.0, -24.0, -44.0), Qt::AlignCenter,
                         previewStatusText(preview));
    }

    drawStatusChip(painter, frame, previewStatusText(preview), previewStatusColor(preview.activity),
                   Qt::AlignLeft);
    if (drewPixels && preview.activity != PreviewActivity::Ready &&
        preview.activity != PreviewActivity::Rendering) {
        drawDiagnosticBanner(painter, frame, preview.message);
    }

    if (session_.selection().contextualLayer.has_value() && composition != nullptr) {
        const QString name = layerName(*composition, *session_.selection().contextualLayer);
        const QRectF selectionStatus(frame.left() + 12.0, frame.bottom() - 38.0,
                                     frame.width() - 24.0, 26.0);
        painter.setPen(QPen(QColor(44, 158, 232), 1.0));
        painter.setBrush(QColor(27, 46, 61));
        painter.drawRoundedRect(selectionStatus, 4.0, 4.0);
        painter.setPen(QColor(208, 231, 247));
        const QString status = tr("Selected: %1 · evaluated bounds unavailable").arg(name);
        painter.drawText(
            selectionStatus.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(status, Qt::ElideRight,
                                             static_cast<int>(selectionStatus.width() - 16.0)));
    }
}

void ViewerEditor::updatePreviewAccessibility() {
    const auto& preview = previewController_.state();
    QString frameDescription;
    switch (preview.freshness) {
    case FrameFreshness::None:
        frameDescription = tr("No composition pixels are displayed");
        break;
    case FrameFreshness::Current:
        frameDescription = tr("Current composition pixels are displayed");
        break;
    case FrameFreshness::Stale:
        frameDescription = tr("Previous composition pixels are displayed and marked out of date");
        break;
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    const QString colorStateDescription =
        bufferView.has_value()
            ? (bufferView->isOcioQualified ? tr("Qualified display.")
                                           : tr("Reference display (unqualified)."))
            : tr("Reference display.");
    setAccessibleDescription(
        tr("%1. %2. %3").arg(preview.message, frameDescription, colorStateDescription));
}

std::optional<PositionInteractionMapping> ViewerEditor::currentMapping() const {
    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle& frameHandle = preview.frame;
    if (frameHandle == nullptr) {
        return std::nullopt;
    }
    // A stale frame from another composition -- or an older revision of this one -- is never a
    // mapping source (docs/architecture/animation-and-time.md, "Direct Manipulation And Preview
    // Overrides").
    if (frameHandle->desiredIdentity().compositionId != session_.compositionId() ||
        frameHandle->desiredIdentity().sourceRevision != session_.snapshot().revision()) {
        return std::nullopt;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    // The gesture-mapping geometry is alternative-agnostic (design decision 2): a qualified frame's
    // window/pixel-aspect maps a drag gesture exactly the way a reference frame's does. The frozen
    // PositionInteractionMapping::displayDescriptor stays a
    // render::ReferenceDisplayBufferDescriptor purely as a geometry/change-detection value here
    // (extent, pixel aspect, packed layout) -- never as a claim that the underlying pixels are the
    // unqualified reference product; a qualified frame's isOcioQualified() bit lives on
    // PreviewDisplayBufferView above, not on this reused geometry type, and nothing reads this
    // descriptor's own (always-false) isOcioQualified() to decide provenance.
    const auto bufferView = frameHandle->displayBufferView();
    if (!bufferView.has_value()) {
        return std::nullopt;
    }
    const auto descriptorResult = render::ReferenceDisplayBufferDescriptor::create(
        bufferView->displayWindow, bufferView->pixelAspect);
    if (!descriptorResult) {
        return std::nullopt;
    }
    const auto descriptor = *descriptorResult.value();
    const QRectF displayRect = fitDisplayRect(
        viewerFrame(*this), descriptor.displayWindow().extent(), descriptor.pixelAspect());
    if (displayRect.isEmpty()) {
        return std::nullopt;
    }
    return PositionInteractionMapping{
        .displayRect = displayRect,
        .compositionFormat = composition->format(),
        .resolution = frameHandle->desiredIdentity().resolution,
        .pixelAspect = descriptor.pixelAspect(),
        .displayDescriptor = descriptor,
    };
}

bool ViewerEditor::mappingStillValid() const {
    if (!activeMapping_.has_value()) {
        return false;
    }
    const auto mapping = currentMapping();
    return mapping.has_value() && *mapping == *activeMapping_;
}

void ViewerEditor::endDrag(const bool commit) {
    dragActive_ = false;
    activeMapping_.reset();
    if (commit) {
        (void)session_.commitPositionInteraction();
    } else {
        session_.cancelPositionInteraction();
    }
    // Reuses TimelineRuler's Interactive-cadence arming (docs/architecture/animation-and-time.md,
    // "Session Time And Scrubbing"): bypasses any remaining trailing delay and disarms it.
    previewController_.notifyScrubEnded();
}

void ViewerEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton ||
        !std::holds_alternative<document::LayerId>(session_.selection().primary)) {
        QWidget::mousePressEvent(event);
        return;
    }
    auto mapping = currentMapping();
    if (!mapping.has_value()) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (session_.beginPositionInteraction(*mapping).has_value()) {
        // Typed rejection (no selection, no resolvable/animated-without-a-key/driven position, or
        // an empty mapping): the drag simply never starts. No cursor/handle art communicates this
        // in v1 -- the gesture itself is the whole slice.
        QWidget::mousePressEvent(event);
        return;
    }
    dragActive_ = true;
    dragOrigin_ = event->position();
    activeMapping_ = mapping;
    setFocus(Qt::MouseFocusReason);
    previewController_.beginInteractiveScrub();
    event->accept();
}

void ViewerEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!dragActive_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!mappingStillValid()) {
        endDrag(false);
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Total displacement from the ORIGINAL press point, not from the previous move -- base value
    // plus TOTAL gesture displacement, never a chain of already-rounded intermediates (docs/
    // architecture/animation-and-time.md).
    const QPointF delta = event->position() - dragOrigin_;
    session_.updatePositionInteraction(delta.x(), delta.y());
    event->accept();
}

void ViewerEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragActive_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    endDrag(true);
    event->accept();
}

void ViewerEditor::keyPressEvent(QKeyEvent* event) {
    if (dragActive_ && event->key() == Qt::Key_Escape) {
        endDrag(false);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ViewerEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (dragActive_) {
        endDrag(false);
    }
}

} // namespace bloom::ui
