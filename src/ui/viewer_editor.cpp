#include <bloom/ui/viewer_editor.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/window_status_bar.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QImage>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

// Wheel/menu zoom step (decision 2): each wheel notch or Zoom In/Out menu action multiplies the
// current effective zoom by this factor (clamped into ViewTransform's [kMinZoom, kMaxZoom]). The
// value itself moved to viewer_editor.hpp for task U4 (issue #123) so the Nodes canvas steps by
// exactly the same amount; nothing about this file's behavior changed.

// The Fit item plus the fixed percentage presets a real gesture never removes (decision 3). A
// zoom that lands off this ladder (e.g. from a wheel step) gets ONE additional trailing item --
// see refreshZoomDropdown()'s own comment on why that item is renamed in place rather than
// removed and re-added: kit::KDropdown has no item-removal API.
constexpr std::array<int, 5> kZoomPresets = {25, 50, 100, 200, 400};
constexpr std::array<const char*, 4> kResolutionNames = {"Auto", "Full", "Half", "Quarter"};
// Task VIEW-1. Channel order is the artist's reading order, not the buffer's byte order: the
// composite first, then the composite without alpha, then the three colour channels, then alpha.
constexpr std::array<const char*, 6> kChannelNames = {"RGBA", "RGB", "R", "G", "B", "Alpha"};
constexpr std::array<const char*, 4> kBackgroundNames = {"Solid", "Checkerboard", "Black", "White"};
constexpr auto kResolutionSetting = "viewer/resolution";
constexpr auto kBackgroundSetting = "viewer/background";
// Deliberately the SAME key the timeline's own View > Frames/Timecode pair already writes: the
// display format of time is one question with one answer, and a second key would let the ruler and
// the footer disagree about it. The two surfaces read it at construction and write it on change;
// they do not live-sync within one session, which is the honest limit of a settings key rather
// than a shared model.
constexpr auto kTimeFormatSetting = "timeline/time-format";
constexpr auto kLoopSetting = "playback/loop";

// Lays `controls` out left to right inside `bar`, each at its own size hint, vertically centred.
// A manual layout rather than a QHBoxLayout because the bar paints its own surface and hairline and
// because of the overflow rule below, which a box layout expresses by squeezing children instead.
//
// A control that does not fit ENTIRELY is hidden rather than clipped. A half-drawn dropdown or a
// truncated transport button is a control an artist can see and cannot use; a narrow Viewer simply
// offers fewer of them, and widening the panel brings them back. Nothing important is lost by that:
// the window status bar carries the state that has to stay on screen regardless.
void layoutFooterControls(const std::vector<QWidget*>& controls, const QRectF& bar) {
    const int right = static_cast<int>(bar.right()) - kit::px(kit::Spacing::S);
    int x = static_cast<int>(bar.left()) + kit::px(kit::Spacing::S);
    for (auto* control : controls) {
        if (control == nullptr) {
            continue;
        }
        const auto hint = control->sizeHint();
        if (x + hint.width() > right) {
            control->hide();
            continue;
        }
        const int y =
            static_cast<int>(bar.top()) + (static_cast<int>(bar.height()) - hint.height() + 1) / 2;
        control->setGeometry(x, y, hint.width(), hint.height());
        control->show();
        x += hint.width() + kit::px(kit::Spacing::XS);
    }
}

// The footer's own icon-only transport button: a plain QToolButton carrying a kit icon at the
// Control role, the same "QToolButton + kit::icon()" idiom the timeline transport used before this
// moved (and EditorArea's header chrome still uses). NOT kit::KButton, which is not a QToolButton
// and would break every existing findChild<QToolButton*>("playPauseButton") contract. An icon never
// replaces an accessible name (ADR 0010), so every call site sets a tooltip AND an accessible name.
QToolButton* makeTransportButton(const kit::IconId iconId, const QString& toolTip,
                                 const QString& accessibleName, const QString& objectName,
                                 QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(kit::icon(iconId, kit::IconRole::Control));
    const int box = kit::px(kit::iconSize(kit::IconRole::Control));
    button->setIconSize(QSize(box, box));
    button->setToolTip(toolTip);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    button->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    return button;
}

// The viewer-only channel remap (task VIEW-1). `source` is always the packed RGBA8 display buffer
// paintEvent() borrows, so the byte order below is exactly R, G, B, A. Rgba returns the borrow
// untouched -- no copy at all, which is the case that matters for playback; every other channel
// deep-copies once and is cached by the caller against the frame it was built from.
//
// This is presentation, not evaluation: nothing here reaches an exported frame, the RAM preview
// cache, or the display buffer the colour pipeline produced. It is applied AFTER display-referred
// conversion, on the bytes that are about to be blitted.
QImage remapChannels(const QImage& source, const ViewerChannel channel) {
    if (channel == ViewerChannel::Rgba) {
        return source;
    }
    QImage result = source.copy(); // detaches from the borrowed, immutable frame bytes
    for (int y = 0; y < result.height(); ++y) {
        uchar* row = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            uchar* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
            uchar grey = 0;
            switch (channel) {
            case ViewerChannel::Rgba:
                continue;
            case ViewerChannel::Rgb:
                pixel[3] = 255;
                continue;
            case ViewerChannel::Red:
                grey = pixel[0];
                break;
            case ViewerChannel::Green:
                grey = pixel[1];
                break;
            case ViewerChannel::Blue:
                grey = pixel[2];
                break;
            case ViewerChannel::Alpha:
                grey = pixel[3];
                break;
            }
            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;
            pixel[3] = 255;
        }
    }
    return result;
}

constexpr int kFixedZoomItemCount = 1 + static_cast<int>(kZoomPresets.size());

// What the Resolution control is ACTUALLY delivering. The dropdown says which policy is chosen;
// this says what that policy resolved to, which for Auto is the only place the effective factor is
// visible at all. Task VIEW-1 dropped the frame/time half of this string: the exact frame and time
// are their own click-to-edit control in the footer now (ViewerTimecodeReadout), so keeping them
// here as well would have put the same number on screen twice.
QString viewerResolutionText(const CompositionPreviewController& controller) {
    const auto policy = controller.settings().resolutionPolicy;
    const auto divisor = controller.resolutionDivisor();
    const QString factor = divisor == 4   ? QStringLiteral("¼")
                           : divisor == 2 ? QStringLiteral("½")
                                          : QStringLiteral("1");
    return policy == runtime::PreviewResolutionPolicy::Auto
               ? ViewerEditor::tr("Auto · %1").arg(factor)
               : ViewerEditor::tr(kResolutionNames[static_cast<std::size_t>(policy)]);
}

void drawCheckerboard(QPainter& painter, const QRectF& bounds) {
    // 22px pattern from the SurfaceRaised/Surface pair (decision 1): the WHOLE canvas surround, not
    // just an under-image alpha indicator -- the image is drawn on top of it with its own alpha
    // honored, so a transparent pixel already reads as "checkerboard showing through" with no
    // separate under-image pass needed. Task VIEW-1 made it one of four choices rather than the
    // only one; see drawCanvasBackground().
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

// The canvas surround (task VIEW-1), chosen by the footer's Background dropdown and persisted under
// "viewer/background". Black and White are literal, because that is exactly what an artist asks for
// when checking edges against a known value -- a token would be a different, softer colour and
// would defeat the purpose of the choice. Solid takes the application's own canvas Background token
// (see ViewerBackground's own comment on why it cannot be a per-composition colour yet).
void drawCanvasBackground(QPainter& painter, const QRectF& bounds,
                          const ViewerBackground background) {
    switch (background) {
    case ViewerBackground::Checkerboard:
        drawCheckerboard(painter, bounds);
        return;
    case ViewerBackground::Black:
        painter.fillRect(bounds, QColor(Qt::black));
        return;
    case ViewerBackground::White:
        painter.fillRect(bounds, QColor(Qt::white));
        return;
    case ViewerBackground::Solid:
        break;
    }
    painter.fillRect(bounds, kit::color(kit::Color::Background));
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
        const QRectF layerRect =
            displayRect
                .translated(static_cast<qreal>(token.offsetX), static_cast<qreal>(token.offsetY))
                .adjusted(-spread, -spread, spread, spread);
        painter.setBrush(layerColor);
        painter.drawRoundedRect(layerRect, 2.0, 2.0);
    }
    painter.restore();
}

// Paints the footer's surface and its top hairline, and nothing else. Task VIEW-1 moved the
// colour-state chip, the dropped-frame count and the cache progress to the window status bar
// (window_status_bar.hpp): they describe the application's state, not the frame in this panel, and
// they have to stay visible whether or not a Viewer is open.
void paintStatusBarSurface(QPainter& painter, const QRectF& bar) {
    painter.save();
    painter.fillRect(bar, kit::color(kit::Color::Surface));
    painter.setPen(QPen(kit::color(kit::Color::Border), 1.0));
    painter.drawLine(bar.topLeft(), bar.topRight());
    painter.restore();
}

// FORMAL AMENDMENT 1 (task C1), reshaped by task VIEW-1: the viewer's footer. It exists for the
// whole life of the ViewerEditor that built it -- as a child positioned inside the canvas's own
// bottom strip until takeFooterWidget() hands it to EditorArea, and as that caller's own footer
// slot afterwards. There is exactly ONE copy of the bar either way, which is the point: the
// pre-VIEW-1 arrangement painted the strip into ViewerEditor's paintEvent() AND into a separate
// widget, and the two had to be kept in agreement by hand.
class ViewerFooter final : public QWidget {
  public:
    explicit ViewerFooter(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("viewerFooter"));
        setAccessibleName(ViewerEditor::tr("Viewer controls"));
        setFixedHeight(kit::px(kit::Size::Control));
    }

    // The controls, left to right, in the order the artist reads them. Stored rather than
    // discovered from children() so the order is this file's decision and not Qt's creation order.
    void setControls(std::vector<QWidget*> controls) {
        controls_ = std::move(controls);
        layoutControls();
    }

    void layoutControls() {
        layoutFooterControls(controls_, QRectF(rect()));
        update();
    }

  protected:
    void resizeEvent(QResizeEvent*) override { layoutControls(); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintStatusBarSurface(painter, QRectF(rect()));
    }

  private:
    std::vector<QWidget*> controls_;
};

} // namespace

// The footer's frame/time readout (task VIEW-1): a monospaced label you click to type an exact
// frame number into, whose context menu chooses between a frame index and non-drop timecode.
//
// Not in the anonymous namespace because ViewerEditor holds one by pointer and viewer_editor.hpp
// therefore forward-declares it. No Q_OBJECT: it has no signals or slots of its own -- the seek is
// a callback supplied by its owner -- so it needs no moc pass.
class ViewerTimecodeReadout final : public QWidget {
  public:
    // Called with an exact, already-clamped frame index when the artist commits an edit. The
    // owner is what pauses playback and writes session time; this widget never touches either.
    using SeekFunction = std::function<void(std::uint64_t)>;

    ViewerTimecodeReadout(CompositionSession& session, SeekFunction seek, QWidget* parent)
        : QWidget(parent), session_(session), seek_(std::move(seek)) {
        setObjectName(QStringLiteral("viewerTimeReadout"));
        setAccessibleName(ViewerEditor::tr("Current frame and time"));
        setToolTip(ViewerEditor::tr("Click to type an exact frame number"));
        timecodeFormat_ =
            QSettings()
                .value(QLatin1StringView(kTimeFormatSetting), QStringLiteral("frames"))
                .toString() == QStringLiteral("timecode");

        stack_ = new QStackedLayout(this);
        stack_->setContentsMargins(0, 0, 0, 0);

        label_ = new QLabel(this);
        // Unchanged objectName: this is the same readout the timeline transport used to own, moved
        // rather than replaced (task VIEW-1's "they change parent, not identity").
        label_->setObjectName(QStringLiteral("timelineTimeReadout"));
        label_->setAccessibleName(ViewerEditor::tr("Current frame and time"));
        label_->setFont(kit::font(kit::TypeRole::Value));
        // The click that starts an edit belongs to this widget, so the label never eats it.
        label_->setAttribute(Qt::WA_TransparentForMouseEvents);

        editor_ = new QLineEdit(this);
        editor_->setObjectName(QStringLiteral("viewerTimeReadoutEditor"));
        editor_->setAccessibleName(ViewerEditor::tr("Go to frame"));
        editor_->setFont(kit::font(kit::TypeRole::Value));
        // A frame INDEX, always -- even while the label is showing timecode. Typing a frame number
        // is the one entry form that needs no parsing rules of its own, and it is what the task
        // asks for; the display format is a separate question the context menu answers.
        editor_->setValidator(new QIntValidator(0, std::numeric_limits<int>::max(), editor_));
        editor_->installEventFilter(this);

        stack_->addWidget(label_);
        stack_->addWidget(editor_);
        stack_->setCurrentWidget(label_);

        connect(editor_, &QLineEdit::returnPressed, this, [this] { commitEdit(); });

        // Built once and added to this widget's own action list rather than created inside
        // contextMenuEvent(): the menu is then assembled from actions that exist whether or not it
        // has ever been opened, which is what makes the format reachable by name -- to a test, and
        // to any future menu that wants to offer the same two choices.
        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        framesAction_ = new QAction(ViewerEditor::tr("Frames"), this);
        framesAction_->setObjectName(QStringLiteral("viewerFramesAction"));
        timecodeAction_ = new QAction(ViewerEditor::tr("Timecode"), this);
        timecodeAction_->setObjectName(QStringLiteral("viewerTimecodeAction"));
        timecodeAction_->setToolTip(
            ViewerEditor::tr("Non-drop HH:MM:SS:FF at the nominal frame rate"));
        for (auto* action : {framesAction_, timecodeAction_}) {
            action->setCheckable(true);
            group->addAction(action);
            addAction(action);
        }
        framesAction_->setChecked(!timecodeFormat_);
        timecodeAction_->setChecked(timecodeFormat_);
        connect(framesAction_, &QAction::triggered, this, [this] { setTimecodeFormat(false); });
        connect(timecodeAction_, &QAction::triggered, this, [this] { setTimecodeFormat(true); });

        refresh();
    }

    [[nodiscard]] bool timecodeFormat() const noexcept { return timecodeFormat_; }

    void setTimecodeFormat(const bool timecode) {
        if (timecodeFormat_ == timecode) {
            return;
        }
        timecodeFormat_ = timecode;
        framesAction_->setChecked(!timecode);
        timecodeAction_->setChecked(timecode);
        QSettings().setValue(QLatin1StringView(kTimeFormatSetting),
                             timecode ? QStringLiteral("timecode") : QStringLiteral("frames"));
        refresh();
    }

    // The exact text on screen, and the seam a test reads instead of grabbing pixels.
    [[nodiscard]] QString text() const { return label_->text(); }

    void refresh() {
        const auto time = session_.currentTime();
        const auto context = frameContextFor(session_);
        QString frameText = QStringLiteral("—");
        if (context.has_value()) {
            const auto nearest =
                nearestFrameIndexForTime(context->frameRate, context->duration, time);
            if (nearest.has_value()) {
                frameText = timecodeFormat_
                                ? formatTimelineFrameLabel(*nearest, context->frameRate, true)
                                : QString::number(*nearest);
            }
        }
        label_->setText(
            (timecodeFormat_ ? ViewerEditor::tr("TC %1 · %2") : ViewerEditor::tr("Frame %1 · %2"))
                .arg(frameText, formatExactSeconds(time)));
        label_->setToolTip(timecodeFormat_
                               ? ViewerEditor::tr("Non-drop timecode · exact composition time")
                               : ViewerEditor::tr("Frame index · exact composition time"));
    }

    [[nodiscard]] QSize sizeHint() const override {
        // A FIXED width from the widest string this readout can ever show, not the current text's
        // width: the text changes on every frame of playback, and a width that tracked it would
        // relayout the whole footer sixty times a second and make every control beside it twitch.
        const QFontMetrics metrics(kit::font(kit::TypeRole::Value));
        const int width = metrics.horizontalAdvance(QStringLiteral("TC 00:00:00:00 · 00000.000s")) +
                          kit::px(kit::Spacing::S);
        return {width, kit::px(kit::Size::ControlCompact)};
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            beginEdit();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        menu.addAction(framesAction_);
        menu.addAction(timecodeAction_);
        menu.exec(event->globalPos());
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == editor_) {
            if (event->type() == QEvent::FocusOut) {
                cancelEdit();
            } else if (event->type() == QEvent::KeyPress &&
                       static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                cancelEdit();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

  private:
    void beginEdit() {
        const auto context = frameContextFor(session_);
        if (!context.has_value()) {
            return;
        }
        const auto nearest =
            nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
        editor_->setText(QString::number(nearest.value_or(0)));
        stack_->setCurrentWidget(editor_);
        editor_->selectAll();
        editor_->setFocus(Qt::MouseFocusReason);
    }

    void cancelEdit() {
        if (stack_->currentWidget() == editor_) {
            stack_->setCurrentWidget(label_);
        }
    }

    void commitEdit() {
        const auto context = frameContextFor(session_);
        bool parsed = false;
        const auto typed = editor_->text().toULongLong(&parsed);
        cancelEdit();
        if (!parsed || !context.has_value() || !seek_) {
            // An unparseable entry reverts in silence: there is nothing to report that the
            // unchanged readout does not already say.
            return;
        }
        seek_(std::min<std::uint64_t>(typed, context->maxFrameIndexValue));
    }

    CompositionSession& session_;
    SeekFunction seek_;
    QStackedLayout* stack_ = nullptr;
    QLabel* label_ = nullptr;
    QLineEdit* editor_ = nullptr;
    QAction* framesAction_ = nullptr;
    QAction* timecodeAction_ = nullptr;
    bool timecodeFormat_ = false;
};

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
                             const render::ImageExtent extent,
                             const core::PixelAspectRatio pixelAspect, const QPointF screenPoint,
                             const double factor) noexcept {
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

// Builds the footer row and everything in it (task VIEW-1). Called once, from the constructor.
void ViewerEditor::buildFooter(RamPreviewController* const ramPreview) {
    auto* footer = new ViewerFooter(this);
    statusBarFooter_ = footer;

    // ---- Channel -------------------------------------------------------------------------------
    channelDropdown_ = new kit::KDropdown(footer);
    channelDropdown_->setObjectName("viewerChannelDropdown");
    channelDropdown_->setAccessibleName(tr("Channel"));
    channelDropdown_->setToolTip(tr("Which channels the viewer shows. Never affects an export."));
    channelDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    for (const auto* name : kChannelNames) {
        channelDropdown_->addItem(tr(name));
    }
    connect(channelDropdown_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (index < 0 || index >= static_cast<int>(kChannelNames.size())) {
            return;
        }
        setChannel(static_cast<ViewerChannel>(index));
    });

    // ---- Zoom ----------------------------------------------------------------------------------
    zoomDropdown_ = new kit::KDropdown(footer);
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

    // ---- Resolution ----------------------------------------------------------------------------
    resolutionDropdown_ = new kit::KDropdown(footer);
    resolutionDropdown_->setObjectName("viewerResolutionDropdown");
    resolutionDropdown_->setAccessibleName(tr("Resolution"));
    resolutionDropdown_->setToolTip(tr("Resolution"));
    resolutionDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    for (const auto* name : kResolutionNames) {
        resolutionDropdown_->addItem(tr(name));
    }
    const auto saved = QSettings().value(kResolutionSetting, QStringLiteral("Auto")).toString();
    int savedIndex = 0;
    for (std::size_t i = 0; i < kResolutionNames.size(); ++i) {
        if (saved == QLatin1StringView(kResolutionNames[i])) {
            savedIndex = static_cast<int>(i);
        }
    }
    resolutionDropdown_->setCurrentIndex(savedIndex);
    previewController_.setResolutionPolicy(
        static_cast<runtime::PreviewResolutionPolicy>(savedIndex));
    connect(resolutionDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0 || index >= static_cast<int>(kResolutionNames.size())) {
                    return;
                }
                QSettings().setValue(
                    kResolutionSetting,
                    QString::fromLatin1(kResolutionNames[static_cast<std::size_t>(index)]));
                previewController_.setResolutionPolicy(
                    static_cast<runtime::PreviewResolutionPolicy>(index));
            });
    connect(&previewController_, &CompositionPreviewController::resolutionChanged, this, [this] {
        const QSignalBlocker blocker(resolutionDropdown_);
        resolutionDropdown_->setCurrentIndex(
            static_cast<int>(previewController_.settings().resolutionPolicy));
        resolutionReadout_->setText(viewerResolutionText(previewController_));
    });
    // Part of the Resolution control, not a footer item of its own: what the chosen policy actually
    // resolved to. For Auto that effective factor is visible nowhere else.
    resolutionReadout_ = new QLabel(footer);
    resolutionReadout_->setObjectName("viewerResolutionReadout");
    resolutionReadout_->setAccessibleName(tr("Effective preview resolution"));
    resolutionReadout_->setFont(kit::font(kit::TypeRole::Value));
    resolutionReadout_->setText(viewerResolutionText(previewController_));

    // ---- Background ----------------------------------------------------------------------------
    backgroundDropdown_ = new kit::KDropdown(footer);
    backgroundDropdown_->setObjectName("viewerBackgroundDropdown");
    backgroundDropdown_->setAccessibleName(tr("Background"));
    backgroundDropdown_->setToolTip(
        tr("What the viewer paints behind the composition. Solid is the application's canvas "
           "colour; a composition carries no background colour of its own yet."));
    backgroundDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    for (const auto* name : kBackgroundNames) {
        backgroundDropdown_->addItem(tr(name));
    }
    const auto savedBackground =
        QSettings().value(kBackgroundSetting, QStringLiteral("Solid")).toString();
    int backgroundIndex = 0;
    for (std::size_t i = 0; i < kBackgroundNames.size(); ++i) {
        if (savedBackground == QLatin1StringView(kBackgroundNames[i])) {
            backgroundIndex = static_cast<int>(i);
        }
    }
    background_ = static_cast<ViewerBackground>(backgroundIndex);
    backgroundDropdown_->setCurrentIndex(backgroundIndex);
    connect(backgroundDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0 || index >= static_cast<int>(kBackgroundNames.size())) {
                    return;
                }
                setBackground(static_cast<ViewerBackground>(index));
            });

    // ---- Transport -----------------------------------------------------------------------------
    // Moved here from the timeline (task VIEW-1). Every objectName below is unchanged: these
    // controls changed parent, not identity, and the tests that reach them by name now look in the
    // viewer instead of the timeline.
    stepToStartButton_ =
        makeTransportButton(kit::IconId::CaretLeft, tr("Go to start (Home)"), tr("Go to start"),
                            QStringLiteral("viewerStepToStartButton"), footer);
    stepBackButton_ = makeTransportButton(kit::IconId::StepBack, tr("Step back one frame (Left)"),
                                          tr("Step back one frame"),
                                          QStringLiteral("timelineStepBackButton"), footer);
    playPauseButton_ =
        makeTransportButton(kit::IconId::Play, tr("Toggle playback (Space)"), tr("Toggle playback"),
                            QStringLiteral("playPauseButton"), footer);
    // playPauseButton_ MUST stay a QToolButton with its existing text()/isChecked() contract
    // (playback_controller_tests.cpp, composition_projection_test.cpp both read it by exactly that
    // type/objectName).
    playPauseButton_->setCheckable(true);
    stepForwardButton_ = makeTransportButton(
        kit::IconId::StepForward, tr("Step forward one frame (Right)"),
        tr("Step forward one frame"), QStringLiteral("timelineStepForwardButton"), footer);
    stepToEndButton_ =
        makeTransportButton(kit::IconId::CaretRight, tr("Go to end (End)"), tr("Go to end"),
                            QStringLiteral("viewerStepToEndButton"), footer);
    // The loop control is a real toggle now, with PlaybackController::setLooping() behind it. It
    // was a non-interactive glyph in the timeline for an honest reason -- there was no command to
    // turn looping off -- and task VIEW-1 added the command rather than the illusion of one.
    // Unchanged objectName, changed kind: same role, same name, a control instead of a label.
    loopButton_ = makeTransportButton(kit::IconId::Loop, tr("Loop playback"), tr("Loop playback"),
                                      QStringLiteral("timelineLoopIndicator"), footer);
    loopButton_->setCheckable(true);
    // RAM Preview (task PERF1, item 3). IconId::Sequence is the nearest honest glyph in the kit's
    // existing vocabulary -- a run of frames -- rather than a new vendored asset for one button;
    // the tooltip and accessible name carry the meaning, as iconography rules require of an
    // icon-only control.
    ramPreviewButton_ = makeTransportButton(
        kit::IconId::Sequence,
        tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"),
        tr("RAM preview"), QStringLiteral("timelineRamPreviewButton"), footer);
    ramPreviewButton_->setCheckable(true);
    ramPreviewButton_->setEnabled(ramPreview != nullptr);

    // ---- Frame / timecode readout --------------------------------------------------------------
    timeReadout_ = new ViewerTimecodeReadout(
        session_, [this](const std::uint64_t frameIndex) { seekToFrame(frameIndex); }, footer);

    footer->setControls({channelDropdown_, zoomDropdown_, resolutionDropdown_, resolutionReadout_,
                         backgroundDropdown_, stepToStartButton_, stepBackButton_, playPauseButton_,
                         stepForwardButton_, stepToEndButton_, loopButton_, ramPreviewButton_,
                         timeReadout_});
    layoutStatusBar();
}

ViewerEditor::ViewerEditor(CompositionSession& session,
                           CompositionPreviewController& previewController,
                           RamPreviewController* const ramPreview, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController),
      ramPreview_(ramPreview) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    setMinimumSize(220, 150 + kit::px(kit::Size::Control));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // StrongFocus lets a press-to-drag gesture also receive the Escape key that cancels it, and
    // lets the widget receive Space/Z/F without a prior click.
    setFocusPolicy(Qt::StrongFocus);

    playback_ = &previewController.playbackController();
    buildFooter(ramPreview);
    wireTransport();

    // Every one of these already repainted the status bar for free when it was part of this
    // widget's own paintEvent; FORMAL AMENDMENT 1 keeps that true once takeFooterWidget() moves it
    // out into its own widget by also nudging statusBarFooter_ (a no-op update() call until then,
    // since it starts null).
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::selectionChanged, this, [this] {
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    // New for the status bar's exact readout (decision 3): the original Viewer never needed
    // current-time updates before, since nothing it drew depended on session time.
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] {
        timeReadout_->refresh();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    // The readout also follows a composition switch, which resets session time to exact zero
    // (docs/architecture/animation-and-time.md, "Session Time And Scrubbing") -- so it never shows
    // the previous composition's stale frame for one repaint.
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { timeReadout_->refresh(); });
    // Task S5, item 3b: the dropped-frame readout follows the SAME refresh idiom as every other
    // footer element -- connect to whatever changes it, then update() this widget and the footer.
    connect(&previewController_, &CompositionPreviewController::droppedFrameCountChanged, this,
            [this] {
                update();
                if (statusBarFooter_ != nullptr) {
                    statusBarFooter_->update();
                }
            });
    // Task PERF1, item 3: the RAM preview progress readout, through that same idiom.
    connect(&previewController_, &CompositionPreviewController::ramPreviewProgressChanged, this,
            [this] {
                update();
                if (statusBarFooter_ != nullptr) {
                    statusBarFooter_->update();
                }
            });
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        updatePreviewAccessibility();
        resolutionReadout_->setText(viewerResolutionText(previewController_));
        // A newly delivered frame may carry a format/proxy/pixel-aspect/display-descriptor change
        // (docs/architecture/animation-and-time.md); a mid-drag mapping change cancels the gesture
        // rather than silently mis-mapping the rest of it.
        if (dragActive_ && !mappingStillValid()) {
            endDrag(false);
        }
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    updatePreviewResolution();
    updatePreviewAccessibility();
}

ViewerEditor::~ViewerEditor() { QObject::disconnect(focusConnection_); }

// Everything the transport needs that is not the construction of its buttons: the shared
// PlaybackController, the RAM preview command, the four frame-stepping QActions, and the
// arrow-key reconciliation that keeps those actions from swallowing another panel's navigation.
void ViewerEditor::wireTransport() {
    connect(playPauseButton_, &QToolButton::clicked, playback_, &PlaybackController::toggle);
    connect(playback_, &PlaybackController::stateChanged, this,
            &ViewerEditor::updatePlaybackButton);
    updatePlaybackButton(playback_->state());

    // Looping is a persisted transport preference, read once here and written on every toggle.
    // PlaybackController itself stays free of QSettings: it owns the behavior, not the preference.
    playback_->setLooping(QSettings().value(kLoopSetting, true).toBool());
    connect(loopButton_, &QToolButton::clicked, this, [this](const bool checked) {
        playback_->setLooping(checked);
        QSettings().setValue(kLoopSetting, checked);
    });
    connect(playback_, &PlaybackController::loopingChanged, this, &ViewerEditor::updateLoopButton);
    updateLoopButton();

    // RAM Preview's KEYS are not declared here. Ctrl+Shift+Space and the Escape that cancels a run
    // are application-wide commands owned by the Composition menu (main_window.cpp): one
    // Qt::WindowShortcut owner per sequence, or Qt reports an ambiguous overload and fires neither.
    // This button is the transport's own affordance for that same command, and it calls the same
    // RamPreviewController::toggle() the menu item calls -- never a synthesized key press.
    if (ramPreview_ != nullptr) {
        connect(ramPreviewButton_, &QToolButton::clicked, ramPreview_,
                &RamPreviewController::toggle);
        connect(ramPreview_, &RamPreviewController::stateChanged, this, [this] {
            updateRamPreviewButton();
            updatePlaybackButton(playback_->state());
        });
        updateRamPreviewButton();
    }

    // Frame-stepping shortcuts (issue #108, decisions 1/2), mirroring playPauseAction's own
    // WindowShortcut idiom exactly. Issue #120 (task U5) replaced PropertiesEditor's Position X/Y
    // QDoubleSpinBoxes with kit::KValueField, which has no line edit and does NOT accept
    // ShortcutOverride for Left/Right/Home/End -- so those keys typed while a Position field has
    // focus ALSO fire these actions. Still flagged rather than fixed here: the fix belongs to
    // whoever next owns kit::KValueField's key handling.
    const auto makeStepAction = [this](const QString& text, const QString& objectName,
                                       const QKeySequence& shortcut) {
        auto* action = new QAction(text, this);
        action->setObjectName(objectName);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        return action;
    };
    stepBackwardAction_ =
        makeStepAction(tr("Step Back One Frame"), QStringLiteral("stepBackwardAction"),
                       QKeySequence(Qt::Key_Left));
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });
    // The visible button triggers this SAME action -- one behavior, two entry points. Wired through
    // trigger() rather than QToolButton::setDefaultAction() so the button's own icon, tooltip and
    // objectName stay under this class's control instead of mirroring the action's text.
    connect(stepBackButton_, &QToolButton::clicked, stepBackwardAction_, &QAction::trigger);

    stepForwardAction_ =
        makeStepAction(tr("Step Forward One Frame"), QStringLiteral("stepForwardAction"),
                       QKeySequence(Qt::Key_Right));
    connect(stepForwardAction_, &QAction::triggered, this, [this] { stepFrame(1); });
    connect(stepForwardButton_, &QToolButton::clicked, stepForwardAction_, &QAction::trigger);

    stepToStartAction_ = makeStepAction(tr("Go To Start"), QStringLiteral("stepToStartAction"),
                                        QKeySequence(Qt::Key_Home));
    connect(stepToStartAction_, &QAction::triggered, this, &ViewerEditor::stepToStart);
    connect(stepToStartButton_, &QToolButton::clicked, stepToStartAction_, &QAction::trigger);

    stepToEndAction_ = makeStepAction(tr("Go To End"), QStringLiteral("stepToEndAction"),
                                      QKeySequence(Qt::Key_End));
    connect(stepToEndAction_, &QAction::triggered, this, &ViewerEditor::stepToEnd);
    connect(stepToEndButton_, &QToolButton::clicked, stepToEndAction_, &QAction::trigger);

    // Arrow-key conflict reconciliation, carried over from TimelineEditor, which owned these four
    // actions before task VIEW-1 moved the transport here. The timeline's layer stack consumes
    // Up/Down/Home/End for its OWN row navigation but, unlike a text-entry widget, does not claim
    // the ShortcutOverride event for them, so a same-key WindowShortcut action would silently
    // swallow that navigation. The frozen rule -- widget focus wins, the step action fires
    // otherwise -- is implemented by disabling these four actions outright while such a widget
    // holds keyboard focus: a disabled QAction never claims ShortcutOverride, so the key event
    // reaches the widget and its navigation runs unchanged.
    //
    // The Viewer must not know which panels exist, so the marker is a dynamic property
    // (kDefersTransportKeysProperty) the claiming widget sets on itself rather than a type check.
    focusConnection_ =
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            bool deferred = false;
            for (const QWidget* candidate = now; candidate != nullptr;
                 candidate = candidate->parentWidget()) {
                if (candidate->property(kDefersTransportKeysProperty).toBool()) {
                    deferred = true;
                    break;
                }
            }
            for (auto* action :
                 {stepBackwardAction_, stepForwardAction_, stepToStartAction_, stepToEndAction_}) {
                action->setEnabled(!deferred);
            }
            // The visible step buttons mirror their action's enabled state exactly, so the same
            // reconciliation is visible on the mouse affordance too rather than showing a clickable
            // button that would silently do nothing.
            stepBackButton_->setEnabled(!deferred);
            stepForwardButton_->setEnabled(!deferred);
            stepToStartButton_->setEnabled(!deferred);
            stepToEndButton_->setEnabled(!deferred);
        });
}

void ViewerEditor::updatePlaybackButton(const PlaybackState state) {
    const bool playing = state == PlaybackState::Playing;
    playPauseButton_->setChecked(playing);
    playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
    playPauseButton_->setIcon(
        kit::icon(playing ? kit::IconId::Pause : kit::IconId::Play, kit::IconRole::Control));
    // RAM Preview folded into the play button's CACHED STATE (task VIEW-1): the button says
    // whether the range it is about to play is already in the cache, which is the one thing an
    // artist wants to know before pressing it. The explicit RAM Preview action stays its own
    // button, because caching a range and starting playback are different commands.
    const bool cached = ramPreview_ != nullptr && !ramPreview_->isCaching() &&
                        ramPreview_->totalFrameCount() > 0 &&
                        ramPreview_->cachedFrameCount() >= ramPreview_->totalFrameCount();
    if (playing) {
        playPauseButton_->setToolTip(tr("Pause playback (Space)"));
    } else if (cached) {
        playPauseButton_->setToolTip(tr("Play cached range (Space)"));
    } else {
        playPauseButton_->setToolTip(tr("Play (Space)"));
    }
    playPauseButton_->setProperty("bloomPlaybackCached", cached);
}

void ViewerEditor::updateRamPreviewButton() {
    if (ramPreview_ == nullptr) {
        return;
    }
    const bool caching = ramPreview_->isCaching();
    ramPreviewButton_->setChecked(caching);
    ramPreviewButton_->setToolTip(
        caching ? tr("Cancel the RAM preview being cached (Esc)")
                : tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"));
}

void ViewerEditor::updateLoopButton() {
    const bool looping = playback_->isLooping();
    loopButton_->setChecked(looping);
    loopButton_->setToolTip(looping ? tr("Looping: playback wraps to the start of the work area")
                                    : tr("Not looping: playback stops on the last frame"));
    loopButton_->setAccessibleName(looping ? tr("Looping on") : tr("Looping off"));
}

void ViewerEditor::seekToFrame(const std::uint64_t frameIndex) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    // Stepping while playing pauses playback FIRST through PlaybackController's own public
    // transport API (design decision 1) -- composing with pause() explicitly here rather than
    // relying on handleCurrentTimeChanged()'s existing "any external setCurrentTime() while playing
    // pauses" side effect, so this call site is honest about what it does and the transport state
    // change is never a coincidental side effect of the time write below. Called unconditionally
    // (idempotent no-op if already Stopped), not only when the seek actually moves the playhead.
    playback_->pause();
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration,
                                              std::min(frameIndex, context->maxFrameIndexValue));
    if (targetTime.has_value()) {
        // A landing on the CURRENT exact time is a true no-op through
        // CompositionSession::setCurrentTime()'s own early-return-on-equal-time guard -- no
        // currentTimeChanged signal churn.
        (void)session_.setCurrentTime(*targetTime);
    }
}

void ViewerEditor::stepFrame(const int delta) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    // Left/Right move exactly one frame index from the nearest index to the CURRENT (possibly
    // subframe) time, clamped to [0, maxFrameIndex] (design decision 1). nearestFrameIndex()'s own
    // tie rule decides which frame a subframe time steps from, not this call site.
    const auto nearest =
        nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
    if (!nearest.has_value()) {
        return;
    }
    std::uint64_t target = *nearest;
    if (delta < 0) {
        target = target > 0 ? target - 1 : 0;
    } else {
        target = target < context->maxFrameIndexValue ? target + 1 : context->maxFrameIndexValue;
    }
    seekToFrame(target);
}

void ViewerEditor::stepToStart() { seekToFrame(0); }

void ViewerEditor::stepToEnd() {
    const auto context = frameContextFor(session_);
    if (context.has_value()) {
        seekToFrame(context->maxFrameIndexValue);
    }
}

void ViewerEditor::setChannel(const ViewerChannel channel) {
    if (channel_ == channel) {
        return;
    }
    channel_ = channel;
    // Drop the cached remap eagerly rather than on the next paint: a channel the artist has moved
    // away from should not keep a whole frame resident.
    channelView_ = QImage();
    channelViewFrame_.reset();
    update();
}

void ViewerEditor::setBackground(const ViewerBackground background) {
    if (background_ == background) {
        return;
    }
    background_ = background;
    QSettings().setValue(
        kBackgroundSetting,
        QString::fromLatin1(kBackgroundNames[static_cast<std::size_t>(background)]));
    update();
}

QWidget* ViewerEditor::takeFooterWidget() {
    // FORMAL AMENDMENT 1 (task C1): idempotent -- a second call (this ViewerEditor already gave
    // its footer away) returns nullptr rather than a dangling or duplicate widget. Task VIEW-1: the
    // footer is not BUILT here any more, only handed over; it has existed since construction.
    if (statusBarFooterTaken_) {
        return nullptr;
    }
    statusBarFooterTaken_ = true;
    statusBarFooter_->setParent(nullptr);
    // canvasRect() is now full-bleed (statusBarRect() returns empty) -- repaint immediately rather
    // than waiting for the next incidental update().
    updatePreviewResolution();
    update();
    return statusBarFooter_;
}

ViewTransform ViewerEditor::viewTransformForTest() const noexcept { return transform_; }

QString ViewerEditor::statusBarReadoutTextForTest() const {
    return viewerResolutionText(previewController_);
}

ViewerChannel ViewerEditor::channelForTest() const noexcept { return channel_; }

ViewerBackground ViewerEditor::backgroundForTest() const noexcept { return background_; }

QString ViewerEditor::timeReadoutTextForTest() const { return timeReadout_->text(); }

kit::KDropdown* ViewerEditor::zoomDropdownForTest() const noexcept { return zoomDropdown_; }

QRectF ViewerEditor::statusBarRect() const {
    // FORMAL AMENDMENT 1: once takeFooterWidget() has relocated the status bar to an externally
    // hosted footer widget, this widget's own rect no longer reserves any space for it at all.
    if (statusBarFooterTaken_) {
        return {};
    }
    const qreal barHeight = kit::px(kit::Size::Control);
    return QRectF(0.0, static_cast<qreal>(height()) - barHeight, static_cast<qreal>(width()),
                  barHeight);
}

QRectF ViewerEditor::canvasRect() const {
    const QRectF bar = statusBarRect();
    // Full-bleed (decision 1): no side or top inset at all, only the bottom strip the status bar
    // structurally requires -- that strip is a persistent control row, not "padding" -- and
    // (FORMAL AMENDMENT 1) not reserved at all once that row has moved into an external footer
    // widget, where bar.height() is already 0.
    return QRectF(rect()).adjusted(0.0, 0.0, 0.0, -bar.height());
}

void ViewerEditor::layoutStatusBar() {
    // FORMAL AMENDMENT 1: once the footer has been taken, its new owner positions it; this widget
    // no longer has a bar rect to place it in at all. Until then it fills the bottom strip
    // canvasRect() reserves, and the footer's own resizeEvent lays its controls out inside itself.
    if (statusBarFooterTaken_ || statusBarFooter_ == nullptr) {
        return;
    }
    statusBarFooter_->setGeometry(statusBarRect().toRect());
}

void ViewerEditor::refreshZoomDropdown() {
    updatePreviewResolution();
    if (resolutionReadout_ != nullptr) {
        resolutionReadout_->setText(viewerResolutionText(previewController_));
    }
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

void ViewerEditor::updatePreviewResolution() {
    const auto geometry = currentDisplayGeometry();
    if (!geometry.has_value()) {
        return;
    }
    const auto actual = actualPixelRect(canvasRect(), geometry->extent, geometry->pixelAspect);
    const auto displayed = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                      geometry->pixelAspect, transform_);
    if (actual.isEmpty() || displayed.isEmpty()) {
        return;
    }
    previewController_.setDisplayedCompositionScale(
        std::max(displayed.width() / actual.width(), displayed.height() / actual.height()) *
        devicePixelRatioF());
}

bool ViewerEditor::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (dragActive_) {
            endDrag(false);
        }
        updatePreviewResolution();
    }
    return handled;
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
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto format = composition->format();
    const auto extent = render::ImageExtent::create(format.width(), format.height());
    return DisplayGeometry{.extent = *extent.value(), .pixelAspect = format.pixelAspect()};
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
        drawCanvasBackground(painter, frame, background_);
        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.setPen(kit::color(kit::Color::Muted));
        painter.drawText(frame, Qt::AlignCenter, tr("Create a layer to begin"));
        return;
    }

    drawCanvasBackground(painter, frame, background_);

    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle displayedFrame = preview.frame;
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
                    // The viewer-only channel view (task VIEW-1). RGBA keeps the borrow above
                    // untouched; any other channel is remapped ONCE per (frame, channel) and held
                    // in channelView_, so a repaint -- or a playback tick that re-presents the same
                    // frame -- never re-walks the buffer. Nothing downstream of this paint sees it.
                    if (channel_ != ViewerChannel::Rgba &&
                        (channelViewFrame_ != displayedFrame || channelViewChannel_ != channel_)) {
                        channelView_ = remapChannels(image, channel_);
                        channelViewFrame_ = displayedFrame;
                        channelViewChannel_ = channel_;
                    }
                    const QImage& shownImage =
                        channel_ == ViewerChannel::Rgba ? image : channelView_;
                    const auto geometry = currentDisplayGeometry();
                    const QRectF displayRect =
                        geometry.has_value()
                            ? viewTransformedDisplayRect(frame, geometry->extent,
                                                         geometry->pixelAspect, transform_)
                            : QRectF{};
                    drawFrameShadow(painter, displayRect);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(displayRect, shownImage, QRectF(shownImage.rect()));
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.setPen(QPen(kit::color(kit::Color::BorderHover), 1.0));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(displayRect.adjusted(0.0, 0.0, -1.0, -1.0));
                    if (displayedFrame->hasProcessFrame()) {
                        const auto& format =
                            displayedFrame->processFrame()->identity().plan->format();
                        const auto toScreen = [&](const document::Vec2d point) {
                            return QPointF(
                                displayRect.left() + point.x * displayRect.width() /
                                                         static_cast<double>(format.width()),
                                displayRect.top() + point.y * displayRect.height() /
                                                        static_cast<double>(format.height()));
                        };
                        for (const auto& bounds : previewController_.selectedLayerBounds()) {
                            painter.setPen(QPen(kit::color(kit::Color::Accent), 1.0));
                            QPolygonF polygon;
                            for (const auto point : bounds.polygon)
                                polygon << toScreen(point);
                            painter.setBrush(Qt::NoBrush);
                            painter.drawPolygon(polygon);
                            painter.setBrush(kit::color(kit::Color::Accent));
                            painter.setPen(Qt::NoPen);
                            painter.drawEllipse(toScreen(bounds.anchor), 3.0, 3.0);
                        }
                    }
                }
            }
        }
    }

    // Readiness and diagnostics remain in the status bar. Selection geometry is derived from
    // the delivered process frame and painted in screen space above the composition.
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
    // Single source of wording with the window status bar's own chip (previewColorState()) -- the
    // accessible description and the visible chip can never drift apart, even though they are in
    // two different widgets now.
    const QString colorStateDescription = previewColorState(preview).text;
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
    const auto geometry = currentDisplayGeometry();
    if (!geometry.has_value()) {
        return std::nullopt;
    }
    const QRectF displayRect = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                          geometry->pixelAspect, transform_);
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

    } else {
        unsetCursor();
    }
}

void ViewerEditor::mousePressEvent(QMouseEvent* event) {
    if (!dragActive_ && !panActive_) {
        if (const auto geometry = currentDisplayGeometry();
            geometry.has_value() && (event->button() == Qt::MiddleButton)) {
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

        // Ctrl+0 fits and Ctrl+1 is actual size, in this canvas and in the node canvas alike (task
        // S1, item 8). F and Z are retired in both: the Adobe-standard pair is what an artist
        // arriving from another compositor reaches for, and a single-letter binding that far up the
        // alphabet is needed for tools. The other two ways to reach either are still the context
        // menu and the zoom dropdown (decision 2/4).
        if (event->modifiers() == Qt::ControlModifier) {
            if (event->key() == Qt::Key_1) {
                setZoomActualSize();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_0) {
                setZoomFit();
                event->accept();
                return;
            }
        }
    }
    QWidget::keyPressEvent(event);
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
    updatePreviewResolution();
}

void ViewerEditor::contextMenuEvent(QContextMenuEvent* event) {
    // Kit-styled via the application-wide QMenu stylesheet rule every other Bloom context/popup
    // menu already picks up (e.g. TimelineEditor's "Add Layer" menu,
    // composition_editor_support.cpp) -- no per-menu styling code needed here. Honest,
    // placeholder-free set only (decision 4): no RAM-preview/channel/quality slots, which do not
    // exist yet.
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
