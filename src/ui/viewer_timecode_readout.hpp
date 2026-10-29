#pragma once
// Private to the Viewer's own translation units.

#include "composition_editor_support.hpp"
#include "viewer_editor_internal.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QStackedLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>

namespace bloom::ui {
// The Viewer footer's frame field (task VIEW-1; lane U): the current frame in a Field-recipe box one
// control tall at the end of the transport cluster, with the composition's duration after it in
// muted ink -- "120 / 240", or "00:00:05:00 / 00:00:10:00" in the timecode reading. Click to type an
// exact frame or timecode; the context menu chooses the reading.
//
// Its width is fixed from the composition's LONGEST value (the last frame's label and the duration),
// recomputed only when the composition or the reading changes: the text changes on every frame of
// playback, and a width that tracked it would relayout the whole footer sixty times a second and
// make every control beside it twitch. The current value right-aligns in that column, so a growing
// frame number never moves the duration either.
//
// Not in an anonymous namespace because ViewerEditor holds one by pointer and viewer_editor.hpp
// therefore forward-declares it. No Q_OBJECT: it has no signals or slots of its own -- the seek is
// a callback supplied by its owner -- so it needs no moc pass.
class ViewerTimecodeReadout final : public kit::KFieldFrame {
  public:
    // Called with an exact, already-clamped frame index when the artist commits an edit. The
    // owner is what pauses playback and writes session time; this widget never touches either.
    using SeekFunction = std::function<void(std::uint64_t)>;

    ViewerTimecodeReadout(CompositionSession& session, SeekFunction seek, QWidget* parent)
        : kit::KFieldFrame(parent), session_(session), seek_(std::move(seek)) {
        setObjectName(QStringLiteral("viewerTimeReadout"));
        setAccessibleName(ViewerEditor::tr("Current frame and time"));
        setCursor(Qt::IBeamCursor);
        timecodeFormat_ =
            QSettings()
                .value(QLatin1StringView(viewer_detail::kTimeFormatSetting),
                       QStringLiteral("timecode"))
                .toString() == QStringLiteral("timecode");

        stack_ = new QStackedLayout(this);
        stack_->setContentsMargins(0, 0, 0, 0);

        // The resting reading: the current value and the muted duration, padded inside the box
        // exactly as the editor pads its text, so opening the editor never moves the digits.
        page_ = new QWidget(this);
        auto* row = new QHBoxLayout(page_);
        row->setContentsMargins(padding(), 0, padding(), 0);
        row->setSpacing(0);
        label_ = new kit::KLabel(page_);
        // Unchanged objectName: this is the same readout the timeline transport used to own, moved
        // rather than replaced (task VIEW-1's "they change parent, not identity").
        label_->setObjectName(QStringLiteral("timelineTimeReadout"));
        label_->setAccessibleName(ViewerEditor::tr("Current frame and time"));
        label_->setFont(kit::font(kit::TypeRole::Value));
        label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        duration_ = new kit::KLabel(page_);
        duration_->setObjectName(QStringLiteral("viewerTimeReadoutDuration"));
        duration_->setFont(kit::font(kit::TypeRole::Value));
        duration_->setTone(kit::Color::Muted);
        duration_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        row->addWidget(label_);
        row->addWidget(duration_, 1);
        // The click that starts an edit belongs to this widget, so the labels never eat it.
        page_->setAttribute(Qt::WA_TransparentForMouseEvents);

        // The editor is the kit line edit, which paints the same Field box in the same control
        // body -- focused, so its border is the Accent this box would show while editing. The box
        // itself (kit::KFieldFrame) steps aside meanwhile.
        editor_ = new kit::KLineEdit(this);
        editor_->setObjectName(QStringLiteral("viewerTimeReadoutEditor"));
        editor_->setAccessibleName(ViewerEditor::tr("Go to frame"));
        editor_->setFont(kit::font(kit::TypeRole::Value));
        editor_->setToolTip(ViewerEditor::tr("Enter a frame number or HH:MM:SS:FF timecode"));
        editor_->installEventFilter(this);

        stack_->addWidget(page_);
        stack_->addWidget(editor_);
        stack_->setCurrentWidget(page_);

        connect(editor_, &QLineEdit::returnPressed, this, [this] { commitEdit(); });

        // Built once and added to this widget's own action list rather than created inside
        // contextMenuEvent(): the menu is then assembled from actions that exist whether or not it
        // has ever been opened, which is what makes the format reachable by name -- to a test, and
        // to any future menu that wants to offer the same two choices.
        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        framesAction_ = new QAction(ViewerEditor::tr("Frames"), this);
        framesAction_->setObjectName(QStringLiteral("viewerFramesAction"));
        framesAction_->setToolTip(ViewerEditor::tr("The frame index, counted from the start"));
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
        QSettings().setValue(QLatin1StringView(viewer_detail::kTimeFormatSetting),
                             timecode ? QStringLiteral("timecode") : QStringLiteral("frames"));
        refresh();
    }

    // The exact current value on screen (the duration is durationText()), and the seam a test
    // reads instead of grabbing pixels.
    [[nodiscard]] QString text() const { return label_->text(); }
    [[nodiscard]] QString durationText() const { return duration_->text(); }

    void refresh() {
        const auto time = session_.currentTime();
        const auto context = frameContextFor(session_);
        // With no composition the field keeps its own format, disabled: the zero frame as it
        // would read and no duration, so the footer never shows a bare dash where the time belongs.
        QString frameText = timecodeFormat_ ? QStringLiteral("00:00:00:00") : QStringLiteral("0");
        QString durationText;
        QString longest = frameText;
        if (context.has_value()) {
            const auto nearest =
                nearestFrameIndexForTime(context->frameRate, context->duration, time);
            if (nearest.has_value()) {
                frameText = formatTimelineFrameLabel(*nearest, context->frameRate, timecodeFormat_,
                                                     context->labels);
            }
            longest = formatTimelineFrameLabel(context->maxFrameIndexValue, context->frameRate,
                                               timecodeFormat_, context->labels);
            // The duration is a length, not a label: the frame count, or that many frames as
            // timecode from zero, whatever the start timecode.
            document::CompositionTimecode lengthLabels;
            lengthLabels.dropFrame = context->labels.dropFrame;
            durationText = QStringLiteral(" / ") +
                           formatTimelineFrameLabel(context->maxFrameIndexValue + 1,
                                                    context->frameRate, timecodeFormat_,
                                                    lengthLabels);
        }
        setEnabled(context.has_value());
        setCursor(context.has_value() ? Qt::IBeamCursor : Qt::ArrowCursor);
        label_->setTone(context.has_value() ? kit::Color::Foreground : kit::Color::Faint);
        label_->setText(frameText);
        duration_->setText(durationText);
        duration_->setVisible(!durationText.isEmpty() && !compact_);
        fitWidth(longest, durationText);

        const bool dropFrame = context && context->labels.dropFrame &&
                               document::hasDropFrameTimecode(context->frameRate);
        const QString reading =
            timecodeFormat_
                ? (dropFrame ? ViewerEditor::tr("Current time / duration, drop-frame timecode")
                             : ViewerEditor::tr("Current time / duration, non-drop timecode"))
                : ViewerEditor::tr("Current frame / duration, in frames");
        setToolTip(context.has_value()
                       ? reading + QStringLiteral("\n") +
                             ViewerEditor::tr("Click to type a frame or timecode. Right-click to "
                                              "show frames or timecode.")
                       : ViewerEditor::tr("No composition"));
    }

    // The footer's narrowest form (a priority-row compact form): the current value alone, without
    // the duration, in a box that fits any timecode of the 99-hour range. The row measures it at
    // compactWidth() before it asks for it.
    void setCompact(const bool compact) {
        if (compact_ == compact)
            return;
        compact_ = compact;
        refresh();
    }
    [[nodiscard]] bool compact() const noexcept { return compact_; }
    [[nodiscard]] static int compactWidth() {
        return 2 * padding() + advance(QStringLiteral("00:00:00:00"));
    }

    // The full form's width; the field may be in its compact form, narrower than this.
    [[nodiscard]] QSize sizeHint() const override {
        return {fullWidth_, kit::px(kit::Size::Control)};
    }
    [[nodiscard]] QSize minimumSizeHint() const override {
        return {width(), kit::px(kit::Size::Control)};
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
        menu.setToolTipsVisible(true);
        menu.addAction(framesAction_);
        menu.addAction(timecodeAction_);
        menu.exec(event->globalPos());
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == editor_) {
            // Escape confirms, like Return and clicking away (owner decision, 2026-10-29): the
            // typed frame is taken, and an unparseable entry reverts in silence.
            if (event->type() == QEvent::FocusOut) {
                commitEdit();
            } else if (event->type() == QEvent::KeyPress &&
                       static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                commitEdit();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

  private:
    [[nodiscard]] static int padding() { return kit::KFieldFrame::textInset(); }
    [[nodiscard]] static int advance(const QString& text) {
        return static_cast<int>(
            std::ceil(QFontMetricsF(kit::font(kit::TypeRole::Value)).horizontalAdvance(text)));
    }

    // The box fits the composition's longest value and its duration, and nothing narrower: set
    // only when either changes, never per frame.
    void fitWidth(const QString& longest, const QString& durationText) {
        const int widest = std::max(advance(longest), advance(label_->text()));
        fullWidth_ = 2 * padding() + widest + (durationText.isEmpty() ? 0 : advance(durationText));
        const int width = compact_ ? compactWidth() : fullWidth_;
        const int suffix = compact_ || durationText.isEmpty() ? 0 : advance(durationText);
        const int column = width - padding() - padding() - suffix;
        label_->setFixedWidth(column);
        if (width != width_) {
            width_ = width;
            setFixedWidth(width_);
            updateGeometry();
        }
    }

    void beginEdit() {
        const auto context = frameContextFor(session_);
        if (!context.has_value()) {
            return;
        }
        const auto nearest =
            nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
        editor_->setText(formatTimelineFrameLabel(nearest.value_or(0), context->frameRate,
                                                  timecodeFormat_, context->labels));
        editOpen_ = true;
        setEditing(true);
        stack_->setCurrentWidget(editor_);
        editor_->selectAll();
        editor_->setFocus(Qt::MouseFocusReason);
    }

    void endEdit() {
        editOpen_ = false;
        setEditing(false);
        if (stack_->currentWidget() == editor_) {
            stack_->setCurrentWidget(page_);
        }
        update();
    }

    void commitEdit() {
        // Hiding the focused editor sends it a FocusOut, which comes back here: one commit only.
        if (!editOpen_)
            return;
        const auto context = frameContextFor(session_);
        endEdit();
        // A frame number or timecode in the composition's own numbering (start timecode,
        // drop-frame); an unparseable entry reverts in silence: there is nothing to report that
        // the unchanged readout does not already say.
        const auto typed = context ? parseTimelineFrameLabel(editor_->text(), context->frameRate,
                                                             timecodeFormat_, context->labels)
                                   : std::nullopt;
        if (!typed || !seek_)
            return;
        seek_(std::min<std::uint64_t>(*typed, context->maxFrameIndexValue));
    }

    CompositionSession& session_;
    SeekFunction seek_;
    QStackedLayout* stack_ = nullptr;
    QWidget* page_ = nullptr;
    kit::KLabel* label_ = nullptr;
    kit::KLabel* duration_ = nullptr;
    QLineEdit* editor_ = nullptr;
    QAction* framesAction_ = nullptr;
    QAction* timecodeAction_ = nullptr;
    int width_ = 0;
    int fullWidth_ = 0;
    bool timecodeFormat_ = false;
    bool compact_ = false;
    bool editOpen_ = false;
};
} // namespace bloom::ui
