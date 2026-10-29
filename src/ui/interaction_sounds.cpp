#include <bloom/ui/interaction_sounds.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>

#include <QAbstractButton>
#include <QDialog>
#include <QEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

#include <chrono>

namespace bloom::ui {

namespace {

constexpr auto kHooked = "bloomSoundHooked";
constexpr auto kPressed = "bloomSoundPressed";

bool inToolColumn(const QWidget* widget) {
    for (const QWidget* p = widget->parentWidget(); p != nullptr; p = p->parentWidget()) {
        if (qobject_cast<const kit::KToolColumn*>(p) != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace

InteractionSounds::InteractionSounds(UiFeedback& feedback, QObject* parent)
    : QObject(parent), feedback_(feedback) {}

bool InteractionSounds::isKitControl(const QWidget* widget) const {
    for (const QWidget* p = widget; p != nullptr; p = p->parentWidget()) {
        if (qobject_cast<const QAbstractButton*>(p) != nullptr ||
            qobject_cast<const kit::KDropdown*>(p) != nullptr ||
            qobject_cast<const kit::KRadioGroup*>(p) != nullptr) {
            return true;
        }
    }
    return false;
}

void InteractionSounds::attach(QWidget& widget) { hook(widget); }

void InteractionSounds::hook(QWidget& widget) {
    if (widget.property(kHooked).toBool()) {
        return;
    }
    const auto remember = [&widget] { widget.setProperty(kHooked, true); };

    if (auto* button = qobject_cast<QAbstractButton*>(&widget)) {
        const bool toggle = qobject_cast<kit::KSwitch*>(button) != nullptr ||
                            qobject_cast<kit::KIconToggle*>(button) != nullptr;
        const bool plain = qobject_cast<kit::KButton*>(button) != nullptr ||
                           (qobject_cast<kit::KIconButton*>(button) != nullptr && !toggle &&
                            qobject_cast<kit::KMenuButton*>(button) == nullptr);
        if (!toggle && !plain) {
            return;
        }
        remember();
        // A checkable control cues only when a gesture pressed it first; setChecked() is silent.
        connect(button, &QAbstractButton::pressed, this,
                [button] { button->setProperty(kPressed, button->isCheckable()); });
        if (toggle) {
            connect(button, &QAbstractButton::toggled, this, [this, button](const bool checked) {
                if (!button->property(kPressed).toBool()) {
                    return;
                }
                button->setProperty(kPressed, false);
                if (qobject_cast<kit::KIconToggle*>(button) != nullptr && inToolColumn(button)) {
                    if (checked) {
                        feedback_.play(FeedbackEvent::ToolSwitch);
                    }
                    return;
                }
                feedback_.play(checked ? FeedbackEvent::ToggleOn : FeedbackEvent::ToggleOff);
            });
        } else {
            connect(button, &QAbstractButton::clicked, this, [this, button] {
                // A dialog's own buttons are covered by the dialog open/close cues.
                if (qobject_cast<QDialog*>(button->window()) == nullptr) {
                    feedback_.play(FeedbackEvent::ControlPress);
                }
            });
        }
    } else if (auto* dropdown = qobject_cast<kit::KDropdown*>(&widget)) {
        remember();
        connect(dropdown, &kit::KDropdown::activated, this,
                [this](int) { feedback_.play(FeedbackEvent::Select); });
    } else if (auto* group = qobject_cast<kit::KRadioGroup*>(&widget)) {
        remember();
        connect(group, &kit::KRadioGroup::currentIndexChanged, this, [this, group](int) {
            if (gestureTarget_ == group) {
                feedback_.play(FeedbackEvent::Select);
            }
        });
    }
}

bool InteractionSounds::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) {
        return false;
    }
    switch (event->type()) {
    case QEvent::Polish:
        hook(*widget);
        break;
    case QEvent::MouseButtonPress: {
        if (!widget->isEnabled() && isKitControl(widget)) {
            feedback_.play(FeedbackEvent::Disabled);
        }
        // A press anywhere but a control starts a canvas gesture (scrub, drag, marquee): the
        // interaction cues stay quiet until the button is released.
        feedback_.setInteractionSuppressed(UiFeedback::Suppression::Gesture,
                                           widget->isEnabled() && !isKitControl(widget));
        gestureTarget_ = widget;
        break;
    }
    case QEvent::KeyPress:
        gestureTarget_ = widget;
        break;
    case QEvent::MouseButtonRelease:
        feedback_.setInteractionSuppressed(UiFeedback::Suppression::Gesture, false);
        // Keep the target through this release (a segmented control commits on press, a button on
        // release), then forget it.
        QMetaObject::invokeMethod(this, [this] { gestureTarget_.clear(); }, Qt::QueuedConnection);
        break;
    case QEvent::Show:
    case QEvent::Hide:
        if (auto* dialog = qobject_cast<QDialog*>(widget); dialog != nullptr && dialog->isModal() &&
                                                           dialog->windowType() == Qt::Dialog) {
            if (event->type() == QEvent::Show) {
                const auto tone = dialog->property("kitPromptTone");
                // kit::KPromptTone::Error is 2.
                feedback_.play(tone.isValid() && tone.toInt() == 2 ? FeedbackEvent::Warning
                                                                   : FeedbackEvent::ModalOpen);
            } else if (!event->spontaneous()) {
                feedback_.play(FeedbackEvent::ModalClose);
            }
        }
        break;
    case QEvent::DragEnter:
        if (!dragActive_) {
            dragActive_ = true;
            feedback_.play(FeedbackEvent::DragStart);
        }
        break;
    case QEvent::DragLeave:
        // Moving between widgets leaves and enters again: only a pause ends the drag.
        QTimer::singleShot(std::chrono::milliseconds{200}, this, [this] { dragActive_ = false; });
        break;
    case QEvent::Drop:
        dragActive_ = false;
        feedback_.play(FeedbackEvent::Drop);
        break;
    default:
        break;
    }
    return false;
}

} // namespace bloom::ui
