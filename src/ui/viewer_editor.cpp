#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>
#include <bloom/ui/viewer_gpu_resident_overlay.hpp>
#include <memory>

#include "composition_editor_support.hpp"
#include "viewer_editor_text_layout.hpp"

#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
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
#include <bloom/document/shape.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputMethodEvent>
#include <QIntValidator>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <bloom/render/path_raster.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <numbers>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {
std::vector<document::LayerId>
pointTextLayers(const CompositionSession& session,
                std::span<const runtime::EvaluatedOperationBounds> bounds) {
    std::vector<document::LayerId> result;
    const auto* composition = session.composition();
    if (!composition)
        return result;
    for (const auto& bound : bounds) {
        const auto sourceId = session.directSourceNodeForLayer(bound.layerId);
        const auto* source = sourceId ? composition->graph().findNode(*sourceId) : nullptr;
        if (!source || source->typeId != document::kTextSourceNodeType)
            continue;
        const auto box = std::ranges::find(source->parameters, document::kTextBoxParameterRole,
                                           &document::ParameterBinding::role);
        const auto value =
            box != source->parameters.end() ? session.liveValue(box->parameterId) : std::nullopt;
        const auto* size = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
        if (!size || size->x == 0 || size->y == 0)
            result.push_back(bound.layerId);
    }
    return result;
}

// Keyboard steps use current authored ancestor linear transforms, so auto-repeat never waits
// for the previous nudge's preview. Pivot translations do not affect displacement vectors.
void nudgeViewerSelection(CompositionSession& session, const QPointF compositionDelta) {
    const auto* layer = std::get_if<document::LayerId>(&session.selection().primary);
    const auto position = session.effectiveVec2Value(document::kPositionParameterRole);
    if (!layer || !position || !session.composition())
        return;
    QTransform parentWorld;
    for (auto parent = session.parentOf(*layer); parent; parent = session.parentOf(*parent)) {
        const auto boundary = session.boundaryNodeForLayer(*parent);
        const auto* node = boundary ? session.composition()->graph().findNode(*boundary) : nullptr;
        if (!node)
            return;
        const auto scaleBinding = std::ranges::find(node->parameters, document::kScaleParameterRole,
                                                    &document::ParameterBinding::role);
        const auto rotationBinding = std::ranges::find(
            node->parameters, document::kRotationParameterRole, &document::ParameterBinding::role);
        if (scaleBinding == node->parameters.end() || rotationBinding == node->parameters.end())
            return;
        const auto scale = session.effectiveVec2Value(scaleBinding->parameterId);
        const auto rotation = session.effectiveScalarValue(rotationBinding->parameterId);
        if (!scale || !rotation)
            return;
        QTransform local;
        local.rotate(*rotation);
        local.scale(scale->x, scale->y);
        parentWorld *= local;
    }
    bool invertible = false;
    const auto inverse = parentWorld.inverted(&invertible);
    if (!invertible)
        return;
    const auto delta = inverse.map(compositionDelta);
    (void)session.setSelectedPosition(position->x + delta.x(), position->y + delta.y());
}

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
constexpr auto kSafeAreasSetting = "viewer/overlay/safe-areas";
constexpr auto kCentreCrossSetting = "viewer/overlay/centre-cross";
constexpr auto kThirdsSetting = "viewer/overlay/thirds";
constexpr auto kRulersSetting = "viewer/overlay/rulers";
constexpr auto kPixelGridSetting = "viewer/overlay/pixel-grid";

[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveViewerColorConfig(const CompositionSession& session) {
    const auto* builtIn = std::get_if<document::BuiltInOcioConfigLocator>(
        &session.colorSettings().ocioConfig.locator);
    if (builtIn == nullptr) {
        return std::nullopt;
    }
    auto result =
        color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
                                  session.colorSettings().ocioConfig.expectedRevision.digest,
                                  session.colorIntent().workingColorSpaceId);
    return std::move(result).takeResolved();
}

[[nodiscard]] bool hasLookTaggedEffect(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return false;
    }
    for (const auto& node : composition->graph().nodes()) {
        if (node.typeId != "bloom.ocio-file-transform") {
            continue;
        }
        const auto look =
            std::ranges::find(node.parameters, "look", &document::ParameterBinding::role);
        if (look == node.parameters.end()) {
            continue;
        }
        const auto* record = composition->parameters().find(look->parameterId);
        if (record == nullptr) {
            continue;
        }
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        if (constant == nullptr) {
            return true;
        }
        const auto* enabled = std::get_if<bool>(&constant->value);
        if (enabled == nullptr || *enabled) {
            return true;
        }
    }
    return false;
}

QString safeAreaPresetName(const ViewerSafeAreaPreset preset) {
    switch (preset) {
    case ViewerSafeAreaPreset::Broadcast:
        return QStringLiteral("Broadcast");
    case ViewerSafeAreaPreset::Hd:
        return QStringLiteral("HD");
    case ViewerSafeAreaPreset::Cinema:
        return QStringLiteral("Cinema");
    case ViewerSafeAreaPreset::Social:
        return QStringLiteral("Social");
    case ViewerSafeAreaPreset::Custom:
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Broadcast");
}

ViewerSafeAreaPreset safeAreaPresetFromName(const QString& name) {
    if (name == QStringLiteral("HD"))
        return ViewerSafeAreaPreset::Hd;
    if (name == QStringLiteral("Cinema"))
        return ViewerSafeAreaPreset::Cinema;
    if (name == QStringLiteral("Social"))
        return ViewerSafeAreaPreset::Social;
    if (name == QStringLiteral("Custom"))
        return ViewerSafeAreaPreset::Custom;
    return ViewerSafeAreaPreset::Broadcast;
}

QString safeAreaPresetSetting(const document::CompositionId id) {
    return QStringLiteral("viewer/overlay/safe-area-preset/%1").arg(id.value());
}

// Lays `controls` out left to right inside `bar`, each at its own size hint, vertically centred.
// A manual layout rather than a QHBoxLayout because the bar paints its own surface and hairline and
// because of the overflow rule below, which a box layout expresses by squeezing children instead.
//
// A control that does not fit ENTIRELY is hidden rather than clipped. A half-drawn dropdown or a
// truncated transport button is a control an artist can see and cannot use; a narrow Viewer simply
// offers fewer of them, and widening the panel brings them back. Nothing important is lost by that:
// the window status bar carries the state that has to stay on screen regardless.
// The footer's own icon-only transport button: a plain QToolButton carrying a kit icon at the
// Control role, the same "QToolButton + kit::icon()" idiom the timeline transport used before this
// moved (and EditorArea's header chrome still uses). NOT kit::KButton, which is not a QToolButton
// and would break every existing findChild<QToolButton*>("playPauseButton") contract. An icon never
// replaces an accessible name (ADR 0010), so every call site sets a tooltip AND an accessible name.
QToolButton* makeTransportButton(const kit::IconId iconId, const QString& toolTip,
                                 const QString& accessibleName, const QString& objectName,
                                 QWidget* parent) {
    auto* button = new kit::KIconButton(parent);
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
    constexpr qreal tileSize = kit::px(kit::Size::ViewerChecker);
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
// would defeat the purpose of the choice. Solid paints the panel's own background token, so the
// surround blends with the panel chrome rather than reading as a separate black rectangle; literal
// black is the Black choice.
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
    painter.fillRect(bounds, kit::color(kit::Color::Canvas));
}

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
                .value(QLatin1StringView(kTimeFormatSetting), QStringLiteral("timecode"))
                .toString() == QStringLiteral("timecode");

        stack_ = new QStackedLayout(this);
        stack_->setContentsMargins(0, 0, 0, 0);

        label_ = new kit::KLabel(this);
        // Unchanged objectName: this is the same readout the timeline transport used to own, moved
        // rather than replaced (task VIEW-1's "they change parent, not identity").
        label_->setObjectName(QStringLiteral("timelineTimeReadout"));
        label_->setAccessibleName(ViewerEditor::tr("Current frame and time"));
        label_->setFont(kit::font(kit::TypeRole::Value));
        // The click that starts an edit belongs to this widget, so the label never eats it.
        label_->setAttribute(Qt::WA_TransparentForMouseEvents);

        editor_ = new kit::KLineEdit(this);
        editor_->setObjectName(QStringLiteral("viewerTimeReadoutEditor"));
        editor_->setAccessibleName(ViewerEditor::tr("Go to frame"));
        editor_->setFont(kit::font(kit::TypeRole::Value));
        // A frame INDEX, always -- even while the label is showing timecode. Typing a frame number
        // is the one entry form that needs no parsing rules of its own, and it is what the task
        // asks for; the display format is a separate question the context menu answers.
        editor_->setToolTip(ViewerEditor::tr("Enter a frame number or HH:MM:SS:FF timecode"));
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
        label_->setText(frameText);
        label_->setToolTip(timecodeFormat_
                               ? ViewerEditor::tr("Non-drop timecode · exact composition time")
                               : ViewerEditor::tr("Frame index · exact composition time"));
    }

    [[nodiscard]] QSize sizeHint() const override {
        // A FIXED width from the widest string this readout can ever show, not the current text's
        // width: the text changes on every frame of playback, and a width that tracked it would
        // relayout the whole footer sixty times a second and make every control beside it twitch.
        return {kit::px(kit::Size::ViewerTimecodeWidth), kit::px(kit::Size::Control)};
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
        std::unique_ptr<QMenu> menuOwner(kit::makeMenu(this));
        auto& menu = *menuOwner;
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
        editor_->setText(timecodeFormat_ ? formatTimelineFrameLabel(nearest.value_or(0),
                                                                    context->frameRate, true)
                                         : QString::number(nearest.value_or(0)));
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
        auto typed = editor_->text().toULongLong(&parsed);
        const auto parts = editor_->text().split(':');
        if (context && parts.size() == 4) {
            bool valid = true;
            std::array<qulonglong, 4> values{};
            for (int index = 0; index < 4; ++index) {
                bool partOk = false;
                values[static_cast<std::size_t>(index)] = parts[index].toULongLong(&partOk);
                valid = valid && partOk;
            }
            const auto numerator = static_cast<qulonglong>(context->frameRate.numerator());
            const auto denominator = static_cast<qulonglong>(context->frameRate.denominator());
            const auto nominal =
                std::max<qulonglong>(1, (numerator + denominator / 2) / denominator);
            parsed = valid &&
                     values[0] < std::numeric_limits<qulonglong>::max() / nominal / 3600 - 1 &&
                     values[1] < 60 && values[2] < 60 && values[3] < nominal;
            if (parsed)
                typed = ((values[0] * 3600 + values[1] * 60 + values[2]) * nominal) + values[3];
        }
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

bool cpuFallbackCompletionIsCurrent(const runtime::PreviewRequestIdentity& completed,
                                    const runtime::PreviewRequestIdentity& current) {
    return completed == current;
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

void ViewerEditor::buildHeader() {
    auto* bar = &chrome_.header;
    bar->owner = this;
    bar->objectName = "viewerHeaderMenuBar";
    bar->overflowButtonName = "viewerHeaderOverflowButton";
    bar->overflowMenuName = "viewerHeaderOverflowMenu";

    compositionSelector_ = new kit::KDropdown(this);
    compositionSelector_->setObjectName(QStringLiteral("viewerCompositionSelector"));
    compositionSelector_->setAccessibleName(tr("Composition"));
    compositionSelector_->setToolTip(tr("Choose the composition shown in the viewer"));
    compositionSelector_->setControlSize(kit::KDropdown::ControlSize::Compact);
    compositionSelector_->setMinimumWidth(kit::px(kit::Size::DropdownWidthWide));
    compositionSelector_->setMaximumWidth(kit::px(kit::Size::DropdownWidthExpanded));
    connect(compositionSelector_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0) {
                    return;
                }
                const auto id = document::CompositionId::fromRaw(
                    compositionSelector_->itemData(index).toULongLong());
                if (id.isValid()) {
                    (void)session_.setComposition(id);
                }
            });
    bar->addWidget(compositionSelector_);

    compositionMenuButton_ = new kit::KIconButton(this);
    compositionMenuButton_->setObjectName(QStringLiteral("viewerCompositionMenuButton"));
    compositionMenuButton_->setIcon(kit::icon(kit::IconId::ContextMenu, kit::IconRole::Chrome));
    compositionMenuButton_->setIconSize(QSize(kit::px(kit::iconSize(kit::IconRole::Chrome)),
                                              kit::px(kit::iconSize(kit::IconRole::Chrome))));
    compositionMenuButton_->setToolTip(tr("Composition commands"));
    compositionMenuButton_->setAccessibleName(tr("Composition commands"));
    compositionMenuButton_->setAutoRaise(true);
    compositionMenuButton_->setFixedSize(
        QSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control)));
    auto* compositionMenu = kit::makeMenu(this);
    compositionMenu->setObjectName(QStringLiteral("viewerCompositionMenu"));
    compositionMenuButton_->setMenu(compositionMenu);
    compositionMenuButton_->setPopupMode(QToolButton::InstantPopup);
    compositionMenuButton_->hide();

    viewerCompositionNewAction_ = compositionMenu->addAction(tr("New Composition…"));
    viewerCompositionNewAction_->setObjectName(QStringLiteral("viewerNewCompositionAction"));
    connect(viewerCompositionNewAction_, &QAction::triggered, this, [this] {
        if (const auto id = showNewCompositionDialog(session_, this); id.has_value()) {
            (void)session_.setComposition(*id);
        }
    });
    viewerCompositionDuplicateAction_ = compositionMenu->addAction(tr("Duplicate Composition"));
    viewerCompositionDuplicateAction_->setObjectName(
        QStringLiteral("viewerDuplicateCompositionAction"));
    connect(viewerCompositionDuplicateAction_, &QAction::triggered, this, [this] {
        if (const auto id = duplicateComposition(session_, session_.compositionId());
            id.has_value()) {
            (void)session_.setComposition(*id);
        }
    });
    viewerCompositionRenameAction_ = compositionMenu->addAction(tr("Rename Composition…"));
    viewerCompositionRenameAction_->setObjectName(QStringLiteral("viewerRenameCompositionAction"));
    connect(viewerCompositionRenameAction_, &QAction::triggered, this,
            [this] { (void)renameComposition(session_, session_.compositionId(), this); });
    viewerCompositionDeleteAction_ = compositionMenu->addAction(tr("Delete Composition"));
    viewerCompositionDeleteAction_->setObjectName(QStringLiteral("viewerDeleteCompositionAction"));
    connect(viewerCompositionDeleteAction_, &QAction::triggered, this,
            [this] { (void)deleteComposition(session_, session_.compositionId()); });

    objectSelector_ = new kit::KDropdown(this);
    objectSelector_->setObjectName(QStringLiteral("viewerObjectSelector"));
    objectSelector_->setAccessibleName(tr("Object"));
    objectSelector_->setToolTip(tr("Choose a layer in the current composition"));
    objectSelector_->setControlSize(kit::KDropdown::ControlSize::Compact);
    objectSelector_->setMinimumWidth(kit::px(kit::Size::DropdownWidthWide));
    objectSelector_->setMaximumWidth(kit::px(kit::Size::DropdownWidthExpanded));
    connect(objectSelector_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (index <= 0) {
            session_.clearSelection();
            return;
        }
        const auto id = document::LayerId::fromRaw(objectSelector_->itemData(index).toULongLong());
        if (id.isValid()) {
            session_.selectLayer(id);
        }
    });
    bar->addWidget(objectSelector_);

    viewerViewMenu_ = kit::makeMenu(tr("View"), this);
    viewerViewMenu_->setObjectName(QStringLiteral("viewerViewMenu"));
    viewerFitAction_ = viewerViewMenu_->addAction(tr("Fit"));
    viewerFitAction_->setObjectName(QStringLiteral("viewerFitAction"));
    viewerFitAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    viewerFitAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    viewerActualSizeAction_ = viewerViewMenu_->addAction(tr("Actual Size"));
    viewerActualSizeAction_->setObjectName(QStringLiteral("viewerActualSizeAction"));
    viewerActualSizeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    viewerActualSizeAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    viewerZoomInAction_ = viewerViewMenu_->addAction(tr("Zoom In"));
    viewerZoomInAction_->setObjectName(QStringLiteral("viewerZoomInAction"));
    viewerZoomInAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
    viewerZoomInAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    viewerZoomOutAction_ = viewerViewMenu_->addAction(tr("Zoom Out"));
    viewerZoomOutAction_->setObjectName(QStringLiteral("viewerZoomOutAction"));
    viewerZoomOutAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    viewerZoomOutAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(viewerFitAction_);
    addAction(viewerActualSizeAction_);
    addAction(viewerZoomInAction_);
    addAction(viewerZoomOutAction_);
    connect(viewerFitAction_, &QAction::triggered, this, &ViewerEditor::setZoomFit);
    connect(viewerActualSizeAction_, &QAction::triggered, this, &ViewerEditor::setZoomActualSize);
    connect(viewerZoomInAction_, &QAction::triggered, this, &ViewerEditor::zoomInAtCenter);
    connect(viewerZoomOutAction_, &QAction::triggered, this, &ViewerEditor::zoomOutAtCenter);

    auto* channelMenu = viewerViewMenu_->addMenu(tr("Channel"));
    channelMenu->setObjectName(QStringLiteral("viewerChannelMenu"));
    auto* channelGroup = new QActionGroup(channelMenu);
    channelGroup->setExclusive(true);
    for (int index = 0; index < static_cast<int>(kChannelNames.size()); ++index) {
        auto* action = channelMenu->addAction(tr(kChannelNames[static_cast<std::size_t>(index)]));
        action->setObjectName(QStringLiteral("viewerChannel%1Action").arg(index));
        action->setCheckable(true);
        action->setData(index);
        channelGroup->addAction(action);
    }
    channelGroup->actions().front()->setChecked(true);
    connect(channelGroup, &QActionGroup::triggered, this, [this](QAction* action) {
        setChannel(static_cast<ViewerChannel>(action->data().toInt()));
    });

    auto* backgroundMenu = viewerViewMenu_->addMenu(tr("Background"));
    backgroundMenu->setObjectName(QStringLiteral("viewerBackgroundMenu"));
    auto* backgroundGroup = new QActionGroup(backgroundMenu);
    backgroundGroup->setExclusive(true);
    for (int index = 0; index < static_cast<int>(kBackgroundNames.size()); ++index) {
        auto* action =
            backgroundMenu->addAction(tr(kBackgroundNames[static_cast<std::size_t>(index)]));
        action->setObjectName(QStringLiteral("viewerBackground%1Action").arg(index));
        action->setCheckable(true);
        action->setData(index);
        backgroundGroup->addAction(action);
    }
    backgroundGroup->actions().front()->setChecked(true);
    connect(backgroundGroup, &QActionGroup::triggered, this, [this](QAction* action) {
        setBackground(static_cast<ViewerBackground>(action->data().toInt()));
    });

    viewerViewMenu_->addSeparator();
    safeAreasAction_ = viewerViewMenu_->addAction(tr("Safe Areas"));
    safeAreasAction_->setObjectName(QStringLiteral("viewerSafeAreasAction"));
    safeAreasAction_->setCheckable(true);
    safeAreasAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
    safeAreasAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    overlayOptions_.safeAreas = QSettings().value(kSafeAreasSetting, false).toBool();
    safeAreasAction_->setChecked(overlayOptions_.safeAreas);
    addAction(safeAreasAction_);
    connect(safeAreasAction_, &QAction::toggled, this, [this](const bool enabled) {
        overlayOptions_.safeAreas = enabled;
        QSettings().setValue(kSafeAreasSetting, enabled);
        ++gpuOverlayRevision_;
        updateGpuResidentPresentation();
        update();
    });

    safeAreaPresetMenu_ = viewerViewMenu_->addMenu(tr("Safe Area Preset"));
    safeAreaPresetMenu_->setObjectName(QStringLiteral("viewerSafeAreaPresetMenu"));
    const std::array<std::pair<QString, ViewerSafeAreaPreset>, 5> presets{{
        {tr("Broadcast (4:3)"), ViewerSafeAreaPreset::Broadcast},
        {tr("HD (16:9)"), ViewerSafeAreaPreset::Hd},
        {tr("Cinema"), ViewerSafeAreaPreset::Cinema},
        {tr("Social"), ViewerSafeAreaPreset::Social},
        {tr("Custom…"), ViewerSafeAreaPreset::Custom},
    }};
    for (std::size_t index = 0; index < presets.size(); ++index) {
        auto* action = safeAreaPresetMenu_->addAction(presets[index].first);
        action->setObjectName(QStringLiteral("viewerSafeArea%1Action").arg(index));
        action->setCheckable(true);
        safeAreaPresetActions_[index] = action;
        connect(action, &QAction::triggered, this,
                [this, preset = presets[index].second] { applySafeAreaPreset(preset); });
    }

    const auto addOverlayToggle = [this](const QString& label, const QString& objectName,
                                         const QString& setting, const QKeySequence& shortcut,
                                         bool* state) {
        auto* action = viewerViewMenu_->addAction(label);
        action->setObjectName(objectName);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        *state = QSettings().value(setting, false).toBool();
        action->setChecked(*state);
        addAction(action);
        connect(action, &QAction::toggled, this, [state, setting](const bool enabled) {
            *state = enabled;
            QSettings().setValue(setting, enabled);
        });
        connect(action, &QAction::toggled, this, [this] {
            ++gpuOverlayRevision_;
            updateGpuResidentPresentation();
            update();
        });
        return action;
    };
    centreCrossAction_ = addOverlayToggle(
        tr("Centre Cross"), QStringLiteral("viewerCentreCrossAction"), kCentreCrossSetting,
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), &overlayOptions_.centreCross);
    thirdsAction_ =
        addOverlayToggle(tr("Thirds"), QStringLiteral("viewerThirdsAction"), kThirdsSetting,
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), &overlayOptions_.thirds);
    rulersAction_ =
        addOverlayToggle(tr("Rulers"), QStringLiteral("viewerRulers"), kRulersSetting,
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), &overlayOptions_.rulers);
    pixelGridAction_ = addOverlayToggle(
        tr("Pixel Grid"), QStringLiteral("viewerPixelGridAction"), kPixelGridSetting,
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), &overlayOptions_.pixelGrid);

    viewerSelectMenu_ = kit::makeMenu(tr("Select"), this);
    viewerSelectMenu_->setObjectName(QStringLiteral("viewerSelectMenu"));
    auto* selectAll = viewerSelectMenu_->addAction(tr("All"));
    selectAll->setObjectName(QStringLiteral("viewerSelectAllAction"));
    selectAll->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
    selectAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    auto* selectNone = viewerSelectMenu_->addAction(tr("None"));
    selectNone->setObjectName(QStringLiteral("viewerSelectNoneAction"));
    selectNone->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    selectNone->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    auto* invert = viewerSelectMenu_->addAction(tr("Invert"));
    invert->setObjectName(QStringLiteral("viewerInvertSelectionAction"));
    invert->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
    invert->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(selectAll);
    addAction(selectNone);
    addAction(invert);
    connect(selectAll, &QAction::triggered, this, &ViewerEditor::selectAllObjects);
    connect(selectNone, &QAction::triggered, this, &ViewerEditor::selectNoObjects);
    connect(invert, &QAction::triggered, this, &ViewerEditor::invertObjectSelection);

    bar->addMenuButton(tr("View"), viewerViewMenu_, QStringLiteral("viewerViewMenuButton"));
    bar->addMenuButton(tr("Select"), viewerSelectMenu_, QStringLiteral("viewerSelectMenuButton"));
    auto* addMenu = kit::makeMenu(tr("Add"), this);
    addMenu->setObjectName("viewerAddMenu");
    connect(addMenu->addAction(tr("Solid")), &QAction::triggered, this,
            [this] { (void)addDefaultSolidLayer(session_); });
    connect(addMenu->addAction(tr("Text")), &QAction::triggered, this,
            [this] { (void)addDefaultTextLayer(session_); });
    for (std::int64_t index = 0; index <= 6; ++index) {
        const auto kind = static_cast<document::ShapeKind>(index);
        auto* shapeAction =
            addMenu->addAction(QString::fromUtf8(document::shapeKindName(kind).data()));
        shapeAction->setObjectName(QStringLiteral("viewerAddShape.%1").arg(index));
        connect(shapeAction, &QAction::triggered, this,
                [this, kind] { (void)addDefaultShapeLayer(session_, kind); });
    }
    bar->addMenuButton(tr("Add"), addMenu, "viewerAddMenuButton");
    viewerViewMenu_->addMenu(compositionMenu)->setText(tr("Composition"));
    bar->addStretch();

    fullscreenButton_ = new kit::KIconButton(this);
    fullscreenButton_->setObjectName(QStringLiteral("viewerFullscreenButton"));
    fullscreenButton_->setIcon(kit::icon(kit::IconId::Maximize, kit::IconRole::Chrome));
    fullscreenButton_->setIconSize(QSize(kit::px(kit::iconSize(kit::IconRole::Chrome)),
                                         kit::px(kit::iconSize(kit::IconRole::Chrome))));
    fullscreenButton_->setToolTip(tr("Full Screen (F11)"));
    fullscreenButton_->setAccessibleName(tr("Full Screen (F11)"));
    fullscreenButton_->setAutoRaise(true);
    fullscreenButton_->setCheckable(true);
    fullscreenButton_->setChecked(window() != nullptr && window()->isFullScreen());
    fullscreenButton_->setFixedSize(
        QSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control)));
    connect(fullscreenButton_, &QToolButton::clicked, this, [this] {
        if (window() == nullptr) {
            return;
        }
        if (auto* action = window()->findChild<QAction*>(QStringLiteral("viewFullScreenAction"))) {
            action->trigger();
            fullscreenButton_->setChecked(window()->isFullScreen());
        }
    });
    fullscreenButton_->hide();
    headerMenuWidget_ = EditorArea::buildChromeRow(chrome_.header, this);
    headerMenuWidget_->hide(); // The canvas remains full-bleed until EditorArea hosts chrome.

    rebuildCompositionSelector();
    rebuildObjectSelector();
    updateCompositionActions();
}

void ViewerEditor::rebuildCompositionSelector() {
    if (compositionSelector_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(compositionSelector_);
    compositionSelector_->clearItems();
    const auto compositions = session_.snapshot().project().compositions();
    if (compositions.empty()) {
        compositionSelector_->addItem(tr("None"));
        return;
    }
    int selectedIndex = 0;
    int index = 0;
    for (const auto& composition : compositions) {
        compositionSelector_->addItem(QString::fromStdString(composition.name()),
                                      QVariant::fromValue<qulonglong>(composition.id().value()));
        if (composition.id() == session_.compositionId()) {
            selectedIndex = index;
        }
        ++index;
    }
    compositionSelector_->setCurrentIndex(selectedIndex);
}

void ViewerEditor::rebuildObjectSelector() {
    if (objectSelector_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(objectSelector_);
    objectSelector_->clearItems();
    objectSelector_->addItem(tr("None"), QVariant::fromValue<qulonglong>(0));
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        objectSelector_->setCurrentIndex(0);
        return;
    }
    const auto selectedLayer = session_.selection().contextualLayer;
    int selectedIndex = 0;
    int index = 1;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        objectSelector_->addItem(layerName(*composition, boundary.layerId),
                                 QVariant::fromValue<qulonglong>(boundary.layerId.value()));
        if (selectedLayer.has_value() && *selectedLayer == boundary.layerId) {
            selectedIndex = index;
        }
        ++index;
    }
    objectSelector_->setCurrentIndex(selectedIndex);
}

void ViewerEditor::updateCompositionActions() {
    if (viewerCompositionNewAction_ == nullptr) {
        return;
    }
    const auto* composition = session_.composition();
    const bool hasComposition = composition != nullptr;
    viewerCompositionDuplicateAction_->setEnabled(hasComposition);
    viewerCompositionRenameAction_->setEnabled(hasComposition);
    // Deleting the final composition is allowed and returns to the blank, usable empty project.
    viewerCompositionDeleteAction_->setEnabled(hasComposition);
}

void ViewerEditor::updateOverlayActions() {
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        overlayOptions_.safeAreaSettings = composition->safeAreas();
        const QString saved =
            QSettings().value(safeAreaPresetSetting(composition->id())).toString();
        if (!saved.isEmpty()) {
            overlayOptions_.safeAreaPreset = safeAreaPresetFromName(saved);
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.93) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.90) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Hd;
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.90) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.85) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Cinema;
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.90) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.80) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Broadcast;
        } else {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Custom;
        }
    } else {
        overlayOptions_.safeAreaSettings = {};
        overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Broadcast;
    }
    if (safeAreasAction_ != nullptr) {
        const QSignalBlocker blocker(safeAreasAction_);
        safeAreasAction_->setChecked(overlayOptions_.safeAreas);
        safeAreasAction_->setEnabled(composition != nullptr);
    }
    if (safeAreaPresetMenu_ != nullptr) {
        safeAreaPresetMenu_->setEnabled(composition != nullptr);
        for (std::size_t index = 0; index < safeAreaPresetActions_.size(); ++index) {
            const QSignalBlocker blocker(safeAreaPresetActions_[index]);
            safeAreaPresetActions_[index]->setChecked(
                static_cast<int>(overlayOptions_.safeAreaPreset) == static_cast<int>(index));
        }
    }
}

void ViewerEditor::applySafeAreaPreset(const ViewerSafeAreaPreset preset) {
    if (preset == ViewerSafeAreaPreset::Custom) {
        showCustomSafeAreaDialog();
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    document::SafeAreaSettings settings{};
    switch (preset) {
    case ViewerSafeAreaPreset::Broadcast:
        settings = {.action = 0.90, .title = 0.80};
        break;
    case ViewerSafeAreaPreset::Hd:
        settings = {.action = 0.93, .title = 0.90};
        break;
    case ViewerSafeAreaPreset::Cinema:
        settings = {.action = 0.90, .title = 0.85};
        break;
    case ViewerSafeAreaPreset::Social:
        settings = {.action = 0.90, .title = 0.80};
        break;
    case ViewerSafeAreaPreset::Custom:
        return;
    }
    commands::Transaction transaction(QStringLiteral("Set Safe Area Preset").toStdString(),
                                      session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionSafeAreas>(composition->id(), settings);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        overlayOptions_.safeAreaPreset = preset;
        QSettings().setValue(safeAreaPresetSetting(composition->id()), safeAreaPresetName(preset));
        updateOverlayActions();
        update();
    }
}

void ViewerEditor::showCustomSafeAreaDialog() {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("viewerSafeAreaDialog"));
    dialog.setWindowTitle(tr("Custom Safe Areas"));
    auto* form = new QFormLayout(&dialog);
    auto* action = new QDoubleSpinBox(&dialog);
    action->setObjectName(QStringLiteral("viewerActionSafeAreaField"));
    action->setRange(1.0, 100.0);
    action->setDecimals(1);
    action->setSuffix(QStringLiteral("%"));
    action->setValue(composition->safeAreas().action * 100.0);
    form->addRow(tr("Action"), action);
    auto* title = new QDoubleSpinBox(&dialog);
    title->setObjectName(QStringLiteral("viewerTitleSafeAreaField"));
    title->setRange(1.0, 100.0);
    title->setDecimals(1);
    title->setSuffix(QStringLiteral("%"));
    title->setValue(composition->safeAreas().title * 100.0);
    form->addRow(tr("Title"), title);
    auto* error = new kit::KLabel(&dialog);
    error->setObjectName(QStringLiteral("viewerSafeAreaErrorLabel"));
    error->hide();
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("viewerSafeAreaDialogButtons"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, action, title, error] {
        if (title->value() > action->value()) {
            error->setText(QObject::tr("Title safe area must not exceed Action safe area."));
            error->show();
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const document::SafeAreaSettings settings{action->value() / 100.0, title->value() / 100.0};
    commands::Transaction transaction(QStringLiteral("Set Custom Safe Areas").toStdString(),
                                      session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionSafeAreas>(composition->id(), settings);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Custom;
        QSettings().setValue(safeAreaPresetSetting(composition->id()),
                             safeAreaPresetName(ViewerSafeAreaPreset::Custom));
        updateOverlayActions();
        update();
    }
}

double ViewerEditor::effectiveZoom(const QRectF& displayRect,
                                   const DisplayGeometry& geometry) const {
    const QRectF actual = actualPixelRect(canvasRect(), geometry.extent, geometry.pixelAspect);
    return actual.width() > 0.0 ? displayRect.width() / actual.width() : 1.0;
}

void ViewerEditor::selectAllObjects() {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    std::set<document::NodeId> nodes;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        if (boundary.nodeId.isValid()) {
            nodes.insert(boundary.nodeId);
        }
    }
    if (!nodes.empty()) {
        session_.selectNodes(nodes, *nodes.begin());
    }
}

void ViewerEditor::selectNoObjects() { session_.clearSelection(); }

void ViewerEditor::invertObjectSelection() {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    std::set<document::NodeId> all;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        if (boundary.nodeId.isValid()) {
            all.insert(boundary.nodeId);
        }
    }
    std::set<document::NodeId> remaining;
    std::ranges::set_difference(all, session_.selectedNodes(),
                                std::inserter(remaining, remaining.end()));
    if (remaining.empty()) {
        session_.clearSelection();
    } else {
        session_.selectNodes(remaining, *remaining.begin());
    }
}

void ViewerEditor::zoomInAtCenter() {
    if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
        const QRectF frame = canvasRect();
        transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                    frame.center(), kZoomStepFactor);
        refreshZoomDropdown();
        update();
    }
}

void ViewerEditor::zoomOutAtCenter() {
    if (const auto geometry = currentDisplayGeometry(); geometry.has_value()) {
        const QRectF frame = canvasRect();
        transform_ = zoomAboutPoint(transform_, frame, geometry->extent, geometry->pixelAspect,
                                    frame.center(), 1.0 / kZoomStepFactor);
        refreshZoomDropdown();
        update();
    }
}

// Builds the footer row and everything in it (task VIEW-1). Called once, from the constructor.
void ViewerEditor::rebuildDisplayViewControl() {
    if (viewerDisplayView_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(viewerDisplayView_);
    viewerDisplayView_->clearItems();
    const auto resolved = resolveViewerColorConfig(session_);
    if (!resolved.has_value() || resolved->displays().empty()) {
        viewerDisplayView_->addItem(tr("Unavailable"));
        viewerDisplayView_->setCurrentIndex(0);
        viewerDisplayView_->setEnabled(false);
        viewerDisplayView_->setMutedValue(true);
        return;
    }

    int defaultIndex = 0;
    for (std::size_t index = 0; index < resolved->displays().size(); ++index) {
        const auto& entry = resolved->displays()[index];
        const auto label = QStringLiteral("%1 / %2").arg(QString::fromStdString(entry.display),
                                                         QString::fromStdString(entry.view));
        const auto data =
            QStringList{QString::fromStdString(entry.display), QString::fromStdString(entry.view)};
        const int itemIndex = viewerDisplayView_->addItem(label, data);
        viewerDisplayView_->setItemToolTip(
            itemIndex,
            QStringLiteral("%1 · %2").arg(label, QString::fromStdString(entry.colourSpaceId)));
        if (entry.isDefault) {
            defaultIndex = itemIndex;
        }
    }
    const auto saved = QSettings().value(displayViewSettingsKey()).toStringList();
    int selectedIndex = defaultIndex;
    if (saved.size() == 2) {
        for (int index = 0; index < viewerDisplayView_->count(); ++index) {
            if (viewerDisplayView_->itemData(index).toStringList() == saved) {
                selectedIndex = index;
                break;
            }
        }
    }
    viewerDisplayView_->setEnabled(true);
    viewerDisplayView_->setMutedValue(false);
    viewerDisplayView_->setCurrentIndex(selectedIndex);
    const auto selected = viewerDisplayView_->itemData(selectedIndex).toStringList();
    if (selected.size() == 2) {
        previewController_.setViewerDisplayView(selected[0].toStdString(),
                                                selected[1].toStdString());
    }
}

void ViewerEditor::updateLookControl() {
    if (viewerLookToggle_ == nullptr) {
        return;
    }
    const bool available = hasLookTaggedEffect(session_);
    viewerLookToggle_->setEnabled(available);
    viewerLookToggle_->setToolTip(
        available ? tr("Look: include look-tagged effects in the viewer")
                  : tr("Look unavailable: this composition has no look-tagged effect"));
}

void ViewerEditor::buildFooter(RamPreviewController* const ramPreview) {
    auto* footer = this;
    chrome_.footer.objectName = "viewerFooter";

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

    roiButton_ = new kit::KIconButton(footer);
    roiButton_->setToolTip(tr("Region of interest: Ctrl-drag with Select"));
    roiButton_->setObjectName("viewerRoiToggle");
    roiButton_->setAccessibleName(tr("Region of interest"));
    roiButton_->setCheckable(true);
    roiButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    roiButton_->setText(tr("ROI"));
    connect(roiButton_, &QToolButton::toggled, this, [this] { publishRoi(); });
    roiClearButton_ = new kit::KIconButton(footer);
    roiClearButton_->setToolTip(tr("Clear region of interest"));
    roiClearButton_->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Control));
    roiClearButton_->setEnabled(false);
    roiClearButton_->setObjectName("viewerRoiClear");
    connect(roiClearButton_, &QToolButton::clicked, this, [this] {
        roiGesture_.reset();
        roiRect_.reset();
        roiButton_->setChecked(false);
        publishRoi();
    });

    exposureField_ = new kit::KValueField(footer);
    exposureField_->setObjectName("viewerExposure");
    exposureField_->setLabel(tr("EV"));
    exposureField_->setAccessibleName(tr("Viewer exposure in EV stops"));
    exposureField_->setCompact(true);
    exposureField_->setRange(-32, 32);
    exposureField_->setSingleStep(0.1);
    exposureField_->setDecimals(2);
    exposureField_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    gammaField_ = new kit::KValueField(footer);
    gammaField_->setObjectName("viewerGamma");
    gammaField_->setLabel(tr("γ"));
    gammaField_->setAccessibleName(tr("Viewer gamma"));
    gammaField_->setToolTip(tr("Gamma after display encoding; viewer display only"));
    gammaField_->setCompact(true);
    gammaField_->setRange(0.01, 10);
    gammaField_->setSingleStep(0.05);
    gammaField_->setDecimals(2);
    gammaField_->setValue(1);
    gammaField_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    const auto changed = [this] {
        viewAdjust_ = {exposureField_->value(), gammaField_->value()};
        const auto prefix = analysisSettingsPrefix();
        QSettings settings;
        settings.setValue(prefix + "exposure", viewAdjust_.exposure);
        settings.setValue(prefix + "gamma", viewAdjust_.gamma);
        refreshViewAdjustment();
        update();
    };
    connect(exposureField_, &kit::KValueField::valueChanged, this, changed);
    connect(gammaField_, &kit::KValueField::valueChanged, this, changed);
    analysisTimer_ = new QTimer(this);
    analysisTimer_->setInterval(16);
    connect(analysisTimer_, &QTimer::timeout, this, &ViewerEditor::consumeViewAdjustment);
    loadViewAdjust();

    viewerDisplayView_ = new kit::KDropdown(footer);
    viewerDisplayView_->setObjectName("viewerDisplayView");
    viewerDisplayView_->setAccessibleName(tr("Display and view"));
    viewerDisplayView_->setToolTip(tr("Display / View; viewer display only, never export"));
    viewerDisplayView_->setControlSize(kit::KDropdown::ControlSize::Compact);
    connect(viewerDisplayView_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0) {
                    return;
                }
                const auto selected = viewerDisplayView_->itemData(index).toStringList();
                if (selected.size() != 2) {
                    return;
                }
                QSettings().setValue(displayViewSettingsKey(), selected);
                previewController_.setViewerDisplayView(selected[0].toStdString(),
                                                        selected[1].toStdString());
            });

    viewerLookToggle_ = new kit::KIconToggle(kit::IconId::Visible, footer);
    viewerLookToggle_->setObjectName("viewerLookToggle");
    viewerLookToggle_->setAccessibleName(tr("Look"));
    viewerLookToggle_->setToolTip(tr("Look: include look-tagged effects in the viewer"));
    const bool showLook = QSettings().value(lookSettingsKey(), true).toBool();
    viewerLookToggle_->setChecked(showLook);
    previewController_.setViewerLookEnabled(showLook);
    connect(viewerLookToggle_, &kit::KIconToggle::toggled, this, [this](const bool enabled) {
        QSettings().setValue(lookSettingsKey(), enabled);
        previewController_.setViewerLookEnabled(enabled);
    });
    rebuildDisplayViewControl();
    updateLookControl();

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
        resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));
    });
    // Part of the Resolution control, not a footer item of its own: what the chosen policy actually
    // resolved to. For Auto that effective factor is visible nowhere else.
    resolutionReadout_ = new kit::KLabel(footer);
    resolutionReadout_->setObjectName("viewerResolutionReadout");
    resolutionReadout_->setAccessibleName(tr("Effective preview resolution"));
    resolutionReadout_->setFont(kit::font(kit::TypeRole::Value));
    resolutionReadout_->setText(viewerResolutionText(previewController_));
    resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));

    // ---- Background ----------------------------------------------------------------------------
    backgroundDropdown_ = new kit::KDropdown(footer);
    backgroundDropdown_->setObjectName("viewerBackgroundDropdown");
    backgroundDropdown_->setAccessibleName(tr("Background"));
    backgroundDropdown_->setToolTip(
        tr("What the viewer paints behind the composition. Solid uses its background colour."));
    backgroundDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    for (const auto* name : kBackgroundNames) {
        backgroundDropdown_->addItem(tr(name));
    }
    // The default background is Checkerboard, so alpha behind the composition is always visible. A
    // missing or unrecognized viewer/background reads as it.
    const auto savedBackground =
        QSettings().value(kBackgroundSetting, QStringLiteral("Checkerboard")).toString();
    int backgroundIndex = static_cast<int>(ViewerBackground::Checkerboard);
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

    channelDropdown_->setFixedWidth(kit::px(kit::Size::ViewerChannelWidth));
    zoomDropdown_->setWidthFloor(kit::px(kit::Size::ViewerZoomWidth));
    resolutionDropdown_->setFixedWidth(kit::px(kit::Size::ViewerResolutionWidth));
    timeReadout_->setFixedWidth(kit::px(kit::Size::ViewerTimecodeWidth));
    ramPreviewButton_->setFixedWidth(kit::px(kit::Size::ViewerModeWidth));
    ramPreviewButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    ramPreviewButton_->setText(tr("RAM Preview"));
    // Background stays available from View. Keep old object identities for automation.
    backgroundDropdown_->hide();
    resolutionReadout_->hide();
    resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));
    for (auto* control : std::initializer_list<QWidget*>{
             channelDropdown_, roiButton_, roiClearButton_, exposureField_, gammaField_,
             viewerDisplayView_, viewerLookToggle_, ramPreviewButton_, stepToStartButton_,
             stepBackButton_, playPauseButton_, stepForwardButton_, stepToEndButton_, loopButton_})
        chrome_.footer.addWidget(control);
    chrome_.footer.addStretch();
    for (auto* control :
         std::initializer_list<QWidget*>{timeReadout_, zoomDropdown_, resolutionDropdown_})
        chrome_.footer.addWidget(control);
    statusBarFooter_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
    viewerDisplayView_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    chrome_.hosted = [this] {
        statusBarFooterTaken_ = true;
        loadViewAdjust();
        updatePreviewResolution();
        update();
    };
    layoutStatusBar();
}

ViewerEditor::ViewerEditor(CompositionSession& session,
                           CompositionPreviewController& previewController,
                           RamPreviewController* const ramPreview, QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController),
      ramPreview_(ramPreview) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    auto* tools = new kit::KToolColumn(this);
    toolColumn_ = tools;
    tools->setObjectName("viewerToolColumn");
    const auto addTool = [this, tools](Tool tool, kit::IconId icon, const QString& tip,
                                       const char* name) {
        auto* button = tools->addTool(icon, tip, name);
        button->setProperty("viewerTool", static_cast<int>(tool));
        connect(button, &QToolButton::clicked, this, [this, tool] { selectTool(tool); });
        button->setChecked(tool == Tool::Select);
    };
    addTool(Tool::Select, kit::IconId::Select, tr("Select and move (V)"), "viewerSelectTool");
    addTool(Tool::Hand, kit::IconId::Pan, tr("Hand: drag to pan (H)"), "viewerHandTool");
    addTool(Tool::Zoom, kit::IconId::Zoom, tr("Zoom: click to zoom in; Alt-click to zoom out (Z)"),
            "viewerZoomTool");
    addTool(Tool::Text, kit::IconId::Text, tr("Text: click to place and type on the canvas (T)"),
            "viewerTextTool");
    addTool(Tool::Rectangle, kit::IconId::Rectangle,
            tr("Rectangle: drag; Shift constrains; Alt draws from centre (R)"),
            "viewerRectangleTool");
    addTool(Tool::Ellipse, kit::IconId::Ellipse,
            tr("Ellipse: drag; Shift makes a circle; Alt draws from centre (E)"),
            "viewerEllipseTool");
    addTool(Tool::Polygon, kit::IconId::Polygon,
            tr("Polygon: drag a five-sided polygon; Shift constrains; Alt draws from centre"),
            "viewerPolygonTool");
    addTool(Tool::Star, kit::IconId::Star,
            tr("Star: drag a five-point star; Shift constrains; Alt draws from centre"),
            "viewerStarTool");
    addTool(Tool::Line, kit::IconId::Line,
            tr("Line: drag; Shift snaps to 45 degrees; Alt draws from centre"), "viewerLineTool");
    addTool(Tool::Pen, kit::IconId::Pen,
            tr("Pen: click anchors; drag handles; click first to close; Enter finishes; Escape "
               "cancels (P)"),
            "viewerPenTool");
    tools->adjustSize();
    tools->move(0, 0);

    setMinimumSize(kit::px(kit::Size::ViewerMinWidth),
                   std::max(kit::px(kit::Size::ViewerMinHeight),
                            tools->sizeHint().height() + kit::px(kit::Size::Control)));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // StrongFocus lets a press-to-drag gesture also receive the Escape key that cancels it, and
    // lets the widget receive Space/Z/F without a prior click.
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    probeTimer_ = new QTimer(this);
    probeTimer_->setInterval(16);
    connect(probeTimer_, &QTimer::timeout, this, &ViewerEditor::consumeProbe);
    connect(this, &ViewerEditor::probeChanged, &previewController_,
            &CompositionPreviewController::probeChanged);
    connect(&previewController_, &CompositionPreviewController::stateChanged, this, [this] {
        if (probePosition_)
            refreshProbe(*probePosition_);
    });

    connect(&session_, &CompositionSession::snapshotChanged, this, [this] { cancelCreation(); });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        cancelCreation();
        roiRect_.reset();
        if (roiButton_)
            roiButton_->setChecked(false);
        publishRoi();
    });
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { cancelCreation(); });
    connect(&session_, &CompositionSession::selectionChanged, this, [this] {
        cancelPathDrag();
        selectedAnchor_.reset();
        update();
    });
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        if (textEdit_ && !session_.isValueEditing(textEdit_->parameter))
            finishTextEditing(false);
        update();
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // Color readiness also refreshes projections without changing the document revision.
        // The session invalidates its value edit before publishing an actual revision change.
        if (textEdit_ && !session_.isValueEditing(textEdit_->parameter))
            finishTextEditing(false);
    });
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { finishTextEditing(false); });
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            [this] { finishTextEditing(false); });
    connect(&session_, &CompositionSession::selectionChanged, this,
            [this] { finishTextEditing(true); });
    playback_ = &previewController.playbackController();
    buildHeader();
    buildFooter(ramPreview);
    wireTransport();
    connect(&session_, &CompositionSession::colorSettingsChanged, this,
            [this] { rebuildDisplayViewControl(); });
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] { updateLookControl(); });
    connect(&session_, &CompositionSession::compositionChanged, this,
            [this] { updateLookControl(); });

    // Every one of these already repainted the status bar for free when it was part of this

    // out into its own widget by also nudging statusBarFooter_ (a no-op update() call until then,
    // since it starts null).
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        rebuildCompositionSelector();
        rebuildObjectSelector();
        updateCompositionActions();
        updateOverlayActions();
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        rebuildCompositionSelector();
        rebuildObjectSelector();
        updateCompositionActions();
        updateOverlayActions();
        updatePreviewResolution();
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(&session_, &CompositionSession::selectionChanged, this, [this] {
        rebuildObjectSelector();
        ++gpuOverlayRevision_;
        updateGpuResidentPresentation();
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
        resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));
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
    connect(&previewController_, &CompositionPreviewController::stateChanged, this,
            &ViewerEditor::refreshViewAdjustment);
    updatePreviewResolution();
    updatePreviewAccessibility();
    updateOverlayActions();

    // GPU-resident presentation is inert until setGpuPresentationDependencies() supplies a live
    // client. It never creates a device/thread here; the adapter owns the only native objects.
    gpuResident_ = std::make_unique<ViewerGpuResidentController>();
    gpuResident_->setPresentAck([this](const bool presented, const std::string&) {
        // True only after the owner genuinely published a present for this request.
        residentActive_ = presented;
        update();
    });
    gpuResident_->setCpuFallback([this] { requestResidentCpuFallback(); });
    gpuResident_->setCpuCoverSnapshot([this]() -> QPixmap { return renderCpuCoverSnapshot(); });
    gpuResident_->setInputSink(
        [this](const ViewerGpuInputEvent& event) { forwardGpuInput(event); });
    gpuResident_->setStateChanged([this] { updateGpuResidentPresentation(); });
    gpuResidentTimer_ = new QTimer(this);
    gpuResidentTimer_->setInterval(16);
    connect(gpuResidentTimer_, &QTimer::timeout, this, &ViewerEditor::pollGpuResident);
}

ViewerEditor::~ViewerEditor() {
    if (gpuResidentTimer_ != nullptr) {
        gpuResidentTimer_->stop();
    }
    if (cpuFallbackTask_.has_value()) {
        cpuFallbackTask_->cancel();
        cpuFallbackTask_.reset();
    }
    // The host retire-before-mutation gate must have already settled any live native target. A
    // target still live here is the ownership-contract violation ViewerGpuPresenter documents.
    gpuResident_.reset();
    gpuContainer_ = nullptr;
    clearProbe();
    if (adjustTask_)
        adjustTask_->cancel();
    finishTextEditing(true);
    cancelCreation();
    QObject::disconnect(focusConnection_);
}

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
    if (channelDropdown_ != nullptr &&
        channelDropdown_->currentIndex() != static_cast<int>(channel)) {
        const QSignalBlocker blocker(channelDropdown_);
        channelDropdown_->setCurrentIndex(static_cast<int>(channel));
    }
    if (viewerViewMenu_ != nullptr) {
        for (auto* action : viewerViewMenu_->findChildren<QAction*>()) {
            if (action->data().toInt() == static_cast<int>(channel) &&
                action->objectName().startsWith(QStringLiteral("viewerChannel"))) {
                action->setChecked(true);
            }
        }
    }
    updateGpuResidentPresentation();
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
    if (backgroundDropdown_ != nullptr &&
        backgroundDropdown_->currentIndex() != static_cast<int>(background)) {
        const QSignalBlocker blocker(backgroundDropdown_);
        backgroundDropdown_->setCurrentIndex(static_cast<int>(background));
    }
    if (viewerViewMenu_ != nullptr) {
        for (auto* action : viewerViewMenu_->findChildren<QAction*>()) {
            if (action->data().toInt() == static_cast<int>(background) &&
                action->objectName().startsWith(QStringLiteral("viewerBackground"))) {
                action->setChecked(true);
            }
        }
    }
    updateGpuResidentPresentation();
    update();
}

void ViewerEditor::applyApplicationPreferences(const ApplicationPreferences& preferences) {
    // Drive the panel's own controls rather than reaching past them, so their existing
    // persistence and the preview controller's resolution update stay the one code path. An index
    // equal to the current one emits nothing, which is what makes this idempotent.
    const auto resolutionIndex = [&preferences] {
        switch (preferences.viewerResolution) {
        case ViewerResolutionPreference::Full:
            return 1;
        case ViewerResolutionPreference::Half:
            return 2;
        case ViewerResolutionPreference::Quarter:
            return 3;
        case ViewerResolutionPreference::Auto:
            break;
        }
        return 0;
    }();
    if (resolutionDropdown_ != nullptr && resolutionDropdown_->currentIndex() != resolutionIndex) {
        resolutionDropdown_->setCurrentIndex(resolutionIndex);
    }

    const auto backgroundIndex = [&preferences] {
        switch (preferences.viewerBackground) {
        case ViewerBackgroundPreference::Checkerboard:
            return 1;
        case ViewerBackgroundPreference::Black:
            return 2;
        case ViewerBackgroundPreference::White:
            return 3;
        case ViewerBackgroundPreference::Solid:
            break;
        }
        return 0;
    }();
    if (backgroundDropdown_ != nullptr && backgroundDropdown_->currentIndex() != backgroundIndex) {
        backgroundDropdown_->setCurrentIndex(backgroundIndex);
    }

    const auto syncAction = [](QAction* action, const bool desired) {
        if (action != nullptr && action->isChecked() != desired) {
            action->setChecked(desired);
        }
    };
    syncAction(safeAreasAction_, preferences.viewerSafeAreas);
    syncAction(centreCrossAction_, preferences.viewerCentreCross);
    syncAction(thirdsAction_, preferences.viewerThirds);
    syncAction(rulersAction_, preferences.viewerRulers);
    syncAction(pixelGridAction_, preferences.viewerPixelGrid);
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

    // hosted footer widget, this widget's own rect no longer reserves any space for it at all.
    if (statusBarFooterTaken_) {
        return {};
    }
    const qreal barHeight = kit::px(kit::Size::FooterRow);
    return QRectF(0.0, static_cast<qreal>(height()) - barHeight, static_cast<qreal>(width()),
                  barHeight);
}

QRectF ViewerEditor::contentRect() const {
    const QRectF bar = statusBarRect();
    return QRectF(rect()).adjusted(kit::px(kit::Size::ToolColumnWidth), 0.0, 0.0, -bar.height());
}

QRectF ViewerEditor::canvasRect() const {
    const QRectF bar = statusBarRect();
    const qreal padding = kit::px(kit::Size::ViewerWorkPadding);
    return QRectF(rect()).adjusted(padding + kit::px(kit::Size::ToolColumnWidth), padding, -padding,
                                   -bar.height() - padding);
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
        resolutionDropdown_->setToolTip(viewerResolutionText(previewController_));
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
    // A resident frame is presented at its native resolution. Auto stays stable Full so a zoom/pan
    // only updates the present parameters (destination/source) and never forces a re-evaluation or
    // an implicit proxy resolution change. Half/Quarter remain explicit choices made elsewhere.
    if (residentFrameIsDisplayed()) {
        return;
    }
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
    if (textEdit_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (textEdit_ && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        finishTextEditing(true);
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::Hide || event->type() == QEvent::DevicePixelRatioChange)
        cancelCreation();
    if (dragActive_ && (event->type() == QEvent::UngrabMouse ||
                        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        endDrag(false);
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (dragActive_) {
            endDrag(false);
        }
        updatePreviewResolution();
        updateGpuResidentPresentation();
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
    painter.fillRect(rect(), kit::color(kit::Color::Canvas));

    // The surround fills EVERY pixel of the content area -- right of the tool column, above the
    // footer -- not just the padded fit rectangle (owner, 2026-09-15: "it should be 100% to
    // viewer content width and height, not partial"). canvasRect() stays the fit target only.
    const QRectF surround = contentRect();
    const QRectF frame = canvasRect();
    const auto* composition = session_.composition();

    if (composition == nullptr) {
        // Honest empty state (decision 5): no evaluation warnings, no busywork -- a quiet,
        // product-neutral invitation. Muted ink, Ui type (Value/Geist Mono is reserved for
        // numeric/timecode surfaces, not prose -- kit/tokens.hpp).
        drawCanvasBackground(painter, surround, background_);
        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.setPen(kit::color(kit::Color::Muted));
        painter.drawText(frame, Qt::AlignCenter, tr("Create a layer to begin"));
        return;
    }

    drawCanvasBackground(painter, surround, background_);

    // While a native present is genuinely active the child QWindow occludes this widget's painting
    // and owns the pixels AND the overlays (they were recorded into the native overlay). Do not
    // paint a CPU image behind it.
    if (residentActive_ && !coverSnapshotInProgress_) {
        return;
    }

    paintViewerContent(painter);
}

void ViewerEditor::paintViewerContent(QPainter& painter) {
    // During a cover snapshot the last valid CPU frame is drawn explicitly (never the resident arm,
    // whose CPU span is empty and whose native child is excluded from this pixmap anyway).
    const PreparedPreviewFrameHandle displayedFrame =
        coverSnapshotInProgress_
            ? (cpuFallbackFrame_ != nullptr ? cpuFallbackFrame_ : lastCpuFrame_)
            : paintableCpuFrame();
    const QRectF frame = canvasRect();
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
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(displayRect, shownImage, QRectF(shownImage.rect()));
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.setPen(QPen(kit::color(kit::Color::CompositionFrame),
                                        kit::kCompositionFrameWidth));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(displayRect.adjusted(0.0, 0.0, -kit::kCompositionFrameWidth,
                                                          -kit::kCompositionFrameWidth));
                    if (geometry.has_value()) {
                        const auto bounds = previewController_.selectedLayerBounds();
                        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
                            bufferView->displayWindow, bufferView->pixelAspect);
                        if (descriptor && session_.composition()) {
                            const ViewerMapping mapping{
                                displayRect, session_.composition()->format(),
                                displayedFrame->desiredIdentity().resolution,
                                bufferView->pixelAspect, *descriptor.value()};
                            painter.save();
                            painter.setClipRect(contentRect());
                            paintViewerOverlays(
                                painter, frame, mapping, effectiveZoom(displayRect, *geometry),
                                overlayOptions_,
                                textEdit_ ? std::span<const runtime::EvaluatedOperationBounds>{}
                                          : bounds,
                                pointTextLayers(session_, bounds));
                            painter.restore();
                        }
                    }
                }
            }
        }
    }

    paintRoi(painter);
    paintCreation(painter);
    paintPathTools(painter);
    paintTextEditing(painter);

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

std::optional<ViewerMapping> ViewerEditor::currentMapping() const {
    const auto& preview = previewController_.state();
    const PreparedPreviewFrameHandle& frameHandle = preview.frame;
    if (frameHandle == nullptr) {
        return std::nullopt;
    }
    // A stale frame from another composition, another project, or an older evaluation is never a
    // mapping source (docs/architecture/animation-and-time.md, "Direct Manipulation And Preview
    // Overrides"). A committed frame honestly carries the retained evaluation snapshot's revision,
    // which a verified layout-only edit may have left behind the live revision; an interactive
    // frame carries the live revision. Both name THIS live document, so requiring the project id to
    // agree -- not just the numeric revision -- is what keeps an old project's frame from ever
    // mapping, even when its revision number collides.
    const auto& desired = frameHandle->desiredIdentity();
    const auto& live = session_.snapshot();
    // TEMPORAL-2B: the genuine snapshot is resolved for the frame's OWN time, so a frame retained
    // at an older revision inside an unaffected segment still maps; an interactive frame carries
    // live.
    const auto& evaluation = session_.evaluationSnapshotForTime(desired.time);
    if (desired.compositionId != session_.compositionId() ||
        desired.projectId != live.project().id() ||
        desired.projectId != evaluation.project().id() ||
        (desired.sourceRevision != live.revision() &&
         desired.sourceRevision != evaluation.revision()) ||
        desired.time != session_.currentTime()) {
        return std::nullopt;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    // The gesture-mapping geometry is alternative-agnostic (design decision 2): a qualified frame's
    // window/pixel-aspect maps a drag gesture exactly the way a reference frame's does. The frozen
    // ViewerMapping::displayDescriptor stays a
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
    // handed to CompositionSession::beginTransformInteraction(), and frozen there for the gesture's
    // duration; ViewerMapping's own equality (already comparing displayRect) is what
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
    return ViewerMapping{
        .displayRect = displayRect,
        .compositionFormat = composition->format(),
        .resolution = frameHandle->desiredIdentity().resolution,
        .pixelAspect = descriptor.pixelAspect(),
        .displayDescriptor = descriptor,
    };
}

ViewerHit ViewerEditor::hitAt(const ViewerMapping& mapping, const QPointF point) const {
    std::vector<runtime::EvaluatedOperationBounds> ordered;
    const auto& frame = previewController_.state().frame;
    if (!frame)
        return {};
    // SPLIT-2: read geometry translated to the CURRENT live graph, then order by the live Merge
    // entries. The raw frame bounds carry the retained snapshot's layer IDs, which after an
    // equivalent split name the head.
    const auto bounds = previewController_.currentLayerBounds();
    if (const auto* stack = session_.timelineMerge()) {
        for (const auto& entry : stack->entries()) {
            const auto found = std::ranges::find(bounds, entry.layerId,
                                                 &runtime::EvaluatedOperationBounds::layerId);
            if (found != bounds.end())
                ordered.push_back(*found);
        }
    }
    const auto selected = previewController_.selectedLayerBounds();
    return hitTestViewer(mapping, point, ordered, selected, pointTextLayers(session_, selected));
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
        (void)session_.commitTransformInteraction();
    } else {
        session_.cancelTransformInteraction();
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

void ViewerEditor::selectTool(const Tool tool) {
    finishTextEditing(true);
    cancelCreation();
    if (dragActive_)
        endDrag(false);
    tool_ = tool;
    for (auto* button : toolColumn_->findChildren<QToolButton*>())
        button->setChecked(button->property("viewerTool").toInt() == static_cast<int>(tool));
    updatePanCursor();
    setFocus(Qt::ShortcutFocusReason);
    update();
}

void ViewerEditor::updatePanCursor() {
    if (panActive_) {
        setCursor(Qt::ClosedHandCursor);

    } else if (tool_ == Tool::Hand) {
        setCursor(Qt::OpenHandCursor);
    } else if (tool_ == Tool::Zoom) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
}

void ViewerEditor::mousePressEvent(QMouseEvent* event) {
    if (roiPress(event))
        return;
    if (textEditPress(event))
        return;
    if (textPress(event))
        return;
    if (pathPress(event))
        return;
    if (creationPress(event))
        return;
    if (dragActive_ && event->button() == Qt::RightButton) {
        endDrag(false);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton)
        setFocus(Qt::MouseFocusReason);
    if (!dragActive_ && !panActive_) {
        if (const auto geometry = currentDisplayGeometry();
            geometry.has_value() && (event->button() == Qt::MiddleButton ||
                                     (event->button() == Qt::LeftButton && tool_ == Tool::Hand))) {
            beginPan(event->button(), event->position(), *geometry);
            event->accept();
            return;
        }
    }

    if (!dragActive_ && !panActive_ && tool_ == Tool::Zoom && event->button() == Qt::LeftButton) {
        if (const auto geometry = currentDisplayGeometry()) {
            transform_ =
                zoomAboutPoint(transform_, canvasRect(), geometry->extent, geometry->pixelAspect,
                               event->position(),
                               event->modifiers().testFlag(Qt::AltModifier) ? 1.0 / kZoomStepFactor
                                                                            : kZoomStepFactor);
            refreshZoomDropdown();
            update();
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton || tool_ != Tool::Select || dragActive_) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    auto mapping = currentMapping();
    if (!mapping.has_value() || !contentRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto hit = hitAt(*mapping, event->position());
    if (hit.region == ViewerHitRegion::Empty) {
        session_.clearSelection();
        event->accept();
        return;
    }
    session_.selectLayer(hit.layerId, event->modifiers().testFlag(Qt::ShiftModifier));
    const auto selected = previewController_.selectedLayerBounds();
    const auto bounds =
        std::ranges::find(selected, hit.layerId, &runtime::EvaluatedOperationBounds::layerId);
    if (bounds == selected.end())
        return;
    auto kind = TransformGesture::Kind::Move;
    switch (hit.region) {
    case ViewerHitRegion::Scale:
        kind = TransformGesture::Kind::Scale;
        break;
    case ViewerHitRegion::Rotate:
        kind = TransformGesture::Kind::Rotate;
        break;
    case ViewerHitRegion::Anchor:
        kind = TransformGesture::Kind::Anchor;
        break;
    default:
        break;
    }
    if (session_.beginTransformInteraction({kind, hit.handle, event->position(), *bounds}, *mapping,
                                           {event->modifiers().testFlag(Qt::ShiftModifier),
                                            event->modifiers().testFlag(Qt::AltModifier)})) {
        event->accept();
        return;
    }
    dragActive_ = true;
    activeMapping_ = mapping;
    setFocus(Qt::MouseFocusReason);
    previewController_.beginInteractiveScrub();
    event->accept();
}

void ViewerEditor::mouseMoveEvent(QMouseEvent* event) {
    refreshProbe(event->position());
    if (roiMove(event))
        return;
    if (textEdit_) {
        if (textEdit_->dragging)
            moveTextCaret(event->position(), true);
        event->accept();
        return;
    }
    if (pathMove(event))
        return;
    if (creationMove(event))
        return;
    if (panActive_) {
        transform_ = panBaseTransform_;
        transform_.pan += (event->position() - panOrigin_);
        refreshZoomDropdown();
        update();
        event->accept();
        return;
    }
    if (!dragActive_) {
        if (tool_ == Tool::Select) {
            const auto mapping = currentMapping();
            const auto hit = mapping ? hitAt(*mapping, event->position()) : ViewerHit{};
            switch (hit.region) {
            case ViewerHitRegion::Move:
                setCursor(Qt::SizeAllCursor);
                break;
            case ViewerHitRegion::Anchor:
                setCursor(Qt::CrossCursor);
                break;
            case ViewerHitRegion::Rotate:
                setCursor(Qt::OpenHandCursor);
                break;
            case ViewerHitRegion::Scale:
                switch (hit.handle % 4) {
                case 0:
                    setCursor(Qt::SizeFDiagCursor);
                    break;
                case 1:
                    setCursor(Qt::SizeVerCursor);
                    break;
                case 2:
                    setCursor(Qt::SizeBDiagCursor);
                    break;
                default:
                    setCursor(Qt::SizeHorCursor);
                    break;
                }
                break;
            case ViewerHitRegion::Empty:
                unsetCursor();
                break;
            }
        }
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
    session_.updateTransformInteraction(event->position(),
                                        {event->modifiers().testFlag(Qt::ShiftModifier),
                                         event->modifiers().testFlag(Qt::AltModifier)});
    event->accept();
}

void ViewerEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (roiRelease(event))
        return;
    if (textEdit_ && event->button() == Qt::LeftButton) {
        textEdit_->dragging = false;
        event->accept();
        return;
    }
    if (pathRelease(event))
        return;
    if (creationRelease(event))
        return;
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
    if (mappingStillValid()) {
        session_.updateTransformInteraction(event->position(),
                                            {event->modifiers().testFlag(Qt::ShiftModifier),
                                             event->modifiers().testFlag(Qt::AltModifier)});
        endDrag(true);
    } else {
        endDrag(false);
    }
    event->accept();
}

void ViewerEditor::wheelEvent(QWheelEvent* event) {
    if (dragActive_ || panActive_ || creation_) {
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
    if (roiGesture_ && event->key() == Qt::Key_Escape) {
        roiGesture_.reset();
        update();
        event->accept();
        return;
    }
    if (textEditKey(event))
        return;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        tool_ == Tool::Select && beginTextEditing()) {
        event->accept();
        return;
    }
    if (pathKey(event))
        return;
    if (!dragActive_ && !panActive_ && event->modifiers() == Qt::NoModifier) {
        std::optional<Tool> next;
        switch (event->key()) {
        case Qt::Key_V:
        case Qt::Key_Escape:
            next = Tool::Select;
            break;
        case Qt::Key_H:
            next = Tool::Hand;
            break;
        case Qt::Key_Z:
            next = Tool::Zoom;
            break;
        case Qt::Key_R:
            next = Tool::Rectangle;
            break;
        case Qt::Key_E:
            next = Tool::Ellipse;
            break;
        case Qt::Key_P:
            next = Tool::Pen;
            break;
        case Qt::Key_T:
            next = Tool::Text;
            break;
        default:
            break;
        }
        if (next) {
            selectTool(*next);
            event->accept();
            return;
        }
    }
    if (dragActive_ && event->key() == Qt::Key_Escape) {
        endDrag(false);
        event->accept();
        return;
    }
    if (!dragActive_ && !panActive_) {

        if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier) {
            if (event->key() == Qt::Key_Delete) {
                std::set<document::NodeId> nodes;
                for (const auto node : session_.selectedNodes())
                    if (session_.layerForNode(node))
                        nodes.insert(node);
                if (!nodes.empty()) {
                    commands::Transaction transaction("Delete Layers",
                                                      session_.snapshot().revision());
                    transaction.emplace<commands::RemoveNodes>(session_.compositionId(),
                                                               std::move(nodes));
                    (void)session_.executeTransaction(std::move(transaction));
                }
                event->accept();
                return;
            }
            QPointF delta;
            const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
            switch (event->key()) {
            case Qt::Key_Left:
                delta.setX(-step);
                break;
            case Qt::Key_Right:
                delta.setX(step);
                break;
            case Qt::Key_Up:
                delta.setY(-step);
                break;
            case Qt::Key_Down:
                delta.setY(step);
                break;
            default:
                break;
            }
            if (!delta.isNull()) {
                nudgeViewerSelection(session_, delta);
                event->accept();
                return;
            }
        }

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
    cancelCreation();
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
    updateGpuResidentPresentation();
}

void ViewerEditor::contextMenuEvent(QContextMenuEvent* event) {
    // Kit-styled via the application-wide QMenu stylesheet rule every other Bloom context/popup
    // menu already picks up (e.g. TimelineEditor's "Add Layer" menu,
    // composition_editor_support.cpp) -- no per-menu styling code needed here. Honest,
    // placeholder-free set only (decision 4): no RAM-preview/channel/quality slots, which do not
    // exist yet.
    std::unique_ptr<QMenu> menuOwner(kit::makeMenu(this));
    auto& menu = *menuOwner;
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

#include "viewer_tools.ipp"
#include "viewer_tools_path.ipp"

#include "viewer_editor_gpu.ipp"

} // namespace bloom::ui
