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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QStackedLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

namespace bloom::ui {
// The footer's frame/time readout (task VIEW-1): a monospaced label you click to type an exact
// frame number into, whose context menu chooses between a frame index and non-drop timecode.
//
// Not in an anonymous namespace because ViewerEditor holds one by pointer and viewer_editor.hpp
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
                .value(QLatin1StringView(viewer_detail::kTimeFormatSetting),
                       QStringLiteral("timecode"))
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
        QSettings().setValue(QLatin1StringView(viewer_detail::kTimeFormatSetting),
                             timecode ? QStringLiteral("timecode") : QStringLiteral("frames"));
        refresh();
    }

    // The exact text on screen, and the seam a test reads instead of grabbing pixels.
    [[nodiscard]] QString text() const { return label_->text(); }

    void refresh() {
        const auto time = session_.currentTime();
        const auto context = frameContextFor(session_);
        // With no composition the readout keeps its own format, disabled: the zero frame as it
        // would read, so the footer never shows a bare dash where the time belongs.
        QString frameText = timecodeFormat_ ? QStringLiteral("00:00:00:00") : QStringLiteral("0");
        if (context.has_value()) {
            const auto nearest =
                nearestFrameIndexForTime(context->frameRate, context->duration, time);
            if (nearest.has_value()) {
                frameText = formatTimelineFrameLabel(*nearest, context->frameRate, timecodeFormat_,
                                                     context->labels);
            }
        }
        setEnabled(context.has_value());
        static_cast<kit::KLabel*>(label_)->setTone(context.has_value() ? kit::Color::Foreground
                                                                       : kit::Color::Faint);
        label_->setText(frameText);
        const bool dropFrame = context && context->labels.dropFrame &&
                               document::hasDropFrameTimecode(context->frameRate);
        label_->setToolTip(
            timecodeFormat_
                ? (dropFrame ? ViewerEditor::tr("Drop-frame timecode · exact composition time")
                             : ViewerEditor::tr("Non-drop timecode · exact composition time"))
                : ViewerEditor::tr("Frame index · exact composition time"));
    }

    [[nodiscard]] QSize sizeHint() const override {
        // A FIXED width from the widest string this readout can ever show, not the current text's
        // width: the text changes on every frame of playback, and a width that tracked it would
        // relayout the whole footer sixty times a second and make every control beside it twitch.
        return {kit::px(kit::Size::ViewerTimecodeWidth), kit::px(kit::Size::Control)};
    }
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

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
        editor_->setText(formatTimelineFrameLabel(nearest.value_or(0), context->frameRate,
                                                  timecodeFormat_, context->labels));
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
        cancelEdit();
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
    QLabel* label_ = nullptr;
    QLineEdit* editor_ = nullptr;
    QAction* framesAction_ = nullptr;
    QAction* timecodeAction_ = nullptr;
    bool timecodeFormat_ = false;
};
} // namespace bloom::ui
