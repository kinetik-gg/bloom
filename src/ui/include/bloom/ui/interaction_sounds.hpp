#pragma once

// Turns kit control gestures into UiFeedback interaction events without the kit knowing about
// sound. One instance, installed on the QApplication, watches widgets as they are polished and
// connects the signals of the kit controls it recognises (switches, checkboxes, icon toggles and
// tools, buttons, dropdowns, radio groups); it also sees modal dialogs open and close, a press on
// a disabled control, and drag enter/drop. Every cue still goes through UiFeedback, which applies
// the Preferences switches, so a muted category costs only a settings check.
//
// Only user gestures cue: a programmatic setChecked() or setCurrentIndex() is silent.

#include <bloom/ui/ui_feedback.hpp>

#include <QObject>
#include <QPointer>

class QWidget;

namespace bloom::ui {

class InteractionSounds final : public QObject {
    Q_OBJECT

  public:
    explicit InteractionSounds(UiFeedback& feedback, QObject* parent = nullptr);

    // Hooks an already-polished widget's own signals (what Polish does for later ones).
    void attach(QWidget& widget);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void hook(QWidget& widget);
    [[nodiscard]] bool isKitControl(const QWidget* widget) const;

    UiFeedback& feedback_;
    // The control that received the press/key being dispatched, so a signal it emits while handling
    // that event is known to be a user gesture.
    QPointer<QWidget> gestureTarget_;
    bool dragActive_ = false;
};

} // namespace bloom::ui
