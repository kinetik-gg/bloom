// Lane C: the kit dialog primitive. KDialog's keys (Return, Ctrl+Return, Escape and the Escape that
// belongs to an open field editor), the validator's inline reasons, the platform-ordered kit
// button row, initial focus, the shared form label column and its visual Tab order, the
// confirm/promptText/promptRange helpers, kit::chooseFile() and the task dialog.
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/task_dialog.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>

namespace {

using namespace bloom::ui;
using Role = kit::KDialog::Role;

int failures = 0;
void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

// The next widget Tab reaches from `widget`: the next focus-chain entry that takes Tab focus.
[[nodiscard]] QWidget* nextTabStop(QWidget* widget) {
    for (auto* next = widget->nextInFocusChain(); next != nullptr && next != widget;
         next = next->nextInFocusChain())
        if ((next->focusPolicy() & Qt::TabFocus) != 0 && next->isVisible() && next->isEnabled())
            return next;
    return nullptr;
}

void open(QWidget& dialog) {
    dialog.show();
    dialog.activateWindow();
    (void)QTest::qWaitForWindowExposed(&dialog);
    QApplication::processEvents();
}

// A dialog with one name field and an OK/Cancel row; the name may not be empty.
struct NameDialog final {
    std::unique_ptr<kit::KDialog> dialog;
    kit::KLineEdit* name = nullptr;
    kit::KValueField* count = nullptr;
    kit::KLabel* nameReason = nullptr;

    explicit NameDialog(QWidget& owner) {
        dialog =
            std::make_unique<kit::KDialog>(QStringLiteral("Name"), QStringLiteral("named"), owner);
        auto* form = dialog->addForm();
        name = new kit::KLineEdit(QStringLiteral("Shot"), dialog.get());
        nameReason = form->addRow(QStringLiteral("Name"), name);
        count = new kit::KValueField(dialog.get());
        count->setDecimals(0);
        count->setRange(0, 1000);
        count->setValue(1);
        (void)form->addRow(QStringLiteral("Count"), count);
        dialog->addButton(Role::Accept, QStringLiteral("OK"));
        dialog->addButton(Role::Reject, QStringLiteral("Cancel"));
        dialog->setValidator([this]() -> std::optional<kit::KRefusal> {
            if (name->text().isEmpty())
                return kit::KRefusal{name, QStringLiteral("Enter a name.")};
            return std::nullopt;
        });
        dialog->watch(name);
        dialog->setInitialFocus(name);
    }
};

void testOwnerWidthAndFocus(QWidget& owner) {
    auto* child = new QWidget(&owner);
    NameDialog named(*child);
    expect(named.dialog->parentWidget() == &owner,
           "a dialog over a child widget belongs to the child's window");
    expect(named.dialog->windowModality() == Qt::WindowModal, "modality is explicit");
    expect(named.dialog->minimumWidth() == kit::px(kit::Size::DialogTextWidth),
           "the width comes from the dialog width tokens");
    const auto* layout = named.dialog->layout();
    expect(layout != nullptr && layout->contentsMargins().left() == kit::px(kit::Spacing::M) &&
               layout->spacing() == kit::px(kit::Spacing::S),
           "margins are Spacing::M and rows are Spacing::S apart");
    open(*named.dialog);
    expect(named.dialog->focusWidget() == named.name && named.name->selectedText() == "Shot",
           "the initial focus is the name, its text selected");
    named.dialog->reject();
    delete child;
}

void testButtonRow(QWidget& owner) {
    kit::KDialog dialog(QStringLiteral("Buttons"), QStringLiteral("buttons"), owner);
    auto* accept = dialog.addButton(Role::Accept, QStringLiteral("Save"));
    auto* destructive = dialog.addButton(Role::Destructive, QStringLiteral("Don't Save"));
    auto* reject = dialog.addButton(Role::Reject, QStringLiteral("Cancel"));
    auto* box = dialog.buttonBox();
    expect(box->buttonRole(accept) == QDialogButtonBox::AcceptRole &&
               box->buttonRole(destructive) == QDialogButtonBox::DestructiveRole &&
               box->buttonRole(reject) == QDialogButtonBox::RejectRole,
           "each kit button sits in the button box under its platform role");
    expect(box->style()->styleHint(QStyle::SH_DialogButtonLayout, nullptr, box) ==
               QApplication::style()->styleHint(QStyle::SH_DialogButtonLayout),
           "the button box follows the platform's button layout policy");
    expect(accept->variant() == kit::KButton::Variant::Primary &&
               destructive->variant() == kit::KButton::Variant::Danger &&
               reject->variant() == kit::KButton::Variant::Secondary,
           "Accept is Primary, Destructive is Danger, the rest Secondary");
    expect(accept->objectName() == QStringLiteral("buttonsAcceptButton"),
           "buttons are named after the dialog and their role");
}

void testEnterAndValidation(QWidget& owner) {
    NameDialog named(owner);
    open(*named.dialog);
    named.name->setText(QString{});
    expect(!named.dialog->button(Role::Accept)->isEnabled() && !named.nameReason->isHidden() &&
               named.nameReason->text() == QStringLiteral("Enter a name."),
           "a refused value disables OK and shows its reason under its own field");
    QTest::keyClick(named.name, Qt::Key_Return);
    expect(named.dialog->isVisible(), "Return is refused while OK is disabled");
    named.name->setText(QStringLiteral("Hero"));
    expect(named.nameReason->isHidden() && named.dialog->button(Role::Accept)->isEnabled(),
           "the reason hides once the value is valid");
    QTest::keyClick(named.name, Qt::Key_Return);
    expect(!named.dialog->isVisible() && named.dialog->result() == QDialog::Accepted &&
               named.dialog->chosenRole() == Role::Accept,
           "Return accepts");
}

void testEscapeOrder(QWidget& owner) {
    NameDialog named(owner);
    open(*named.dialog);
    named.count->setFocus();
    QTest::keyClick(named.count, Qt::Key_Return);
    expect(named.count->isEditing(), "Return on a value field opens its editor");
    QTest::keyClick(named.count->lineEdit(), Qt::Key_Escape);
    expect(named.dialog->isVisible() && !named.count->isEditing(),
           "the first Escape belongs to the open field editor; the dialog stays");
    QTest::keyClick(named.count, Qt::Key_Escape);
    expect(!named.dialog->isVisible() && named.dialog->chosenRole() == Role::Reject,
           "the next Escape rejects the dialog");
}

void testControlReturn(QWidget& owner) {
    {
        NameDialog named(owner);
        open(*named.dialog);
        named.count->setFocus();
        QTest::keyClick(named.count, Qt::Key_Return);
        named.count->lineEdit()->setText(QStringLiteral("42"));
        QTest::keyClick(named.count->lineEdit(), Qt::Key_Return, Qt::ControlModifier);
        expect(!named.dialog->isVisible() && named.dialog->result() == QDialog::Accepted,
               "Ctrl+Return accepts from an open value-field editor");
        expect(named.count->value() == 42.0, "the open editor's typed value is committed first");
    }
    {
        kit::KDialog dialog(QStringLiteral("Notes"), QStringLiteral("notes"), owner);
        auto* notes = new QPlainTextEdit(&dialog);
        dialog.contentLayout()->addWidget(notes);
        dialog.addButton(Role::Accept, QStringLiteral("OK"));
        dialog.addButton(Role::Reject, QStringLiteral("Cancel"));
        open(dialog);
        notes->setFocus();
        QTest::keyClick(notes, Qt::Key_Return);
        expect(dialog.isVisible(), "Return in a multi-line field is a newline");
        QTest::keyClick(notes, Qt::Key_Return, Qt::ControlModifier);
        expect(!dialog.isVisible() && dialog.result() == QDialog::Accepted,
               "Ctrl+Return accepts from a multi-line field");
    }
    {
        NameDialog named(owner);
        open(*named.dialog);
        named.name->setText(QString{});
        QTest::keyClick(named.name, Qt::Key_Return, Qt::ControlModifier);
        expect(named.dialog->isVisible(), "Ctrl+Return is refused while the dialog is invalid");
        named.dialog->reject();
    }
}

void testFormLabelsAndTabOrder(QWidget& owner) {
    kit::KDialog dialog(QStringLiteral("Form"), QStringLiteral("form"), owner, Qt::WindowModal,
                        kit::KDialog::Width::Wide);
    auto* first = dialog.addForm();
    auto [section, second] = dialog.addSection(QStringLiteral("More"), QStringLiteral("more"));
    // Constructed in the WRONG order: the unit before the start field.
    auto* unit = new kit::KLineEdit(&dialog);
    auto* start = new kit::KLineEdit(&dialog);
    auto* duration = new kit::KLineEdit(&dialog);
    auto* holder = new QWidget(&dialog);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(duration);
    row->addWidget(unit);
    (void)first->addRow(QStringLiteral("Start"), start);
    (void)first->addRow(QStringLiteral("Duration"), holder);
    auto* wide = new kit::KLineEdit(&dialog);
    (void)second->addRow(QStringLiteral("A much longer label"), wide);
    dialog.addButton(Role::Accept, QStringLiteral("OK"));
    open(dialog);
    expect(nextTabStop(start) == duration && nextTabStop(duration) == unit,
           "Tab follows the rows in visual order, not construction order");
    expect(nextTabStop(unit) == section->header() && nextTabStop(section->header()) == wide,
           "a section's header comes before its rows");
    expect(first->labelFor(start)->width() == second->labelFor(wide)->width() &&
               first->labelFor(start)->width() >= second->labelFor(wide)->sizeHint().width(),
           "every form of the dialog shares one label column, sized to the widest label");
    expect(first->labelFor(start) != nullptr &&
               first->labelFor(start)->alignment().testFlag(Qt::AlignLeft),
           "labels are left-aligned KLabels");
    dialog.reject();
}

void testSectionOpensForItsReason(QWidget& owner) {
    kit::KDialog dialog(QStringLiteral("Hidden"), QStringLiteral("hidden"), owner);
    auto [section, form] = dialog.addSection(QStringLiteral("Advanced"), QStringLiteral("adv"));
    auto* field = new kit::KLineEdit(&dialog);
    (void)form->addRow(QStringLiteral("Value"), field);
    section->setCollapsed(true);
    dialog.addButton(Role::Accept, QStringLiteral("OK"));
    dialog.setValidator([field]() -> std::optional<kit::KRefusal> {
        return kit::KRefusal{field, QStringLiteral("No.")};
    });
    expect(!section->isCollapsed(), "a reason inside a closed section opens it");
    dialog.setValidator([]() -> std::optional<kit::KRefusal> {
        return kit::KRefusal{nullptr, QStringLiteral("The project changed.")};
    });
    expect(!dialog.dialogReason()->isHidden() &&
               dialog.dialogReason()->text() == QStringLiteral("The project changed."),
           "a refusal without a field shows above the buttons");
}

void testConfirm(QWidget& owner) {
    kit::KConfirmSpec spec;
    spec.title = QStringLiteral("Delete");
    spec.objectName = QStringLiteral("deletePrompt");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = QStringLiteral("Delete “Shot”?");
    spec.destructiveText = QStringLiteral("Delete");
    spec.rejectText = QStringLiteral("Cancel");
    spec.defaultRole = Role::Reject;
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<kit::KDialog*>(QApplication::activeModalWidget()))
            QTest::keyClick(dialog, Qt::Key_Return);
    });
    expect(kit::confirm(owner, spec) == Role::Reject,
           "a destructive prompt's Return is the safe default");
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<kit::KDialog*>(QApplication::activeModalWidget()))
            dialog->button(Role::Destructive)->click();
    });
    expect(kit::confirm(owner, spec) == Role::Destructive, "the destructive button answers");
    bool answered = false;
    kit::confirm(owner, spec, [&answered](const Role role) { answered = role == Role::Reject; });
    auto* asynchronous = owner.findChild<kit::KDialog*>(QStringLiteral("deletePrompt"));
    expect(asynchronous != nullptr && asynchronous->isVisible() && !answered,
           "the asynchronous confirm returns at once with the prompt open");
    if (asynchronous != nullptr)
        QTest::keyClick(asynchronous, Qt::Key_Escape);
    QApplication::processEvents();
    expect(answered, "Escape answers the asynchronous confirm with Reject");
}

void testPromptText(QWidget& owner) {
    kit::KTextPromptSpec spec;
    spec.title = QStringLiteral("Rename");
    spec.objectName = QStringLiteral("renamePrompt");
    spec.label = QStringLiteral("Name");
    spec.text = QStringLiteral("Old");
    spec.validate = [](const QString& text) -> std::optional<QString> {
        if (text == QStringLiteral("Taken"))
            return QStringLiteral("That name is already used.");
        return std::nullopt;
    };
    bool refusedInside = false;
    QTimer::singleShot(0, [&refusedInside] {
        auto* dialog = qobject_cast<kit::KDialog*>(QApplication::activeModalWidget());
        auto* field =
            dialog ? dialog->findChild<QLineEdit*>(QStringLiteral("renamePromptField")) : nullptr;
        if (!field)
            return;
        field->setText(QStringLiteral("Taken"));
        QTest::keyClick(field, Qt::Key_Return);
        refusedInside = dialog->isVisible() && dialog->refusal().has_value() &&
                        dialog->refusal()->reason == QStringLiteral("That name is already used.");
        field->setText(QStringLiteral("New"));
        QTest::keyClick(field, Qt::Key_Return);
    });
    const auto chosen = kit::promptText(owner, spec);
    expect(refusedInside, "a refused name keeps the prompt open with its reason inside");
    expect(chosen == std::optional(QStringLiteral("New")), "an accepted name is returned");
}

void testPromptRange(QWidget& owner) {
    kit::KRangePromptSpec spec;
    spec.title = QStringLiteral("Range");
    spec.objectName = QStringLiteral("rangePrompt");
    spec.firstLabel = QStringLiteral("First");
    spec.lastLabel = QStringLiteral("Last");
    spec.minimum = 0;
    spec.maximum = 100;
    spec.first = 0;
    spec.last = 100;
    std::unique_ptr<kit::KDialog> prompt(kit::buildRangePrompt(owner, spec));
    auto* first = prompt->findChild<kit::KValueField*>(QStringLiteral("kRangeFirst"));
    auto* last = prompt->findChild<kit::KValueField*>(QStringLiteral("kRangeLast"));
    first->setValue(60);
    last->setValue(10);
    expect(prompt->refusal().has_value() && prompt->refusal()->field == last &&
               !prompt->button(Role::Accept)->isEnabled(),
           "a last value before the first is refused under Last");
    last->setValue(80);
    expect(!prompt->refusal().has_value() && kit::rangeOf(*prompt).first == 60 &&
               kit::rangeOf(*prompt).last == 80,
           "a valid range is read back");
}

void testChooseFile(QWidget& owner) {
    QTemporaryDir directory;
    const auto typed = QDir(directory.path()).filePath(QStringLiteral("project"));
    QWidget* parent = nullptr;
    QTimer::singleShot(0, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QFileDialog*>(widget); dialog && dialog->isVisible()) {
                parent = dialog->parentWidget();
                dialog->selectFile(typed);
                QMetaObject::invokeMethod(dialog, "accept");
                return;
            }
    });
    const auto chosen = kit::chooseFile(owner, {.mode = kit::KFileRequest::Mode::Save,
                                                .title = QStringLiteral("Save"),
                                                .kind = QStringLiteral("kit-test"),
                                                .nameFilters = QStringLiteral("Bloom (*.bloom)"),
                                                .defaultSuffix = QStringLiteral("bloom"),
                                                .startPath = {}});
    expect(parent == &owner, "the file dialog is parented to the owner window");
    expect(chosen == std::optional(typed + QStringLiteral(".bloom")),
           "Save adds the default suffix to a name typed without one");
    expect(QDir(kit::rememberedDirectory(QStringLiteral("kit-test"))) == QDir(directory.path()),
           "the directory is remembered per kind");
    const auto built =
        kit::buildFileDialog(owner, {.mode = kit::KFileRequest::Mode::Open,
                                     .title = QStringLiteral("Open"),
                                     .kind = QStringLiteral("kit-test"),
                                     .nameFilters = QStringLiteral("Bloom (*.bloom)"),
                                     .defaultSuffix = {},
                                     .startPath = {}});
    expect(built->testOption(QFileDialog::DontUseNativeDialog) &&
               built->windowModality() == Qt::WindowModal && built->parentWidget() == &owner,
           "Bloom's own file dialog on every platform, window-modal over the owner");
    bool oneHeight = true;
    for (auto* combo : built->findChildren<QComboBox*>())
        oneHeight = oneHeight && combo->height() == kit::px(kit::Size::Control);
    for (auto* button : built->findChildren<QPushButton*>())
        oneHeight = oneHeight && button->height() == kit::px(kit::Size::Control);
    expect(oneHeight, "the file dialog's combos and buttons are one control tall");
    expect(kit::withDefaultSuffix(QStringLiteral("/a/b.exr"), QStringLiteral("png")) ==
                   QStringLiteral("/a/b.exr") &&
               kit::withDefaultSuffix(QStringLiteral("/a/b"), QStringLiteral("png")) ==
                   QStringLiteral("/a/b.png"),
           "a typed suffix is never replaced");
}

void testTaskDialog(QWidget& owner) {
    kit::KTaskDialog dialog(QStringLiteral("Work"), QStringLiteral("work"), owner);
    int stops = 0;
    QObject::connect(&dialog, &kit::KTaskDialog::stopRequested, [&stops] { ++stops; });
    dialog.showProgress(QStringLiteral("Working"), QStringLiteral("Step 1"));
    open(dialog);
    QTest::keyClick(&dialog, Qt::Key_Return);
    expect(dialog.isVisible(), "Return does nothing while the work runs");
    QTest::keyClick(&dialog, Qt::Key_Escape);
    expect(dialog.isVisible() && stops == 1 && !dialog.cancelButton()->isEnabled() &&
               dialog.headingText() == QStringLiteral("Cancelling…"),
           "Escape asks the work to stop and the dialog says so: visible " +
               std::to_string(dialog.isVisible()) + " stops " + std::to_string(stops) +
               " heading " + dialog.headingText().toStdString());
    dialog.showSummary(QStringLiteral("Done"), QStringLiteral("3 · 1"), {},
                       {{QStringLiteral("a-very-long-file-name.png"), QStringLiteral("Bad")}});
    expect(dialog.summarizing() && dialog.closeButton()->isVisible() &&
               !dialog.cancelButton()->isVisible(),
           "the summary has one Close");
    QTest::keyClick(&dialog, Qt::Key_Return);
    expect(!dialog.isVisible(), "Return closes the summary");
}

void testCheckLabel(QWidget& owner) {
    kit::KDialog dialog(QStringLiteral("Check"), QStringLiteral("check"), owner);
    auto* form = dialog.addForm();
    auto* box = new kit::KCheckBox(&dialog);
    box->setObjectName(QStringLiteral("hardware"));
    (void)form->addCheckRow({}, *box, QStringLiteral("Use hardware"));
    open(dialog);
    auto* words = dialog.findChild<kit::KCheckLabel*>(QStringLiteral("hardwareLabel"));
    expect(words != nullptr && box->accessibleName() == QStringLiteral("Use hardware"),
           "a check row's words name the check box");
    if (words != nullptr)
        QTest::mouseClick(words, Qt::LeftButton);
    expect(box->isChecked(), "clicking the words toggles the check box");
    dialog.reject();
}

void testPathField(QWidget& owner) {
    kit::KPathField field(&owner);
    field.resize(kit::px(kit::Size::DropdownWidthWide), kit::px(kit::Size::Control));
    const auto path = QStringLiteral("/volumes/production/shows/long-show-name/cache/bloom-media");
    field.setText(path);
    field.show();
    QCoreApplication::processEvents();
    expect(field.toolTip() == path, "a path field's tooltip is the whole path");
    // At rest it paints the path elided in the middle: the last folder stays on screen.
    const QImage image = field.grab().toImage();
    expect(!image.isNull(), "a path field paints at rest");
    expect(field.fontMetrics()
               .elidedText(path, Qt::ElideMiddle, field.width())
               .contains(QChar(0x2026)),
           "a path wider than the field is elided");
    field.hide();
}

// With BLOOM_SCREENSHOT_DIR set, review captures of a destructive confirm and a refused prompt.
void writeScreenshots(QWidget& owner, const QString& directory) {
    QDir().mkpath(directory);
    kit::KConfirmSpec spec;
    spec.title = QStringLiteral("Delete");
    spec.objectName = QStringLiteral("shotConfirm");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = QStringLiteral("Delete \u201cPlate 12\u201d? It is used twice in 1 composition.");
    spec.detail = QStringLiteral("Layers that use it will show it as missing. Undo restores it.");
    spec.destructiveText = QStringLiteral("Delete");
    spec.rejectText = QStringLiteral("Cancel");
    spec.defaultRole = Role::Reject;
    std::unique_ptr<kit::KDialog> confirm(kit::buildConfirm(owner, spec));
    open(*confirm);
    QTest::qWait(100);
    expect(confirm->grab().save(directory + QStringLiteral("/kit-confirm.png")), "confirm shot");
    confirm->reject();
    kit::KTextPromptSpec text;
    text.title = QStringLiteral("Rename Composition");
    text.objectName = QStringLiteral("shotPrompt");
    text.label = QStringLiteral("Name");
    text.validate = [](const QString& value) -> std::optional<QString> {
        if (value.isEmpty())
            return QStringLiteral("Enter a non-empty name up to 128 UTF-8 bytes.");
        return std::nullopt;
    };
    std::unique_ptr<kit::KDialog> prompt(kit::buildTextPrompt(owner, text));
    open(*prompt);
    QTest::qWait(100);
    expect(prompt->grab().save(directory + QStringLiteral("/kit-prompt-refused.png")),
           "prompt shot");
    prompt->reject();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Qt's widget file dialog saves its state into the user's Qt settings: not from a test.
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QApplication::setOrganizationName(QStringLiteral("BloomKitDialogTests"));
    QWidget owner;
    owner.resize(800, 600);
    owner.show();
    testOwnerWidthAndFocus(owner);
    testButtonRow(owner);
    testEnterAndValidation(owner);
    testEscapeOrder(owner);
    testControlReturn(owner);
    testFormLabelsAndTabOrder(owner);
    testSectionOpensForItsReason(owner);
    testConfirm(owner);
    testPromptText(owner);
    testPromptRange(owner);
    testChooseFile(owner);
    testTaskDialog(owner);
    testCheckLabel(owner);
    testPathField(owner);
    if (const auto shots = qEnvironmentVariable("BLOOM_SCREENSHOT_DIR"); !shots.isEmpty())
        writeScreenshots(owner, shots);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
