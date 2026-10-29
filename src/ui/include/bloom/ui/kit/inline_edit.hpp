#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>

class QLineEdit;
class QPlainTextEdit;
class QWidget;

namespace bloom::ui::kit {

// The one inline-edit policy (owner decision, 2026-10-29: Escape confirms everywhere). Every
// editor that opens in place over a value -- KValueField's text entry, a rename field, the Viewer
// frame field, the Properties text rows, the colour hex field -- installs one of these instead of
// spelling the keys out itself:
//
// - Return/Enter, Escape, Tab/Backtab and focus loss all COMMIT. Discarding a change is Undo.
// - Text the owner's validator refuses never silently reverts. Return or Tab with refused text
//   keeps the editor open, marks it with the Error border and states the reason in its tooltip and
//   accessible description. Escape or focus loss with refused text closes the editor and keeps the
//   last valid value, and a tooltip says the typed text was not taken.
// - A multi-line editor (setMultiline) takes Return as a line break and commits on Ctrl+Return
//   (Command+Return on macOS), Escape and focus loss.
// - A focus change to the editor's own context menu (Qt::PopupFocusReason) or a window switch
//   (Qt::ActiveWindowFocusReason) is not an ending; the edit continues when the window returns.
//
// The policy only decides; the owner commits and closes. It claims Escape, Return and Tab as
// shortcut overrides while the editor has focus, so a window shortcut on Escape (Cancel RAM
// Preview) never fires out from under a field being typed into.
class KInlineEdit final : public QObject {
    Q_OBJECT

  public:
    // Empty for acceptable text, otherwise the plain reason it is refused.
    using Validator = std::function<QString(const QString& text)>;

    enum class Ending : std::uint8_t {
        Return,
        Escape,
        Tab,
        Backtab,
        FocusOut,
    };
    Q_ENUM(Ending)

    explicit KInlineEdit(QLineEdit& editor, Validator validator = {});
    explicit KInlineEdit(QPlainTextEdit& editor, Validator validator = {});

    void setValidator(Validator validator);
    // Whether Tab and Backtab end the edit (the default). A host that moves focus itself after a
    // commit turns this off and handles Tab from accepted(Ending::Tab).
    void setTabEnds(bool ends);

    [[nodiscard]] QString text() const;
    // The reason currently shown on the editor; empty when it shows none.
    [[nodiscard]] QString refusal() const { return refusal_; }
    [[nodiscard]] bool hasRefusal() const noexcept { return !refusal_.isEmpty(); }
    // Validates the current text, and shows or clears the refusal accordingly. True when valid.
    bool validate();
    void clearRefusal();

    // The ending the owner asks for itself (a click elsewhere on a NoFocus surface, a host about to
    // be torn down): the same as a focus loss.
    void finish();

    // The validator every rename uses: a name must have at least one visible character.
    [[nodiscard]] static Validator nameValidator();

    // The refused-text note an owner shows when it closes an editor whose text was not taken.
    static void showRefusalNote(QWidget& near, const QString& text, const QString& reason);

  Q_SIGNALS:
    // The text is valid and the edit ends: commit it and close the editor.
    void accepted(const QString& text, bloom::ui::kit::KInlineEdit::Ending ending);
    // The edit ends with refused text: close the editor and keep the last valid value. The note
    // has already been shown.
    void refused(const QString& text, const QString& reason,
                 bloom::ui::kit::KInlineEdit::Ending ending);
    void refusalChanged(const QString& reason);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    KInlineEdit(QWidget& editor, Validator validator);
    void end(Ending ending, bool mayStayOpen);
    void showRefusal(const QString& reason);

    QPointer<QWidget> editor_;
    QLineEdit* line_ = nullptr;
    QPlainTextEdit* plain_ = nullptr;
    Validator validator_;
    QString refusal_;
    QString restingToolTip_;
    bool tabEnds_ = true;
    bool ending_ = false;
};

} // namespace bloom::ui::kit
