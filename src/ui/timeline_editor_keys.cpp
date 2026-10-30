// After Effects' Timeline keys (docs/ux/interaction-model.md, "Timeline"). They are Timeline
// keys, not window shortcuts: Qt::WidgetWithChildrenShortcut actions on the panel, so they answer
// while the layer table, the lanes or the graph have focus, and a focused text or value field keeps
// its own typing (Qt hands it the ShortcutOverride first). The Viewer and the context bar keep the
// tool letters R, E, P and T, which they read only while they hold focus; the eventFilter below
// adds the same keys when no widget in the window has focus at all.
#include <bloom/ui/timeline_editor.hpp>

#include "timeline_reveal.hpp"

#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QKeyEvent>

namespace bloom::ui {
namespace {
using timeline_reveal::PlayheadEdit;
using timeline_reveal::Reveal;
} // namespace

void TimelineEditor::createTimelineKeys() {
    const auto key = [this](const QString& name, const QKeySequence& sequence, auto callback) {
        auto* action = new QAction(this);
        action->setObjectName(name);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
        keyActions_.push_back(action);
    };
    struct RevealKey {
        Qt::Key key;
        Reveal reveal;
        const char* name;
    };
    for (const auto& [letter, reveal, name] : {RevealKey{Qt::Key_A, Reveal::Anchor, "Anchor"},
                                               RevealKey{Qt::Key_P, Reveal::Position, "Position"},
                                               RevealKey{Qt::Key_S, Reveal::Scale, "Scale"},
                                               RevealKey{Qt::Key_R, Reveal::Rotation, "Rotation"},
                                               RevealKey{Qt::Key_T, Reveal::Opacity, "Opacity"},
                                               RevealKey{Qt::Key_U, Reveal::Keyed, "Keyed"},
                                               RevealKey{Qt::Key_M, Reveal::Masks, "Masks"},
                                               RevealKey{Qt::Key_E, Reveal::Effects, "Effects"}}) {
        for (const bool additive : {false, true}) {
            key(QStringLiteral("timelineReveal%1%2")
                    .arg(QLatin1StringView(name), additive ? QStringLiteral("Add") : QString()),
                QKeySequence(additive ? (Qt::SHIFT | letter) : QKeyCombination(letter)),
                [this, reveal, additive] { revealProperties(static_cast<int>(reveal), additive); });
        }
    }
    key(QStringLiteral("timelinePreviousKeyframeAction"), QKeySequence(Qt::Key_J),
        [this] { stepToKeyframe(false); });
    key(QStringLiteral("timelineNextKeyframeAction"), QKeySequence(Qt::Key_K),
        [this] { stepToKeyframe(true); });
    key(QStringLiteral("timelineMoveInToPlayheadAction"), QKeySequence(Qt::Key_BracketLeft),
        [this] { editTimingAtPlayhead(static_cast<int>(PlayheadEdit::MoveIn)); });
    key(QStringLiteral("timelineMoveOutToPlayheadAction"), QKeySequence(Qt::Key_BracketRight),
        [this] { editTimingAtPlayhead(static_cast<int>(PlayheadEdit::MoveOut)); });
    key(QStringLiteral("timelineTrimInToPlayheadAction"),
        QKeySequence(Qt::ALT | Qt::Key_BracketLeft),
        [this] { editTimingAtPlayhead(static_cast<int>(PlayheadEdit::TrimIn)); });
    key(QStringLiteral("timelineTrimOutToPlayheadAction"),
        QKeySequence(Qt::ALT | Qt::Key_BracketRight),
        [this] { editTimingAtPlayhead(static_cast<int>(PlayheadEdit::TrimOut)); });
}

// The selected layers, or every listed layer when none is selected (as in After Effects).
std::vector<document::LayerId> TimelineEditor::keyTargetLayers() const {
    std::vector<document::LayerId> all;
    std::vector<document::LayerId> selected;
    const auto& primary = session_.selection().primary;
    const auto* direct = std::get_if<document::LayerId>(&primary);
    for (const auto& entry : stack_->entries()) {
        if (entry.rowKind != TimelineLayerEntry::Kind::Layer || !entry.layerId.isValid())
            continue;
        all.push_back(entry.layerId);
        const auto boundary = session_.boundaryNodeForLayer(entry.layerId);
        if ((boundary && session_.selectedNodes().contains(*boundary)) ||
            (direct && *direct == entry.layerId) ||
            session_.selection().contextualLayer == entry.layerId)
            selected.push_back(entry.layerId);
    }
    return selected.empty() ? all : selected;
}

void TimelineEditor::revealProperties(const int value, const bool additive) {
    auto reveal = static_cast<Reveal>(value);
    // U twice in quick succession is UU: the modified properties instead of the keyed ones.
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (reveal == Reveal::Keyed) {
        const bool twice = lastRevealWasKeyed_ && lastRevealKeyMs_ &&
                           now - *lastRevealKeyMs_ <= QApplication::doubleClickInterval();
        lastRevealWasKeyed_ = !twice;
        if (twice)
            reveal = Reveal::Modified;
    } else {
        lastRevealWasKeyed_ = false;
    }
    lastRevealKeyMs_ = now;
    for (const auto layer : keyTargetLayers()) {
        const auto found = revealed_.find(layer);
        std::optional<std::set<document::ParameterId>> current;
        if (found != revealed_.end())
            current = found->second;
        // UU replaces the U it follows rather than toggling it away.
        if (reveal == Reveal::Modified && !additive)
            current.reset();
        const auto next = timeline_reveal::nextReveal(
            current, timeline_reveal::revealedParameters(session_, layer, reveal), additive);
        if (next) {
            revealed_[layer] = *next;
            expandedLayers_.insert(layer);
        } else {
            revealed_.erase(layer);
            expandedLayers_.erase(layer);
        }
    }
    rebuild();
}

void TimelineEditor::stepToKeyframe(const bool forward) {
    // The keys the Timeline shows: a collapsed layer's summary and every listed property row's own.
    std::vector<core::RationalTime> times;
    const auto& entries = stack_->entries();
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const auto& entry = entries[static_cast<std::size_t>(row)];
        if (entry.rowKind == TimelineLayerEntry::Kind::Layer) {
            const auto summary = lanes_->keySummaryTimes(row);
            times.insert(times.end(), summary.begin(), summary.end());
        } else if ((entry.rowKind == TimelineLayerEntry::Kind::Parameter ||
                    entry.rowKind == TimelineLayerEntry::Kind::Component) &&
                   (keyframesVisible_ || graphEditor_)) {
            for (const auto& key : timeline_reveal::rowKeys(session_, entry))
                times.push_back(key.time);
        }
    }
    if (const auto target = timeline_reveal::adjacentTime(times, session_.currentTime(), forward))
        (void)session_.setCurrentTime(*target);
}

void TimelineEditor::editTimingAtPlayhead(const int value) {
    const auto edit = static_cast<PlayheadEdit>(value);
    const bool trim = edit == PlayheadEdit::TrimIn || edit == PlayheadEdit::TrimOut;
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return;
    commands::Transaction transaction(trim ? "Trim Layer" : "Move Layer",
                                      session_.snapshot().revision());
    bool any = false;
    for (const auto layer : keyTargetLayers()) {
        const auto* record = composition->graph().findLayer(layer);
        if (record == nullptr || record->locked)
            continue;
        const auto proposal = timeline_reveal::playheadTiming(session_, layer, edit);
        if (!proposal ||
            (proposal->start == record->timing.startTime && proposal->in == record->inPoint &&
             proposal->out == record->endPoint(composition->duration())))
            continue;
        transaction.emplace<commands::SetLayerTiming>(session_.compositionId(), layer,
                                                      proposal->start, proposal->in, proposal->out,
                                                      proposal->speed);
        any = true;
    }
    if (any)
        (void)session_.executeTransaction(std::move(transaction));
}

bool TimelineEditor::eventFilter(QObject* watched, QEvent* event) {
    // With no widget focused the window receives the key itself; the Timeline's keys answer then
    // too, and only then, so no panel's own keys are ever taken.
    if (watched == keyWindow_ && event->type() == QEvent::KeyPress &&
        QApplication::focusWidget() == nullptr && isVisible()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const QKeySequence pressed(key->keyCombination());
        for (auto* action : keyActions_)
            if (action->isEnabled() && action->shortcut() == pressed) {
                action->trigger();
                return true;
            }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace bloom::ui
