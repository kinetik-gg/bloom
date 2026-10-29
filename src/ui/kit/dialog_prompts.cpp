#include <bloom/ui/kit/dialog_prompts.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPointer>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] IconId toneIcon(const KPromptTone tone) {
    switch (tone) {
    case KPromptTone::Warning:
        return IconId::Warning;
    case KPromptTone::Error:
        return IconId::Error;
    case KPromptTone::Question:
    case KPromptTone::Info:
        return IconId::Info;
    }
    return IconId::Info;
}

[[nodiscard]] Color toneInk(const KPromptTone tone) {
    switch (tone) {
    case KPromptTone::Warning:
        return Color::Warn;
    case KPromptTone::Error:
        return Color::Error;
    case KPromptTone::Question:
    case KPromptTone::Info:
        return Color::Muted;
    }
    return Color::Muted;
}

constexpr const char* kFirstField = "kRangeFirst";
constexpr const char* kLastField = "kRangeLast";

[[nodiscard]] KValueField* rangeField(QWidget& parent, const QString& name, const QString& label,
                                      const KRangePromptSpec& spec, const std::int64_t value) {
    auto* field = new KValueField(&parent);
    field->setObjectName(name);
    field->setAccessibleName(label);
    field->setDecimals(0);
    field->setSingleStep(1.0);
    field->setRange(static_cast<double>(spec.minimum), static_cast<double>(spec.maximum));
    field->setValue(static_cast<double>(value));
    return field;
}

} // namespace

KDialog* buildConfirm(QWidget& owner, const KConfirmSpec& spec) {
    auto* dialog = new KDialog(spec.title, spec.objectName, owner);
    // Read by the application's interaction-sound observer: an Error prompt is heard as a warning.
    dialog->setProperty("kitPromptTone", static_cast<int>(spec.tone));
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(px(Spacing::M));
    auto* glyph = new KLabel(QString{}, dialog);
    glyph->setObjectName(spec.objectName.isEmpty() ? QStringLiteral("kPromptGlyph")
                                                   : spec.objectName + QStringLiteral("Glyph"));
    glyph->setPixmap(iconPixmap(toneIcon(spec.tone), Size::IconLarge, toneInk(spec.tone),
                                State::Normal, IconWeight::Bold));
    glyph->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    row->addWidget(glyph, 0, Qt::AlignTop);
    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(px(Spacing::XS));
    if (!spec.heading.isEmpty()) {
        auto* heading = new KLabel(spec.heading, dialog, TypeRole::Title);
        heading->setObjectName(spec.objectName + QStringLiteral("Heading"));
        heading->setWordWrap(true);
        text->addWidget(heading);
    }
    if (!spec.detail.isEmpty()) {
        auto* detail = new KLabel(spec.detail, dialog);
        detail->setObjectName(spec.objectName + QStringLiteral("Detail"));
        detail->setWordWrap(true);
        // Without a heading the detail is the message itself.
        detail->setTone(spec.heading.isEmpty() ? Color::Foreground : Color::Muted);
        detail->setToolTip(spec.detailToolTip);
        text->addWidget(detail);
    }
    row->addLayout(text, 1);
    dialog->contentLayout()->addLayout(row);
    if (!spec.document.isEmpty()) {
        auto* document = new QPlainTextEdit(spec.document, dialog);
        document->setObjectName(spec.objectName + QStringLiteral("Document"));
        document->setReadOnly(true);
        styleDocumentView(*document);
        document->setFont(font(TypeRole::UiSmall));
        document->setMaximumHeight(px(Size::DiagnosticHeight));
        dialog->contentLayout()->addWidget(document);
    }
    dialog->contentLayout()->addStretch(1);
    if (!spec.acceptText.isEmpty())
        (void)dialog->addButton(KDialog::Role::Accept, spec.acceptText);
    if (!spec.destructiveText.isEmpty())
        (void)dialog->addButton(KDialog::Role::Destructive, spec.destructiveText);
    if (!spec.alternateText.isEmpty())
        (void)dialog->addButton(KDialog::Role::Alternate, spec.alternateText);
    if (!spec.rejectText.isEmpty())
        (void)dialog->addButton(KDialog::Role::Reject, spec.rejectText);
    dialog->setDefaultRole(spec.defaultRole);
    return dialog;
}

KDialog::Role confirm(QWidget& owner, const KConfirmSpec& spec) {
    std::unique_ptr<KDialog> dialog(buildConfirm(owner, spec));
    dialog->exec();
    return dialog->chosenRole();
}

void confirm(QWidget& owner, const KConfirmSpec& spec, std::function<void(KDialog::Role)> done) {
    buildConfirm(owner, spec)->open(std::move(done));
}

void inform(QWidget& owner, const KPromptTone tone, const QString& title, const QString& heading,
            const QString& detail, const QString& objectName) {
    KConfirmSpec spec;
    spec.title = title;
    spec.objectName = objectName;
    spec.tone = tone;
    spec.heading = heading;
    spec.detail = detail;
    spec.acceptText = QObject::tr("OK");
    buildConfirm(owner, spec)->open([](KDialog::Role) {});
}

KDialog* buildTextPrompt(QWidget& owner, const KTextPromptSpec& spec) {
    auto* dialog = new KDialog(spec.title, spec.objectName, owner);
    auto* form = dialog->addForm();
    auto* field = new KLineEdit(spec.text, dialog);
    field->setObjectName(spec.objectName + QStringLiteral("Field"));
    field->setAccessibleName(spec.label);
    field->setPlaceholderText(spec.placeholder);
    (void)form->addRow(spec.label, field);
    dialog->contentLayout()->addStretch(1);
    (void)dialog->addButton(KDialog::Role::Accept,
                            spec.acceptText.isEmpty() ? QObject::tr("OK") : spec.acceptText);
    (void)dialog->addButton(KDialog::Role::Reject, QObject::tr("Cancel"));
    if (spec.validate)
        dialog->setValidator([field, validate = spec.validate]() -> std::optional<KRefusal> {
            if (auto reason = validate(field->text()))
                return KRefusal{field, *reason};
            return std::nullopt;
        });
    dialog->watch(field);
    dialog->setInitialFocus(field);
    return dialog;
}

std::optional<QString> promptText(QWidget& owner, const KTextPromptSpec& spec) {
    std::unique_ptr<KDialog> dialog(buildTextPrompt(owner, spec));
    if (dialog->exec() != QDialog::Accepted)
        return std::nullopt;
    return dialog->findChild<KLineEdit*>(spec.objectName + QStringLiteral("Field"))->text();
}

void promptText(QWidget& owner, const KTextPromptSpec& spec,
                std::function<void(std::optional<QString>)> done) {
    auto* dialog = buildTextPrompt(owner, spec);
    QPointer<KLineEdit> field =
        dialog->findChild<KLineEdit*>(spec.objectName + QStringLiteral("Field"));
    dialog->open([field, done = std::move(done)](const KDialog::Role role) {
        if (done)
            done(role == KDialog::Role::Accept && field ? std::optional<QString>(field->text())
                                                        : std::nullopt);
    });
}

KDialog* buildRangePrompt(QWidget& owner, const KRangePromptSpec& spec) {
    auto* dialog = new KDialog(spec.title, spec.objectName, owner);
    auto* form = dialog->addForm();
    if (spec.addRows)
        spec.addRows(*dialog, *form);
    auto* first =
        rangeField(*dialog, QString::fromLatin1(kFirstField), spec.firstLabel, spec, spec.first);
    auto* last =
        rangeField(*dialog, QString::fromLatin1(kLastField), spec.lastLabel, spec, spec.last);
    (void)form->addRow(spec.firstLabel, first);
    (void)form->addRow(spec.lastLabel, last);
    dialog->contentLayout()->addStretch(1);
    (void)dialog->addButton(KDialog::Role::Accept,
                            spec.acceptText.isEmpty() ? QObject::tr("OK") : spec.acceptText);
    (void)dialog->addButton(KDialog::Role::Reject, QObject::tr("Cancel"));
    dialog->watch(first);
    dialog->watch(last);
    dialog->setValidator(
        [first, last, validate = spec.validate,
         order = spec.orderReason.isEmpty()
                     ? QObject::tr("The last value must not come before the first.")
                     : spec.orderReason]() -> std::optional<KRefusal> {
            if (validate)
                if (auto refusal = validate())
                    return refusal;
            if (first->value() > last->value())
                return KRefusal{last, order};
            return std::nullopt;
        });
    return dialog;
}

KRange rangeOf(const KDialog& prompt) {
    const auto* first = prompt.findChild<KValueField*>(QString::fromLatin1(kFirstField));
    const auto* last = prompt.findChild<KValueField*>(QString::fromLatin1(kLastField));
    return {first != nullptr ? static_cast<std::int64_t>(first->value()) : 0,
            last != nullptr ? static_cast<std::int64_t>(last->value()) : 0};
}

std::optional<KRange> promptRange(QWidget& owner, const KRangePromptSpec& spec) {
    std::unique_ptr<KDialog> dialog(buildRangePrompt(owner, spec));
    if (dialog->exec() != QDialog::Accepted)
        return std::nullopt;
    return rangeOf(*dialog);
}

} // namespace bloom::ui::kit
