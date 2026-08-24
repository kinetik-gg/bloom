#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/image_types.hpp>

#include <QImage>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <limits>

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
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        updatePreviewAccessibility();
        update();
    });
    updatePreviewAccessibility();
}

void ViewerEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(15, 16, 18));

    const QRectF frame = QRectF(rect()).adjusted(28.0, 28.0, -28.0, -28.0);
    painter.fillRect(frame, QColor(39, 41, 45));
    painter.setPen(QPen(QColor(77, 80, 87), 1.0));
    painter.drawRect(frame.adjusted(0.0, 0.0, -1.0, -1.0));

    constexpr int gridStep = 32;
    painter.setPen(QPen(QColor(46, 48, 53), 1.0));
    for (qreal x = frame.left() + gridStep; x < frame.right(); x += gridStep) {
        painter.drawLine(QPointF(x, frame.top()), QPointF(x, frame.bottom()));
    }
    for (qreal y = frame.top() + gridStep; y < frame.bottom(); y += gridStep) {
        painter.drawLine(QPointF(frame.left(), y), QPointF(frame.right(), y));
    }

    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle displayedFrame = preview.frame;
    bool drewPixels = false;
    if (displayedFrame != nullptr) {
        const auto viewResult = displayedFrame->displayBuffer().view();
        if (viewResult && viewResult.value()->descriptor().has_value()) {
            const auto view = *viewResult.value();
            const auto descriptor = *view.descriptor();
            const auto extent = descriptor.displayWindow().extent();
            const auto& layout = descriptor.layout();
            if (extent.width() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                extent.height() <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
                layout.rowStrideBytes <=
                    static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
                const auto pixels = view.pixels();
                // displayedFrame owns the immutable bytes for this entire paint. The const-data
                // QImage constructor borrows them, so presentation does not copy or convert a
                // full frame on the UI thread.
                const QImage image(
                    reinterpret_cast<const uchar*>(pixels.data()), static_cast<int>(extent.width()),
                    static_cast<int>(extent.height()),
                    static_cast<qsizetype>(layout.rowStrideBytes), QImage::Format_RGBA8888);
                if (!image.isNull()) {
                    const QRectF displayRect =
                        fitDisplayRect(frame, extent, descriptor.pixelAspect());
                    drawCheckerboard(painter, displayRect);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(displayRect, image, QRectF(image.rect()));
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.setPen(QPen(QColor(104, 107, 114), 1.0));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(displayRect.adjusted(0.0, 0.0, -1.0, -1.0));
                    drewPixels = true;
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
    painter.drawText(QRectF(frame.left(), 2.0, frame.width(), 22.0),
                     Qt::AlignRight | Qt::AlignVCenter, tr("Reference display"));

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
    setAccessibleDescription(
        tr("%1. %2. Reference display.").arg(preview.message, frameDescription));
}

} // namespace bloom::ui
