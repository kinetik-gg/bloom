#pragma once

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QAbstractScrollArea;
class QGridLayout;
class QVBoxLayout;

namespace bloom::ui::kit {

class KButton;
class KCheckBox;
class KDialog;
class KSection;

// One value a dialog refuses: the field the reason belongs under (nullptr for the dialog as a
// whole) and the plain reason naming what to fix.
struct KRefusal final {
    QWidget* field = nullptr;
    QString reason;
};

// The kit look for a read-only document or list inside a dialog (a licence text, a component
// list): no frame, the Field surface, Foreground ink and the kit's constant scrollbars.
void styleDocumentView(QAbstractScrollArea& view);

// A KLabel that toggles its check box when clicked, so a check box's words are part of the
// control (a VA-API or audio switch reads and clicks as one row).
class KCheckLabel final : public KLabel {
    Q_OBJECT
  public:
    KCheckLabel(const QString& text, QAbstractButton& buddy, QWidget* parent = nullptr);

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QPointer<QAbstractButton> buddy_;
};

// A two-column dialog form. Labels are KLabels in one label column that every form of the same
// KDialog shares, left-aligned on every platform; fields grow to the right. Rows are laid out in
// the order they are added, and the dialog's Tab order follows that visual order whatever order
// the widgets were constructed in. Each labelled row owns an inline reason (UiSmall, Error ink,
// Spacing::XXS under the field, hidden while valid) that the dialog's validator fills.
class KForm final : public QWidget {
    Q_OBJECT
  public:
    KForm(KDialog& dialog, QWidget* parent);

    // A labelled row; returns the field's reason label.
    KLabel* addRow(const QString& label, QWidget* field);
    // A row in the field column without a label of its own (a summary line, a check row).
    KLabel* addRow(QWidget* field);
    // A row spanning both columns (a note, a progress row).
    void addWideRow(QWidget* widget);
    // [check][clickable words] in the field column; the label toggles the check box.
    KLabel* addCheckRow(const QString& label, KCheckBox& box, const QString& text);

    void setRowVisible(QWidget* field, bool visible);
    [[nodiscard]] bool isRowVisible(QWidget* field) const;
    [[nodiscard]] KLabel* labelFor(QWidget* field) const;
    [[nodiscard]] KLabel* reasonFor(QWidget* field) const;
    // Every focusable widget in visual order: rows top to bottom, a row left to right.
    [[nodiscard]] std::vector<QWidget*> tabSequence() const;

  private:
    struct Row final {
        KLabel* label = nullptr;
        QWidget* holder = nullptr;
        QWidget* field = nullptr;
        KLabel* reason = nullptr;
    };
    KLabel* place(KLabel* label, QWidget* widget, QWidget* key, bool reason);
    [[nodiscard]] const Row* rowFor(QWidget* field) const;

    KDialog& dialog_;
    QGridLayout* grid_ = nullptr;
    std::vector<Row> rows_;
};

// The one dialog primitive (lane C). A QDialog with a native frame, a required owner (resolved to
// its top-level window), explicit modality, Spacing::M margins and Spacing::S between rows, a
// width taken only from DialogTextWidth / DialogWideWidth, and a QDialogButtonBox holding kit
// buttons so the platform orders them while the kit paints them.
//
// Keys (docs/ux/interaction-model.md, "Dialogs"):
// - Return/Enter presses the focused button-row button, else the default button; it does nothing
//   while that button is disabled, which is what "accept refused while invalid" means.
// - Ctrl+Return / Ctrl+Enter (Cmd on macOS) always presses Accept, from any field including a
//   multi-line one or an open value-field editor; an open editor commits first.
// - Escape rejects the dialog. A field whose own editor is open takes the first Escape (it closes
//   the editor and stays in the dialog); the next Escape rejects.
//
// open(done) is the default: window-modal, asynchronous, deleted when it closes. exec() remains
// for the documented sites that must hand a synchronous answer back to their caller.
class KDialog : public QDialog {
    Q_OBJECT
  public:
    enum class Width : std::uint8_t { Text, Wide };
    enum class Role : std::uint8_t { Accept, Reject, Destructive, Alternate, Apply, Reset };
    using Validator = std::function<std::optional<KRefusal>()>;

    KDialog(const QString& title, const QString& objectName, QWidget& owner,
            Qt::WindowModality modality = Qt::WindowModal, Width width = Width::Text);

    // The top-level window a dialog over `owner` belongs to.
    [[nodiscard]] static QWidget& ownerWindow(QWidget& owner);

    [[nodiscard]] QVBoxLayout* contentLayout() const noexcept { return content_; }
    // A form at the end of the content (or of `layout`, such as a KSection body).
    KForm* addForm();
    KForm* addForm(QVBoxLayout& layout, QWidget& parent);
    // A collapsible section holding one form.
    std::pair<KSection*, KForm*> addSection(const QString& title, const QString& objectName);
    // Sizes every label of every form to the widest; runs again on show.
    void fitLabelColumn();
    void registerLabel(KLabel* label);

    // A kit button in the platform-ordered button row: Accept is Primary, Destructive is Danger,
    // the rest are Secondary. Accept/Destructive/Alternate close the dialog as accepted (after the
    // validator agrees), Reject closes it as rejected, Apply and Reset only emit their signal.
    KButton* addButton(Role role, const QString& text);
    [[nodiscard]] KButton* button(Role role) const;
    [[nodiscard]] QDialogButtonBox* buttonBox() const noexcept { return buttons_; }
    // The button Return presses. Accept by default; a destructive prompt names the safe action.
    void setDefaultRole(Role role);
    [[nodiscard]] Role defaultRole() const noexcept { return defaultRole_; }
    // The role that closed the dialog: Reject for Escape and the window's close button, Accept
    // for a plain QDialog::accept().
    [[nodiscard]] Role chosenRole() const noexcept;

    // The widget focused when the dialog opens; a line edit's text is selected.
    void setInitialFocus(QWidget* widget);

    // One validator for the whole dialog: the first refusal it returns is shown (under its field,
    // or above the buttons) and every closing button but Reject is disabled until it clears.
    void setValidator(Validator validator);
    void revalidate();
    // Revalidates whenever `field` (a line edit, value field, dropdown, check box, colour chip)
    // changes.
    void watch(QWidget* field);
    [[nodiscard]] const std::optional<KRefusal>& refusal() const noexcept { return refusal_; }
    [[nodiscard]] KLabel* dialogReason() const noexcept { return dialogReason_; }
    void registerReason(QWidget* field, KLabel* reason);

    // What pressing the role's button does. False (and the dialog stays) while refused.
    bool activate(Role role);
    // Commits a value field whose text editor is open, as focus leaving it would.
    void commitOpenEditors();

    // Shows the dialog asynchronously with its modality and calls `done` once with the role that
    // closed it. The dialog deletes itself afterwards.
    void open(std::function<void(Role)> done);
    using QDialog::open;

  Q_SIGNALS:
    void applyRequested();
    void resetRequested();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void watchKeys(QWidget* widget);
    void applyTabOrder();

    QVBoxLayout* root_ = nullptr;
    QVBoxLayout* content_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    KLabel* dialogReason_ = nullptr;
    std::vector<std::pair<Role, KButton*>> roleButtons_;
    std::vector<QPointer<KForm>> forms_;
    std::vector<QPointer<KLabel>> labels_;
    std::vector<std::pair<QPointer<QWidget>, QPointer<KLabel>>> reasons_;
    QPointer<QWidget> initialFocus_;
    Validator validator_;
    std::optional<KRefusal> refusal_;
    Role defaultRole_ = Role::Accept;
    Role chosen_ = Role::Reject;
    Qt::WindowModality modality_;
};

} // namespace bloom::ui::kit
