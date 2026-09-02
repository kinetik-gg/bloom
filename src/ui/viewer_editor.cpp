#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QContextMenuEvent>
#include <QImage>
#include <QListView>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <variant>

namespace bloom::ui {
namespace {

// Wheel/menu zoom step (decision 2): each wheel notch or Zoom In/Out menu action multiplies the
// current effective zoom by this factor (clamped into ViewTransform's [kMinZoom, kMaxZoom]).
constexpr double kZoomStepFactor = 1.1;

// The Fit item plus the fixed percentage presets a real gesture never removes (decision 3). A
// zoom that lands off this ladder (e.g. from a wheel step) gets ONE additional trailing item --
// see refreshZoomDropdown()'s own comment on why that item is renamed in place rather than
// removed and re-added: kit::KDropdown has no item-removal API.
constexpr std::array<int, 5> kZoomPresets = {25, 50, 100, 200, 400};
constexpr int kFixedZoomItemCount = 1 + static_cast<int>(kZoomPresets.size());

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
        return kit::color(kit::Color::Accent);
    case PreviewActivity::Ready:
        return kit::color(kit::Color::Ok);
    case PreviewActivity::Unsupported:
        return kit::color(kit::Color::Warn);
    case PreviewActivity::Cancelled:
        return kit::color(kit::Color::Muted);
    case PreviewActivity::Failed:
        return kit::color(kit::Color::Error);
    }
    return kit::color(kit::Color::Muted);
}

// Ok/Warn/Error color-state chip (decision 3). The qualified/unqualified determination is
// ALWAYS driven by the currently-displayed frame's own isOcioQualified() bit -- never by
// `activity` -- so a retained qualified frame keeps reading as qualified even while a later,
// unrelated request is in flight (the "never a silent relabel" contract, unchanged from the
// original top-row label this replaces). PreviewActivity::Failed overrides to Error and shows the
// controller's own fail-closed diagnostic message (docs/architecture/color-management.md, "Any
// state other than Ready is fail-closed for qualified processing"): every Failed case this
// codebase currently produces reaches this state with an already-unqualified retained frame (or
// none at all), so this is additive, not a behavior change for the Ok/Warn cases the original
// label already covered. A theoretical "was qualified, then a later unrelated request failed"
// frame would still show the Error text rather than "Qualified" here -- a deliberate
// simplification, not exercised by any existing pipeline path.
struct ColorChipState final {
    QString text;
    kit::Color colorToken;
};

ColorChipState colorChipStateFor(const CompositionPreviewState& preview) {
    if (preview.activity == PreviewActivity::Failed) {
        return {preview.message.isEmpty() ? ViewerEditor::tr("Color state unavailable")
                                          : preview.message,
                kit::Color::Error};
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    if (bufferView.has_value() && bufferView->isOcioQualified) {
        return {ViewerEditor::tr("Qualified · Bloom Neutral"), kit::Color::Ok};
    }
    return {ViewerEditor::tr("Reference (unqualified)"), kit::Color::Warn};
}

// Formats an exact RationalTime as seconds with EXACTLY 3 truncated decimal digits (millisecond
// resolution), computed purely from the integer numerator/denominator -- never through
// RationalTime::toSeconds()'s binary64 conversion -- so the digits shown are always the value's
// true leading digits, never a rounded/binary64-approximated one (design decision 3: "no
// floating-point accumulation... a subframe time must display honestly").
//
// This is a DELIBERATE, documented duplicate of composition_editors.cpp's anonymous-namespace
// formatExactSeconds() (TimelineEditor::updateTimeReadout()'s own helper): task U3's fence
// forbids touching composition_editors.cpp (properties/timeline lane), and that function is
// neither exported nor movable without editing the fenced file it lives in. Per this task's own
// decision 3 ("reimplement the documented truncation rule and say so" when the timeline's
// formatting function cannot be reused/moved), the truncation RULE is reproduced verbatim from
// composition_editors.cpp; the frame-INDEX math below still goes through the shared, unfenced
// bloom::ui::nearestFrameIndexForTime() (timeline_frame_math.hpp) rather than being duplicated.
// Reported to the supervisor as a cross-lane duplication to fold into one shared helper later.
QString formatExactSecondsForViewer(const core::RationalTime time) {
    constexpr int kDecimalPlaces = 3;
    constexpr unsigned __int128 kScale = 1000;
    const std::int64_t numerator = time.numerator();
    const auto denominator = static_cast<unsigned __int128>(time.denominator());
    const bool negative = numerator < 0;
    const auto magnitude = negative
                               ? static_cast<unsigned __int128>(-static_cast<__int128>(numerator))
                               : static_cast<unsigned __int128>(numerator);
    const auto wholeSeconds = static_cast<qulonglong>(magnitude / denominator);
    const auto remainder = magnitude % denominator;
    const auto scaledFraction = static_cast<qulonglong>((remainder * kScale) / denominator);
    return QStringLiteral("%1%2.%3s")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(wholeSeconds)
        .arg(scaledFraction, kDecimalPlaces, 10, QChar('0'));
}

// "Frame %1 · %2" mirrors TimelineEditor::updateTimeReadout()'s exact display shape
// (composition_editors.cpp) so the Viewer's readout and the Timeline's readout never disagree in
// format, only in which fenced/unfenced pieces produce the two halves (see
// formatExactSecondsForViewer()'s comment above).
QString exactFrameAndTimecodeText(const CompositionSession& session) {
    const auto* composition = session.composition();
    const auto time = session.currentTime();
    QString frameText = QStringLiteral("—");
    if (composition != nullptr) {
        const auto frameRate = composition->format().frameRate();
        const auto duration = composition->duration();
        const auto nearest = nearestFrameIndexForTime(frameRate, duration, time);
        if (nearest.has_value()) {
            frameText = QString::number(*nearest);
        }
    }
    return ViewerEditor::tr("Frame %1 · %2").arg(frameText, formatExactSecondsForViewer(time));
}

void drawCheckerboard(QPainter& painter, const QRectF& bounds) {
    // 22px pattern from the SurfaceRaised/Surface pair (decision 1): this is now the WHOLE canvas
    // surround, not just an under-image alpha indicator -- the image is drawn on top of it with its
    // own alpha honored, so a transparent pixel already reads as "checkerboard showing through"
    // with no separate under-image pass needed.
    constexpr qreal tileSize = 22.0;
    painter.save();
    painter.setClipRect(bounds);
    painter.fillRect(bounds, kit::color(kit::Color::Surface));
    const int columns = static_cast<int>(bounds.width() / tileSize) + 2;
    const int rows = static_cast<int>(bounds.height() / tileSize) + 2;
    for (int row = 0; row < rows; ++row) {
        for (int column = row % 2; column < columns; column += 2) {
            painter.fillRect(QRectF(bounds.left() + column * tileSize,
                                    bounds.top() + row * tileSize, tileSize, tileSize),
                             kit::color(kit::Color::SurfaceRaised));
        }
    }
    painter.restore();
}

// Approximates Elevation::Popup's token shadow (kit::shadow()) as a stack of expanding,
// decreasingly-opaque rounded rects behind `displayRect`. kit::applyElevation() is the real
// mechanism (a QGraphicsDropShadowEffect attached to a widget) but that requires the shadowed
// content to BE a widget; the composed frame here is one QImage blit inside ViewerEditor's own
// paintEvent, not a child widget, so it cannot host a graphics effect. This still consumes the
// token's own offset/blur/color -- no raw literal -- it just composites the blur by hand.
void drawFrameShadow(QPainter& painter, const QRectF& displayRect) {
    const kit::Shadow token = kit::shadow(kit::Elevation::Popup);
    if (token.isFlat() || displayRect.isEmpty()) {
        return;
    }
    constexpr int kLayers = 4;
    painter.save();
    painter.setPen(Qt::NoPen);
    for (int layer = kLayers; layer >= 1; --layer) {
        const qreal t = static_cast<qreal>(layer) / static_cast<qreal>(kLayers);
        QColor layerColor = token.color;
        layerColor.setAlphaF(static_cast<float>(layerColor.alphaF() / kLayers));
        const qreal spread = token.blurRadius * t;
        const QRectF layerRect = displayRect
                                     .translated(static_cast<qreal>(token.offsetX),
                                                 static_cast<qreal>(token.offsetY))
                                     .adjusted(-spread, -spread, spread, spread);
        painter.setBrush(layerColor);
        painter.drawRoundedRect(layerRect, 2.0, 2.0);
    }
    painter.restore();
}

// Fills `chipRect` exactly with a rounded, token-colored pill carrying `text` -- the shared
// low-level primitive behind both the canvas activity chip and the status bar's zoom-independent
// color-state chip (drawStatusChip() below computes ITS OWN chipRect and delegates here).
void paintChip(QPainter& painter, const QRectF& chipRect, const QString& text, const QColor color) {
    if (chipRect.isEmpty()) {
        return;
    }
    const QString elided = painter.fontMetrics().elidedText(
        text, Qt::ElideRight, static_cast<int>(std::max<qreal>(0.0, chipRect.width() - 16.0)));
    painter.setPen(QPen(color.lighter(125), 1.0));
    painter.setBrush(kit::withOpacity(color, 0.855));
    painter.drawRoundedRect(chipRect, 4.0, 4.0);
    painter.setPen(kit::color(kit::Color::Foreground));
    painter.drawText(chipRect.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                     elided);
}

void drawStatusChip(QPainter& painter, const QRectF& available, const QString& text,
                    const QColor color, const Qt::Alignment horizontalAlignment) {
    constexpr qreal chipHeight = 24.0;
    const qreal maximumWidth = std::max<qreal>(0.0, available.width() - 24.0);
    const QString elided = painter.fontMetrics().elidedText(
        text, Qt::ElideRight, static_cast<int>(std::max<qreal>(0.0, maximumWidth - 16.0)));
    const qreal width =
        std::min<qreal>(maximumWidth, painter.fontMetrics().horizontalAdvance(elided) + 16.0);
    const qreal left = horizontalAlignment.testFlag(Qt::AlignRight)
                           ? available.right() - width - 12.0
                           : available.left() + 12.0;
    paintChip(painter, QRectF(left, available.top() + 10.0, width, chipHeight), text, color);
}

void drawDiagnosticBanner(QPainter& painter, const QRectF& available, const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    const QRectF banner =
        available.adjusted(24.0, available.height() * 0.38, -24.0, -available.height() * 0.38);
    painter.setPen(QPen(kit::color(kit::Color::BorderHover), 1.0));
    painter.setBrush(kit::withOpacity(kit::color(kit::Color::Surface), 0.88));
    painter.drawRoundedRect(banner, 5.0, 5.0);
    painter.setPen(kit::color(kit::Color::Foreground));
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

QRectF actualPixelRect(const QRectF& available, const render::ImageExtent extent,
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

    const auto width = static_cast<qreal>(displayWidth);
    const auto height = static_cast<qreal>(displayHeight);
    return QRectF(available.center().x() - width / 2.0, available.center().y() - height / 2.0,
                 width, height);
}

QRectF viewTransformedDisplayRect(const QRectF& available, const render::ImageExtent extent,
                                  const core::PixelAspectRatio pixelAspect,
                                  const ViewTransform& transform) noexcept {
    if (transform.fitToWindow) {
        return fitDisplayRect(available, extent, pixelAspect);
    }
    const QRectF actual = actualPixelRect(available, extent, pixelAspect);
    if (actual.isEmpty()) {
        return {};
    }
    const qreal zoom = std::clamp(transform.zoom, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    const qreal width = actual.width() * zoom;
    const qreal height = actual.height() * zoom;
    const QPointF centeredTopLeft(available.center().x() - width / 2.0,
                                  available.center().y() - height / 2.0);
    return QRectF(centeredTopLeft + transform.pan, QSizeF(width, height));
}

ViewTransform zoomAboutPoint(const ViewTransform& transform, const QRectF& available,
                            const render::ImageExtent extent, const core::PixelAspectRatio pixelAspect,
                            const QPointF screenPoint, const double factor) noexcept {
    const QRectF actual = actualPixelRect(available, extent, pixelAspect);
    const QRectF current = viewTransformedDisplayRect(available, extent, pixelAspect, transform);
    if (actual.isEmpty() || current.isEmpty() || !(actual.width() > 0.0) ||
        !(actual.height() > 0.0) || !(factor > 0.0)) {
        return transform;
    }

    const qreal currentZoom = current.width() / actual.width();
    const qreal newZoom =
        std::clamp(currentZoom * factor, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    // Fraction of the CURRENT display rectangle the cursor sits at -- fixed across the step by
    // construction below (the zoom-about-cursor invariant this function exists to guarantee).
    const qreal fractionX = (screenPoint.x() - current.left()) / current.width();
    const qreal fractionY = (screenPoint.y() - current.top()) / current.height();
    const qreal newWidth = actual.width() * newZoom;
    const qreal newHeight = actual.height() * newZoom;
    const QPointF newTopLeft(screenPoint.x() - fractionX * newWidth,
                             screenPoint.y() - fractionY * newHeight);
    const QPointF centeredTopLeft(available.center().x() - newWidth / 2.0,
                                  available.center().y() - newHeight / 2.0);
    return ViewTransform{
        .fitToWindow = false, .zoom = newZoom, .pan = newTopLeft - centeredTopLeft};
}

ViewerEditor::ViewerEditor(CompositionSession& session,
                           CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    setMinimumSize(220, 150 + kit::px(kit::Size::Control));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // StrongFocus lets a press-to-drag gesture also receive the Escape key that cancels it, and
    // lets the widget receive Space/Z/F without a prior click.
    setFocusPolicy(Qt::StrongFocus);

    zoomDropdown_ = new kit::KDropdown(this);
    zoomDropdown_->setObjectName("viewerZoomDropdown");
    zoomDropdown_->setAccessibleName(tr("Zoom"));
    zoomDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    zoomDropdown_->addItem(tr("Fit"), 0);
    for (const int percent : kZoomPresets) {
        zoomDropdown_->addItem(tr("%1%").arg(percent), percent);
    }
    connect(zoomDropdown_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (index <= 0) {
            setZoomFit();
            return;
        }
        setZoomPercent(zoomDropdown_->itemData(index).toInt());
    });
    layoutStatusBar();

    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&ViewerEditor::update));
    // New for the status bar's exact readout (decision 3): the original Viewer never needed
    // current-time updates before, since nothing it drew depended on session time.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
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

ViewTransform ViewerEditor::viewTransformForTest() const noexcept { return transform_; }

QString ViewerEditor::statusBarReadoutTextForTest() const {
    return exactFrameAndTimecodeText(session_);
}

QString ViewerEditor::statusBarColorChipTextForTest() const {
    return colorChipStateFor(previewController_.state()).text;
}

kit::KDropdown* ViewerEditor::zoomDropdownForTest() const noexcept { return zoomDropdown_; }

QRectF ViewerEditor::statusBarRect() const {
    const qreal barHeight = kit::px(kit::Size::Control);
    return QRectF(0.0, static_cast<qreal>(height()) - barHeight, static_cast<qreal>(width()),
                 barHeight);
}

QRectF ViewerEditor::canvasRect() const {
    const QRectF bar = statusBarRect();
    // Full-bleed (decision 1): no side or top inset at all, only the bottom strip the status bar
    // structurally requires -- that strip is a persistent control row, not "padding".
    return QRectF(rect()).adjusted(0.0, 0.0, 0.0, -bar.height());
}

void ViewerEditor::layoutStatusBar() {
    if (zoomDropdown_ == nullptr) {
        return;
    }
    const QRectF bar = statusBarRect();
    const QSize hint = zoomDropdown_->sizeHint();
    const int x = static_cast<int>(bar.left()) + kit::px(kit::Spacing::S);
    const int y = static_cast<int>(bar.top()) +
                 (static_cast<int>(bar.height()) - hint.height() + 1) / 2;
    zoomDropdown_->setGeometry(x, y, hint.width(), hint.height());
}

void ViewerEditor::refreshZoomDropdown() {
    if (zoomDropdown_ == nullptr) {
        return;
    }
    int targetIndex = 0; // "Fit"
    if (!transform_.fitToWindow) {
        const int percent = static_cast<int>(std::lround(transform_.zoom * 100.0));
        int presetIndex = -1;
        for (std::size_t i = 0; i < kZoomPresets.size(); ++i) {
            if (kZoomPresets[i] == percent) {
                presetIndex = static_cast<int>(i) + 1;
                break;
            }
        }
        if (presetIndex >= 0) {
            targetIndex = presetIndex;
        } else {
            // A zoom off the fixed ladder (e.g. from a wheel step) gets one trailing "current
            // custom value" item (decision 3). kit::KDropdown has no removeItem()/setItemText()
            // (src/ui/include/bloom/ui/kit/dropdown.hpp) -- addItem() only appends and itemText()
            // is read-only -- so a REPEATED custom zoom renames that one trailing item in place by
            // writing through the model kit::KDropdown itself uses (popupView()->model() is the
            // same QStandardItemModel addItem()/itemText() read/write, exposed publicly for
            // exactly this kind of read -- see dropdown.cpp) rather than growing a new item per
            // step or leaving a stale value on screen. The FIRST custom zoom in a session still
            // appends. Reported as a kit API gap (a real setItemText()/removeItem() would replace
            // this workaround) rather than worked around inside kit itself, which is out of this
            // task's fence.
            const QString label = tr("%1%").arg(percent);
            if (zoomDropdown_->count() > kFixedZoomItemCount) {
                auto* model = zoomDropdown_->popupView()->model();
                model->setData(model->index(kFixedZoomItemCount, 0), label, Qt::DisplayRole);
            } else {
                zoomDropdown_->addItem(label, percent);
            }
            targetIndex = kFixedZoomItemCount;
        }
    }
    if (zoomDropdown_->currentIndex() != targetIndex) {
        const QSignalBlocker blocker(zoomDropdown_);
        zoomDropdown_->setCurrentIndex(targetIndex);
    }
    zoomDropdown_->update();
}

void ViewerEditor::setZoomFit() {
    transform_ = ViewTransform{};
    refreshZoomDropdown();
    update();
}

void ViewerEditor::setZoomActualSize() { setZoomPercent(100); }

void ViewerEditor::setZoomPercent(const int percent) {
    transform_ = ViewTransform{.fitToWindow = false, .zoom = percent / 100.0, .pan = {0.0, 0.0}};
    refreshZoomDropdown();
    update();
}

std::optional<ViewerEditor::DisplayGeometry> ViewerEditor::currentDisplayGeometry() const {
    const auto& preview = previewController_.state();
    if (preview.frame == nullptr) {
        return std::nullopt;
    }
    const auto bufferView = preview.frame->displayBufferView();
    if (!bufferView.has_value()) {
        return std::nullopt;
    }
    return DisplayGeometry{.extent = bufferView->displayWindow.extent(),
                           .pixelAspect = bufferView->pixelAspect};
}

void ViewerEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Background));

    const QRectF frame = canvasRect();
    const auto* composition = session_.composition();

    if (composition == nullptr) {
        // Honest empty state (decision 5): no evaluation warnings, no busywork -- a quiet,
        // product-neutral invitation. Muted ink, Ui type (Value/Geist Mono is reserved for
        // numeric/timecode surfaces, not prose -- kit/tokens.hpp).
        drawCheckerboard(painter, frame);
        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.setPen(kit::color(kit::Color::Muted));
        painter.drawText(frame, Qt::AlignCenter, tr("Create a layer to begin"));
        paintStatusBarSurface(painter);
        return;
    }

    drawCheckerboard(painter, frame);

    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle displayedFrame = preview.frame;
    bool drewPixels = false;
    if (displayedFrame != nullptr) {
        // displayBufferView() normalizes both display-product alternatives (reference and
        // qualified) to the same packed-RGBA8 shape -- the viewer draws pixels identically either
        // way; isOcioQualified is only ever read for the status bar's color-state chip, never to
        // change how pixels are drawn.
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
                    const QRectF displayRect = viewTransformedDisplayRect(
                        frame, extent, bufferView->pixelAspect, transform_);
                    drawFrameShadow(painter, displayRect);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(displayRect, image, QRectF(image.rect()));
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.setPen(QPen(kit::color(kit::Color::BorderHover), 1.0));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(displayRect.adjusted(0.0, 0.0, -1.0, -1.0));
                    drewPixels = true;
                }
            }
        }
    }

    // Evaluation-pending/activity chip: always the subtle chip, never a banner (decision 5) --
    // unchanged from before except that the redundant large centered duplicate of this same text
    // (previously drawn whenever no pixels were available yet) is gone.
    drawStatusChip(painter, frame, previewStatusText(preview), previewStatusColor(preview.activity),
                   Qt::AlignLeft);
    // Failure states keep their existing typed surfacing (already token-driven) when stale pixels
    // are still being shown alongside them.
    if (drewPixels && preview.activity != PreviewActivity::Ready &&
        preview.activity != PreviewActivity::Rendering) {
        drawDiagnosticBanner(painter, frame, preview.message);
    }

    if (session_.selection().contextualLayer.has_value()) {
        const QString name = layerName(*composition, *session_.selection().contextualLayer);
        const QRectF selectionStatus(frame.left() + 12.0, frame.bottom() - 38.0,
                                     frame.width() - 24.0, 26.0);
        painter.setPen(QPen(kit::color(kit::Color::Accent), 1.0));
        painter.setBrush(kit::color(kit::Color::SurfaceRaised));
        painter.drawRoundedRect(selectionStatus, 4.0, 4.0);
        painter.setPen(kit::color(kit::Color::Foreground));
        const QString status = tr("Selected: %1 · evaluated bounds unavailable").arg(name);
        painter.drawText(
            selectionStatus.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(status, Qt::ElideRight,
                                             static_cast<int>(selectionStatus.width() - 16.0)));
    }

    paintStatusBarSurface(painter);
}

void ViewerEditor::paintStatusBarSurface(QPainter& painter) {
    const QRectF bar = statusBarRect();
    painter.save();
    painter.fillRect(bar, kit::color(kit::Color::Surface));
    painter.setPen(QPen(kit::color(kit::Color::Border), 1.0));
    painter.drawLine(bar.topLeft(), bar.topRight());

    // Right: the color-state chip (decision 3) -- the contract-preserved qualified/unqualified
    // indicator this whole status bar exists partly to relocate. Computed fresh every paint from
    // live preview state, exactly like the top-row label it replaces, so it is never a step behind
    // or a silent relabel.
    painter.setFont(kit::font(kit::TypeRole::Ui));
    const auto chipState = colorChipStateFor(previewController_.state());
    const qreal chipHeight = std::max<qreal>(16.0, bar.height() - 6.0);
    const qreal chipTop = bar.top() + (bar.height() - chipHeight) / 2.0;
    const qreal chipLeftBound =
        zoomDropdown_ != nullptr
            ? static_cast<qreal>(zoomDropdown_->geometry().right()) + kit::px(kit::Spacing::L)
            : bar.left();
    const qreal maxChipWidth = std::max<qreal>(0.0, bar.width() * 0.4);
    const QString elidedChipText = painter.fontMetrics().elidedText(
        chipState.text, Qt::ElideRight, static_cast<int>(std::max<qreal>(0.0, maxChipWidth - 16.0)));
    const qreal chipWidth = std::min<qreal>(
        maxChipWidth, painter.fontMetrics().horizontalAdvance(elidedChipText) + 16.0);
    const qreal chipLeft =
        std::max<qreal>(chipLeftBound, bar.right() - chipWidth - kit::px(kit::Spacing::S));
    const QRectF chipRect(chipLeft, chipTop, std::max<qreal>(0.0, bar.right() - chipLeft - kit::px(kit::Spacing::S)),
                          chipHeight);
    paintChip(painter, chipRect, chipState.text, kit::color(chipState.colorToken));

    // Center: exact frame + timecode readout, Geist Mono (kit::TypeRole::Value is the monospaced
    // role every numeric/timecode surface uses -- kit/tokens.hpp). Occupies whatever room is left
    // between the zoom dropdown and the color chip.
    painter.setFont(kit::font(kit::TypeRole::Value));
    painter.setPen(kit::color(kit::Color::Foreground));
    const qreal centerRight = chipRect.left() - kit::px(kit::Spacing::S);
    const QRectF centerRect(chipLeftBound, bar.top(), std::max<qreal>(0.0, centerRight - chipLeftBound),
                            bar.height());
    painter.drawText(centerRect, Qt::AlignCenter, exactFrameAndTimecodeText(session_));

    painter.restore();
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
    // Single source of wording with the painted status bar chip (colorChipStateFor()) -- the
    // accessible description and the visible chip can never drift apart.
    const QString colorStateDescription = colorChipStateFor(preview).text;
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
    // THE SEAM (task U3, decision 2): the frozen mapping rectangle used to be ALWAYS
    // fitDisplayRect() -- the fit-to-window rectangle, regardless of any zoom/pan. It is now
    // viewTransformedDisplayRect(), which composes the SAME fit rectangle when transform_ is in
    // Fit mode, or the actively zoomed/panned rectangle otherwise -- so a drag begun at zoom 200%
    // and a pan offset maps screen deltas against the geometry the user actually SEES, and lands
    // exactly under the cursor. Freeze semantics are unchanged: this is still computed once here,
    // handed to CompositionSession::beginPositionInteraction(), and frozen there for the gesture's
    // duration; PositionInteractionMapping's own equality (already comparing displayRect) is what
    // makes mappingStillValid() correctly invalidate a gesture if transform_ changes mid-drag, with
    // zero additional invalidation code needed (mousePressEvent()/wheelEvent() additionally refuse
    // to start a NEW zoom/pan while dragActive_, so this only matters as a defensive backstop).
    const QRectF displayRect = viewTransformedDisplayRect(
        canvasRect(), descriptor.displayWindow().extent(), descriptor.pixelAspect(), transform_);
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

void ViewerEditor::beginPan(const Qt::MouseButton button, const QPointF screenPoint,
                            const DisplayGeometry& geometry) {
    panActive_ = true;
    panButton_ = button;
    panOrigin_ = screenPoint;
    const QRectF frame = canvasRect();
    if (transform_.fitToWindow) {
        // Materializes the Fit-implied zoom into an equivalent Custom transform before panning:
        // Fit recomputes its rectangle from `frame` every call and has no stored zoom/pan to
        // accumulate onto, so panning while Fit means "start being Custom, at the zoom Fit
        // currently shows, with zero pan" -- an exact reproduction of the same rectangle (see
        // viewTransformedDisplayRect()'s own centering, identical to fitDisplayRect()'s).
        const QRectF fitted = fitDisplayRect(frame, geometry.extent, geometry.pixelAspect);
        const QRectF actual = actualPixelRect(frame, geometry.extent, geometry.pixelAspect);
        const double zoom = actual.width() > 0.0 ? fitted.width() / actual.width() : 1.0;
        panBaseTransform_ = ViewTransform{.fitToWindow = false, .zoom = zoom, .pan = {0.0, 0.0}};
    } else {
        panBaseTransform_ = transform_;
    }
    setFocus(Qt::MouseFocusReason);
    updatePanCursor();
}

void ViewerEditor::updatePanCursor() {
    if (panActive_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (spaceHeld_) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
}

void ViewerEditor::mousePressEvent(QMouseEvent* event) {
    if (!dragActive_ && !panActive_) {
        if (const auto geometry = currentDisplayGeometry();
            geometry.has_value() &&
            (event->button() == Qt::MiddleButton ||
             (event->button() == Qt::LeftButton && spaceHeld_))) {
            beginPan(event->button(), event->position(), *geometry);
            event->accept();
            return;
        }
    }

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
    if (panActive_) {
        transform_ = panBaseTransform_;
        transform_.pan += (event->position() - panOrigin_);
        refreshZoomDropdown();
        update();
        event->accept();
        return;
    }
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
    if (panActive_ && event->button() == panButton_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        updatePanCursor();
        event->accept();
        return;
    }
    if (!dragActive_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    endDrag(true);
    event->accept();
}

void ViewerEditor::wheelEvent(QWheelEvent* event) {
    if (dragActive_ || panActive_) {
        event->ignore();
        return;
    }
    const auto geometry = currentDisplayGeometry();
    const int notches = event->angleDelta().y() / 120;
    if (!geometry.has_value() || notches == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const double factor = std::pow(kZoomStepFactor, notches);
    transform_ = zoomAboutPoint(transform_, canvasRect(), geometry->extent, geometry->pixelAspect,
                                event->position(), factor);
    refreshZoomDropdown();
    update();
    event->accept();
}

void ViewerEditor::keyPressEvent(QKeyEvent* event) {
    if (dragActive_ && event->key() == Qt::Key_Escape) {
        endDrag(false);
        event->accept();
        return;
    }
    if (!dragActive_ && !panActive_) {
        if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
            spaceHeld_ = true;
            updatePanCursor();
            event->accept();
            return;
        }
        // Two ways to reach 100%: this key, or the "100%" context-menu/zoom-dropdown item
        // (decision 2/4).
        if (event->key() == Qt::Key_Z) {
            setZoomActualSize();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_F) {
            setZoomFit();
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void ViewerEditor::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
        spaceHeld_ = false;
        updatePanCursor();
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void ViewerEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (dragActive_) {
        endDrag(false);
    }
    if (panActive_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        updatePanCursor();
    }
    layoutStatusBar();
}

void ViewerEditor::contextMenuEvent(QContextMenuEvent* event) {
    // Kit-styled via the application-wide QMenu stylesheet rule every other Bloom context/popup
    // menu already picks up (e.g. TimelineEditor's "Add Layer" menu, composition_editors.cpp) --
    // no per-menu styling code needed here. Honest, placeholder-free set only (decision 4): no
    // RAM-preview/channel/quality slots, which do not exist yet.
    QMenu menu(this);
    QAction* fitAction = menu.addAction(tr("Fit"));
    QAction* actualSizeAction = menu.addAction(tr("100%"));
    menu.addSeparator();
    QAction* zoomInAction = menu.addAction(tr("Zoom In"));
    QAction* zoomOutAction = menu.addAction(tr("Zoom Out"));
    const bool canZoom = currentDisplayGeometry().has_value();
    zoomInAction->setEnabled(canZoom);
    zoomOutAction->setEnabled(canZoom);
    connect(fitAction, &QAction::triggered, this, &ViewerEditor::setZoomFit);
    connect(actualSizeAction, &QAction::triggered, this, &ViewerEditor::setZoomActualSize);
    connect(zoomInAction, &QAction::triggered, this, [this] {
        if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
            const QRectF frame = canvasRect();
            transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                        frame.center(), kZoomStepFactor);
            refreshZoomDropdown();
            update();
        }
    });
    connect(zoomOutAction, &QAction::triggered, this, [this] {
        if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
            const QRectF frame = canvasRect();
            transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                        frame.center(), 1.0 / kZoomStepFactor);
            refreshZoomDropdown();
            update();
        }
    });
    menu.exec(event->globalPos());
}

} // namespace bloom::ui
