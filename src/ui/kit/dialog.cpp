#include <bloom/ui/kit/dialog.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] QDialogButtonBox::ButtonRole boxRole(const KDialog::Role role) {
    switch (role) {
    case KDialog::Role::Accept:
        return QDialogButtonBox::AcceptRole;
    case KDialog::Role::Reject:
        return QDialogButtonBox::RejectRole;
    case KDialog::Role::Destructive:
        return QDialogButtonBox::DestructiveRole;
    case KDialog::Role::Alternate:
        return QDialogButtonBox::ActionRole;
    case KDialog::Role::Apply:
        return QDialogButtonBox::ApplyRole;
    case KDialog::Role::Reset:
        return QDialogButtonBox::ResetRole;
    }
    return QDialogButtonBox::ActionRole;
}

[[nodiscard]] QString roleName(const KDialog::Role role) {
    switch (role) {
    case KDialog::Role::Accept:
        return QStringLiteral("Accept");
    case KDialog::Role::Reject:
        return QStringLiteral("Reject");
    case KDialog::Role::Destructive:
        return QStringLiteral("Destructive");
    case KDialog::Role::Alternate:
        return QStringLiteral("Alternate");
    case KDialog::Role::Apply:
        return QStringLiteral("Apply");
    case KDialog::Role::Reset:
        return QStringLiteral("Reset");
    }
    return {};
}

// Accept, Destructive and Alternate commit and close; the validator gates exactly these.
[[nodiscard]] bool commits(const KDialog::Role role) {
    return role == KDialog::Role::Accept || role == KDialog::Role::Destructive ||
           role == KDialog::Role::Alternate;
}

[[nodiscard]] bool isEnter(const QKeyEvent& key) {
    return key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter;
}

// Focusable widgets under `layout`, in layout order. A focusable widget is one stop; a plain
// holder widget is walked through its own layout.
void collectFocusable(QLayout* layout, std::vector<QWidget*>& out) {
    if (layout == nullptr)
        return;
    for (int index = 0; index < layout->count(); ++index) {
        auto* item = layout->itemAt(index);
        if (auto* widget = item->widget()) {
            if ((widget->focusPolicy() & Qt::TabFocus) != 0)
                out.push_back(widget);
            else
                collectFocusable(widget->layout(), out);
        } else if (auto* nested = item->layout()) {
            collectFocusable(nested, out);
        }
    }
}

} // namespace

void styleDocumentView(QAbstractScrollArea& view) {
    view.setFrameShape(QFrame::NoFrame);
    auto palette = view.palette();
    palette.setColor(QPalette::Base, color(Color::Field));
    palette.setColor(QPalette::Text, color(Color::Foreground));
    view.setPalette(palette);
    view.setAutoFillBackground(true);
}

KCheckLabel::KCheckLabel(const QString& text, QAbstractButton& buddy, QWidget* parent)
    : KLabel(text, parent), buddy_(&buddy) {
    setBuddy(&buddy);
    buddy.setAccessibleName(text);
    setEnabled(buddy.isEnabled());
    buddy.installEventFilter(this);
}

void KCheckLabel::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && buddy_ && buddy_->isEnabled() &&
        rect().contains(event->position().toPoint())) {
        buddy_->setFocus(Qt::MouseFocusReason);
        buddy_->click();
        event->accept();
        return;
    }
    KLabel::mouseReleaseEvent(event);
}

bool KCheckLabel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == buddy_) {
        if (event->type() == QEvent::EnabledChange)
            setEnabled(buddy_->isEnabled());
        else if (event->type() == QEvent::ShowToParent)
            show();
        else if (event->type() == QEvent::HideToParent)
            hide();
    }
    return KLabel::eventFilter(watched, event);
}

KForm::KForm(KDialog& dialog, QWidget* parent) : QWidget(parent), dialog_(dialog) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(px(Spacing::M));
    grid_->setVerticalSpacing(px(Spacing::S));
    grid_->setColumnStretch(1, 1);
}

KLabel* KForm::place(KLabel* label, QWidget* widget, QWidget* key, const bool reason) {
    const int row = static_cast<int>(rows_.size());
    auto* holder = new QWidget(this);
    auto* column = new QVBoxLayout(holder);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(px(Spacing::XXS));
    column->addWidget(widget);
    KLabel* reasonLabel = nullptr;
    if (reason) {
        reasonLabel = new KLabel(QString{}, holder, TypeRole::UiSmall);
        reasonLabel->setWordWrap(true);
        reasonLabel->setTone(Color::Error);
        reasonLabel->hide();
        column->addWidget(reasonLabel);
        dialog_.registerReason(key, reasonLabel);
    }
    if (label != nullptr) {
        label->setParent(this);
        // Top-aligned with the control row even when an inline reason grows the field below it.
        label->setMinimumHeight(px(Size::Control));
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        grid_->addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignTop);
        dialog_.registerLabel(label);
    }
    grid_->addWidget(holder, row, 1);
    rows_.push_back({label, holder, key, reasonLabel});
    if (label != nullptr)
        widget->installEventFilter(this);
    return reasonLabel;
}

KLabel* KForm::addRow(const QString& label, QWidget* field) {
    return place(new KLabel(label, this), field, field, true);
}

KLabel* KForm::addRow(QWidget* field) { return place(nullptr, field, field, true); }

void KForm::addWideRow(QWidget* widget) {
    const int row = static_cast<int>(rows_.size());
    widget->setParent(this);
    grid_->addWidget(widget, row, 0, 1, 2);
    rows_.push_back({nullptr, widget, widget, nullptr});
}

KLabel* KForm::addCheckRow(const QString& label, KCheckBox& box, const QString& text) {
    auto* holder = new QWidget(this);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(px(Spacing::S));
    row->addWidget(&box);
    auto* words = new KCheckLabel(text, box, holder);
    words->setObjectName(box.objectName().isEmpty() ? QString{}
                                                    : box.objectName() + QStringLiteral("Label"));
    row->addWidget(words);
    row->addStretch(1);
    return place(label.isEmpty() ? nullptr : new KLabel(label, this), holder, &box, true);
}

const KForm::Row* KForm::rowFor(QWidget* field) const {
    for (const auto& row : rows_)
        if (row.field == field || row.holder == field)
            return &row;
    return nullptr;
}

void KForm::setRowVisible(QWidget* field, const bool visible) {
    if (const auto* row = rowFor(field)) {
        if (row->label != nullptr)
            row->label->setVisible(visible);
        row->holder->setVisible(visible);
    }
}

bool KForm::isRowVisible(QWidget* field) const {
    const auto* row = rowFor(field);
    return row != nullptr && !row->holder->isHidden();
}

KLabel* KForm::labelFor(QWidget* field) const {
    const auto* row = rowFor(field);
    return row != nullptr ? row->label : nullptr;
}

KLabel* KForm::reasonFor(QWidget* field) const {
    const auto* row = rowFor(field);
    return row != nullptr ? row->reason : nullptr;
}

std::vector<KLabel*> KForm::labels() const {
    std::vector<KLabel*> found;
    for (const auto& row : rows_)
        if (row.label != nullptr)
            found.push_back(row.label);
    return found;
}

bool KForm::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)
        for (const auto& row : rows_)
            if (row.label != nullptr && row.holder->layout() != nullptr &&
                row.holder->layout()->indexOf(qobject_cast<QWidget*>(watched)) >= 0)
                row.label->setVisible(event->type() == QEvent::ShowToParent);
    return QWidget::eventFilter(watched, event);
}

std::vector<QWidget*> KForm::tabSequence() const {
    std::vector<QWidget*> sequence;
    for (const auto& row : rows_) {
        if ((row.holder->focusPolicy() & Qt::TabFocus) != 0)
            sequence.push_back(row.holder);
        else
            collectFocusable(row.holder->layout(), sequence);
    }
    return sequence;
}

KDialog::KDialog(const QString& title, const QString& objectName, QWidget& owner,
                 const Qt::WindowModality modality, const Width width)
    : QDialog(&ownerWindow(owner)), modality_(modality) {
    setObjectName(objectName);
    setWindowTitle(title);
    setWindowModality(modality);
    setMinimumWidth(px(width == Width::Wide ? Size::DialogWideWidth : Size::DialogTextWidth));
    // Dialogs sit on the raised surface (visual-language.md, Surfaces); the native frame stays.
    auto surface = palette();
    surface.setColor(QPalette::Window, color(Color::SurfaceRaised));
    setPalette(surface);
    setAutoFillBackground(true);

    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(px(Spacing::M), px(Spacing::M), px(Spacing::M), px(Spacing::M));
    root_->setSpacing(px(Spacing::S));
    content_ = new QVBoxLayout;
    content_->setContentsMargins(0, 0, 0, 0);
    content_->setSpacing(px(Spacing::S));
    root_->addLayout(content_, 1);
    dialogReason_ = new KLabel(QString{}, this, TypeRole::UiSmall);
    dialogReason_->setObjectName(objectName.isEmpty() ? QStringLiteral("kDialogReason")
                                                      : objectName + QStringLiteral("Reason"));
    dialogReason_->setWordWrap(true);
    dialogReason_->setTone(Color::Error);
    dialogReason_->hide();
    root_->addWidget(dialogReason_);
    buttons_ = new QDialogButtonBox(Qt::Horizontal, this);
    buttons_->setObjectName(objectName.isEmpty() ? QStringLiteral("kDialogButtons")
                                                 : objectName + QStringLiteral("Buttons"));
    buttons_->hide();
    root_->addWidget(buttons_);
}

QWidget& KDialog::ownerWindow(QWidget& owner) { return *owner.window(); }

KForm* KDialog::addForm() {
    auto* form = new KForm(*this, this);
    content_->addWidget(form);
    forms_.emplace_back(form);
    return form;
}

KForm* KDialog::addForm(QVBoxLayout& layout, QWidget& parent) {
    auto* form = new KForm(*this, &parent);
    layout.addWidget(form);
    forms_.emplace_back(form);
    return form;
}

std::pair<KSection*, KForm*> KDialog::addSection(const QString& title, const QString& objectName) {
    auto* section = new KSection(title, this);
    section->setObjectName(objectName);
    section->setResetEnabled(false);
    content_->addWidget(section);
    auto* form = addForm(*section->bodyLayout(), *section->body());
    return {section, form};
}

void KDialog::registerLabel(KLabel* label) {
    labels_.emplace_back(label);
    if (isVisible())
        fitLabelColumn();
}

void KDialog::fitLabelColumn() {
    int width = 0;
    for (const auto& label : labels_)
        if (label)
            width = std::max(width, label->sizeHint().width());
    for (const auto& label : labels_)
        if (label)
            label->setFixedWidth(width);
}

KButton* KDialog::addButton(const Role role, const QString& text) {
    auto* button = new KButton(text, this);
    button->setVariant(role == Role::Accept        ? KButton::Variant::Primary
                       : role == Role::Destructive ? KButton::Variant::Danger
                                                   : KButton::Variant::Secondary);
    if (!objectName().isEmpty())
        button->setObjectName(objectName() + roleName(role) + QStringLiteral("Button"));
    buttons_->addButton(button, boxRole(role));
    buttons_->show();
    roleButtons_.emplace_back(role, button);
    connect(button, &QAbstractButton::clicked, this, [this, role] { (void)activate(role); });
    if (commits(role))
        button->setEnabled(!refusal_.has_value());
    return button;
}

KButton* KDialog::button(const Role role) const {
    for (const auto& [candidate, button] : roleButtons_)
        if (candidate == role)
            return button;
    return nullptr;
}

void KDialog::setDefaultRole(const Role role) { defaultRole_ = role; }

KDialog::Role KDialog::chosenRole() const noexcept {
    return result() == QDialog::Accepted && chosen_ == Role::Reject ? Role::Accept : chosen_;
}

void KDialog::setInitialFocus(QWidget* widget) { initialFocus_ = widget; }

void KDialog::setValidator(Validator validator) {
    validator_ = std::move(validator);
    revalidate();
}

void KDialog::registerReason(QWidget* field, KLabel* reason) {
    reasons_.emplace_back(field, reason);
}

void KDialog::watch(QWidget* field) {
    if (field == nullptr)
        return;
    const auto again = [this] { revalidate(); };
    if (auto* line = qobject_cast<QLineEdit*>(field))
        connect(line, &QLineEdit::textChanged, this, again);
    else if (auto* value = qobject_cast<KValueField*>(field))
        connect(value, &KValueField::valueChanged, this, again);
    else if (auto* dropdown = qobject_cast<KDropdown*>(field))
        connect(dropdown, &KDropdown::currentIndexChanged, this, again);
    else if (auto* chip = qobject_cast<KColorChip*>(field))
        connect(chip, &KColorChip::colorChanged, this, again);
    else if (auto* toggle = qobject_cast<QAbstractButton*>(field))
        connect(toggle, &QAbstractButton::toggled, this, again);
}

void KDialog::revalidate() {
    refusal_ = validator_ ? validator_() : std::nullopt;
    for (auto& [field, reason] : reasons_) {
        if (reason) {
            reason->clear();
            reason->hide();
        }
    }
    dialogReason_->clear();
    dialogReason_->hide();
    if (refusal_) {
        // Only the first refusal shows, under its own field, so it names what to fix next.
        KLabel* target = nullptr;
        for (auto& [field, reason] : reasons_)
            if (field && field == refusal_->field && reason)
                target = reason;
        if (target == nullptr)
            target = dialogReason_;
        target->setText(refusal_->reason);
        target->setTone(refusal_->pending ? Color::Muted : Color::Error);
        target->show();
        // A reason inside a closed section opens it, so it is never out of sight.
        for (auto* widget = refusal_->field; widget != nullptr; widget = widget->parentWidget())
            if (auto* section = qobject_cast<KSection*>(widget); section && section->isCollapsed())
                section->setCollapsed(false);
    }
    // A reason on a tab that is not showing marks that tab with an Error dot.
    for (auto* tabs : findChildren<KTabs*>()) {
        connect(tabs, &KTabs::currentChanged, this, &KDialog::revalidate, Qt::UniqueConnection);
        const int page = refusal_ && !refusal_->pending ? tabs->pageOf(refusal_->field) : -1;
        for (int index = 0; index < tabs->bar()->count(); ++index)
            tabs->bar()->setTabAlert(index, index == page && index != tabs->currentIndex());
    }
    for (const auto& [role, button] : roleButtons_)
        if (commits(role))
            button->setEnabled(!refusal_.has_value());
}

void KDialog::commitOpenEditors() {
    // A value field commits on focus loss; sending that loss commits exactly what the artist typed
    // whether or not this window is active.
    for (auto* field : findChildren<KValueField*>()) {
        if (field->isEditing() && field->lineEdit() != nullptr) {
            QFocusEvent out(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(field->lineEdit(), &out);
        }
    }
}

bool KDialog::activate(const Role role) {
    auto* chosen = button(role);
    if (chosen == nullptr || chosen->isHidden())
        return false;
    if (!chosen->isEnabled()) {
        // A commit refused by the validator still shows where: Enter on a refused form goes to
        // the refused field, on its tab.
        if (commits(role) && refusal_)
            revealRefusal();
        return false;
    }
    switch (role) {
    case Role::Apply:
        emit applyRequested();
        return true;
    case Role::Reset:
        emit resetRequested();
        return true;
    case Role::Reject:
        chosen_ = Role::Reject;
        reject();
        return true;
    case Role::Accept:
    case Role::Destructive:
    case Role::Alternate:
        break;
    }
    commitOpenEditors();
    revalidate();
    if (refusal_) {
        revealRefusal();
        return false;
    }
    chosen_ = role;
    accept();
    return true;
}

void KDialog::revealRefusal() {
    // Show the first refused field, switching to its tab, and put the cursor in it.
    auto* field = refusal_ ? refusal_->field : nullptr;
    if (field == nullptr)
        return;
    for (auto* tabs : findChildren<KTabs*>())
        if (const int page = tabs->pageOf(field); page >= 0)
            tabs->setCurrentIndex(page);
    QWidget* target = field;
    if ((field->focusPolicy() & Qt::TabFocus) == 0) {
        std::vector<QWidget*> inside;
        collectFocusable(field->layout(), inside);
        if (!inside.empty())
            target = inside.front();
    }
    target->setFocus(Qt::OtherFocusReason);
}

void KDialog::open(std::function<void(Role)> done) {
    setAttribute(Qt::WA_DeleteOnClose);
    connect(
        this, &QDialog::finished, this,
        [this, done = std::move(done)](int) {
            if (done)
                done(chosenRole());
        },
        Qt::SingleShotConnection);
    setWindowModality(modality_);
    show();
    raise();
    activateWindow();
}

void KDialog::fitToContent() {
    auto* own = layout();
    if (own == nullptr)
        return;
    own->activate();
    // Never narrower than the content's minimum either: an explicit minimum width (the dialog's
    // token width) must not squeeze a wider layout.
    const int wide = std::max(width(), own->totalMinimumSize().width());
    const int needed = std::max(own->hasHeightForWidth() ? own->totalHeightForWidth(wide)
                                                         : own->sizeHint().height(),
                                own->totalMinimumSize().height());
    if (height() < needed || width() < wide)
        resize(wide, std::max(height(), needed));
}

bool KDialog::event(QEvent* event) {
    const bool handled = QDialog::event(event);
    if (event->type() == QEvent::LayoutRequest && isVisible())
        fitToContent();
    return handled;
}

void KDialog::keyPressEvent(QKeyEvent* event) {
    if (isEnter(*event)) {
        if ((event->modifiers() & Qt::ControlModifier) != 0) {
            commitOpenEditors();
            (void)activate(button(Role::Accept) != nullptr ? Role::Accept : defaultRole_);
            event->accept();
            return;
        }
        // A focused button-row button is what Return means; otherwise the default button.
        if (auto* focused = qobject_cast<QAbstractButton*>(focusWidget());
            focused != nullptr && buttons_->isAncestorOf(focused)) {
            if (focused->isEnabled())
                focused->click();
        } else {
            (void)activate(defaultRole_);
        }
        event->accept();
        return;
    }
    // Escape (QKeySequence::Cancel) rejects through QDialog.
    QDialog::keyPressEvent(event);
}

void KDialog::watchKeys(QWidget* widget) {
    widget->installEventFilter(this);
    for (auto* child : widget->findChildren<QWidget*>())
        child->installEventFilter(this);
}

bool KDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ChildAdded) {
        // Rows added after the dialog opened (another tag field) take Ctrl+Return too.
        if (auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(event)->child()))
            watchKeys(child);
    } else if (event->type() == QEvent::KeyPress && watched != this) {
        auto* key = static_cast<QKeyEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        // Ctrl+Return accepts from inside any field, before the field's own Return handling
        // (a multi-line field's newline, a value field's commit) sees it. A widget a popup has
        // reparented out of this window is not this dialog's to answer for.
        if (widget != nullptr && widget->window() == this && isEnter(*key) &&
            (key->modifiers() & Qt::ControlModifier) != 0) {
            commitOpenEditors();
            (void)activate(button(Role::Accept) != nullptr ? Role::Accept : defaultRole_);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void KDialog::applyTabOrder() {
    std::vector<QWidget*> sequence;
    for (const auto& form : forms_) {
        if (!form)
            continue;
        // A form inside a section follows that section's header.
        for (auto* widget = form->parentWidget(); widget != nullptr && widget != this;
             widget = widget->parentWidget())
            if (auto* section = qobject_cast<KSection*>(widget)) {
                if (auto* header = section->header();
                    header != nullptr && (header->focusPolicy() & Qt::TabFocus) != 0)
                    sequence.push_back(header);
                break;
            }
        const auto part = form->tabSequence();
        sequence.insert(sequence.end(), part.begin(), part.end());
    }
    if (sequence.empty())
        return;
    collectFocusable(buttons_->layout(), sequence);
    for (std::size_t index = 1; index < sequence.size(); ++index)
        QWidget::setTabOrder(sequence[index - 1], sequence[index]);
}

void KDialog::showEvent(QShowEvent* event) {
    chosen_ = Role::Reject;
    fitLabelColumn();
    applyTabOrder();
    watchKeys(this);
    revalidate();
    QDialog::showEvent(event);
    fitToContent();
    if (initialFocus_) {
        initialFocus_->setFocus(Qt::OtherFocusReason);
        if (auto* line = qobject_cast<QLineEdit*>(initialFocus_.data()))
            line->selectAll();
    } else if (auto* fallback = button(defaultRole_); fallback != nullptr && forms_.empty()) {
        // A prompt without fields starts on its default button, so the safe answer is the one a
        // stray Return gives.
        fallback->setFocus(Qt::OtherFocusReason);
    }
}

} // namespace bloom::ui::kit
