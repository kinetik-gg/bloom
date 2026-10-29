#pragma once

#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QLineEdit>
#include <QPlainTextEdit>

class QAction;

namespace bloom::ui::kit {

// The kit's one text field: a Field well in the control body with Spacing::S padding and
// Radius::Small corners, the single border by the kit-wide rule (BorderHover under the pointer,
// Accent while it has focus -- a focused text field is being edited), Faint placeholder ink, and
// an Error border with a reason (setError) for a value a form or an inline editor refuses. The
// look is generated from kit::fieldColors() into the application sheet, so a typed name and a
// typed number (KValueField) read the same.
//
// The type role follows the content: Ui for names and prose, Value (monospaced) for numbers,
// hex, timecode and other machine text. An inline editor -- a rename, an in-place value -- installs
// the kit inline-edit policy with useInlineEdit() (Escape confirms everywhere); a field in a form
// does not need it, since the form's own buttons accept or reject.
class KTextField : public QLineEdit {
    Q_OBJECT

  public:
    explicit KTextField(QWidget* parent = nullptr);
    explicit KTextField(const QString& text, QWidget* parent = nullptr);

    void setTypeRole(TypeRole role);
    [[nodiscard]] TypeRole typeRole() const noexcept { return role_; }

    // Shows the Error border, and the reason as the tooltip and accessible description. An empty
    // reason clears the state.
    void setError(const QString& reason);
    [[nodiscard]] QString error() const;

    // A field drawn by its host (the editor inside a KValueField): no box, no padding, a
    // transparent background, only the text and its selection.
    void setEmbedded(bool embedded);
    [[nodiscard]] bool isEmbedded() const noexcept { return embedded_; }

    // Installs the inline-edit policy once and returns it; a later call replaces the validator.
    KInlineEdit& useInlineEdit(KInlineEdit::Validator validator = {});
    [[nodiscard]] KInlineEdit* inlineEdit() const noexcept { return inline_; }

  private:
    TypeRole role_ = TypeRole::Ui;
    bool embedded_ = false;
    QString restingToolTip_;
    KInlineEdit* inline_ = nullptr;
};

// The historical name of KTextField, kept so every existing caller and forward declaration
// compiles unchanged. New code may use either.
class KLineEdit : public KTextField {
    Q_OBJECT

  public:
    explicit KLineEdit(QWidget* parent = nullptr);
    explicit KLineEdit(const QString& text, QWidget* parent = nullptr);
};

// A search box: KTextField with a leading magnifier glyph and a trailing clear button that shows
// only while there is text. Escape clears the text first; a second Escape (or one on an empty
// field) is left for the surface around it -- a popup closes, a panel takes focus back -- and is
// announced by escaped().
class KSearchField : public KLineEdit {
    Q_OBJECT

  public:
    explicit KSearchField(QWidget* parent = nullptr);
    [[nodiscard]] QAction* clearAction() const noexcept { return clear_; }

  Q_SIGNALS:
    void escaped();

  protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    QAction* search_ = nullptr;
    QAction* clear_ = nullptr;
};

// The multi-line text field: the same Field well, border rule, padding and error state as
// KTextField, for text that runs to several lines (a text layer's content, a job's diagnostics, a
// licence). Value role by default, since every multi-line text Bloom shows is content rather than
// interface prose.
class KTextArea : public QPlainTextEdit {
    Q_OBJECT

  public:
    explicit KTextArea(QWidget* parent = nullptr);
    void setTypeRole(TypeRole role);
    void setError(const QString& reason);
    KInlineEdit& useInlineEdit(KInlineEdit::Validator validator = {});
    [[nodiscard]] KInlineEdit* inlineEdit() const noexcept { return inline_; }

  private:
    QString restingToolTip_;
    KInlineEdit* inline_ = nullptr;
};

} // namespace bloom::ui::kit
