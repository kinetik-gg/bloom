#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <memory>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace bloom::ui {
namespace {

[[nodiscard]] document::CompositionId lowestCompositionId(const document::Project& project) {
    const auto compositions = project.compositions();
    if (compositions.empty()) {
        return {};
    }
    auto lowest = compositions.front().id();
    for (const auto& composition : compositions) {
        if (composition.id().value() < lowest.value()) {
            lowest = composition.id();
        }
    }
    return lowest;
}

struct NewCompositionFields final {
    QLineEdit* name = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QSpinBox* frameRate = nullptr;
    QSpinBox* duration = nullptr;
    QLabel* error = nullptr;
};

} // namespace

std::optional<document::CompositionId> showNewCompositionDialog(CompositionSession& session,
                                                                QWidget* parent) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("newCompositionDialog"));
    dialog.setWindowTitle(QObject::tr("New Composition"));

    auto* form = new QFormLayout(&dialog);
    NewCompositionFields fields;
    fields.name = new kit::KLineEdit(&dialog);
    fields.name->setObjectName(QStringLiteral("assetsNameField"));
    fields.name->setText(
        QObject::tr("Composition %1").arg(session.snapshot().project().compositions().size() + 1));
    form->addRow(QObject::tr("Name"), fields.name);

    const auto* current = session.composition();
    const auto currentFormat =
        current == nullptr ? document::CompositionFormat{} : current->format();
    fields.width = new QSpinBox(&dialog);
    fields.width->setObjectName(QStringLiteral("assetsWidthField"));
    fields.width->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    fields.width->setValue(static_cast<int>(currentFormat.width()));
    form->addRow(QObject::tr("Width"), fields.width);
    fields.height = new QSpinBox(&dialog);
    fields.height->setObjectName(QStringLiteral("assetsHeightField"));
    fields.height->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    fields.height->setValue(static_cast<int>(currentFormat.height()));
    form->addRow(QObject::tr("Height"), fields.height);
    fields.frameRate = new QSpinBox(&dialog);
    fields.frameRate->setObjectName(QStringLiteral("assetsFrameRateField"));
    fields.frameRate->setRange(1, 1000);
    fields.frameRate->setValue(static_cast<int>(currentFormat.frameRate().numerator() /
                                                currentFormat.frameRate().denominator()));
    form->addRow(QObject::tr("Frame rate (fps)"), fields.frameRate);
    fields.duration = new QSpinBox(&dialog);
    fields.duration->setObjectName(QStringLiteral("assetsDurationField"));
    fields.duration->setRange(1, 1'000'000);
    fields.duration->setValue(current == nullptr
                                  ? 10
                                  : static_cast<int>(current->duration().numerator() /
                                                     current->duration().denominator()));
    form->addRow(QObject::tr("Duration (frames)"), fields.duration);

    auto* background = new kit::KColorChip(&dialog);
    background->setObjectName(QStringLiteral("newCompositionBackgroundColor"));
    background->setAccessibleName(QObject::tr("Background Colour"));
    background->setColor({0.0F, 0.0F, 0.0F, 1.0F});
    form->addRow(QObject::tr("Background Colour"), background);
    fields.error = new kit::KLabel(&dialog);
    fields.error->setObjectName(QStringLiteral("newCompositionErrorLabel"));
    fields.error->setWordWrap(true);
    fields.error->hide();
    form->addRow(fields.error);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("assetsDialogButtons"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, &fields] {
        if (!document::isValidHumanFacingName(fields.name->text().toUtf8().toStdString())) {
            fields.error->setText(QObject::tr("Enter a non-empty name up to 128 UTF-8 bytes."));
            fields.error->show();
            fields.name->setFocus();
            return;
        }
        fields.error->hide();
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    const auto rate =
        document::FrameRate::create(static_cast<std::uint32_t>(fields.frameRate->value()), 1);
    const auto format = rate.has_value() ? document::CompositionFormat::create(
                                               static_cast<std::uint32_t>(fields.width->value()),
                                               static_cast<std::uint32_t>(fields.height->value()),
                                               currentFormat.pixelAspect(), *rate)
                                         : std::nullopt;
    if (!rate.has_value() || !format.has_value()) {
        return std::nullopt;
    }

    commands::Transaction transaction(QStringLiteral("Add Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::AddComposition>(
        fields.name->text().toStdString(), *format, *rate,
        core::RationalTime::fromInteger(fields.duration->value()),
        core::Color4d{static_cast<double>(background->color().red),
                      static_cast<double>(background->color().green),
                      static_cast<double>(background->color().blue),
                      static_cast<double>(background->color().alpha)});
    const auto result = session.executeTransaction(std::move(transaction));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kAddCompositionOutput)
               : std::nullopt;
}

std::optional<document::CompositionId> duplicateComposition(CompositionSession& session,
                                                            const document::CompositionId source) {
    if (!source.isValid()) {
        return std::nullopt;
    }
    commands::Transaction transaction(QStringLiteral("Duplicate Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::DuplicateComposition>(source);
    const auto result = session.executeTransaction(std::move(transaction));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kDuplicateCompositionOutput)
               : std::nullopt;
}

bool deleteComposition(CompositionSession& session, const document::CompositionId id) {
    if (!id.isValid()) {
        return false;
    }
    const bool wasActive = session.compositionId() == id;
    commands::Transaction transaction(QStringLiteral("Delete Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::DeleteComposition>(id);
    const auto result = session.executeTransaction(std::move(transaction));
    if (!result.succeeded()) {
        return false;
    }
    if (wasActive && !session.snapshot().project().compositions().empty()) {
        (void)session.setComposition(lowestCompositionId(session.snapshot().project()));
    }
    return true;
}

bool renameComposition(CompositionSession& session, const document::CompositionId id,
                       QWidget* parent) {
    const auto* composition = session.snapshot().project().findComposition(id);
    if (composition == nullptr) {
        return false;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(
        parent, QObject::tr("Rename Composition"), QObject::tr("Name"), QLineEdit::Normal,
        QString::fromStdString(composition->name()), &accepted);
    if (!accepted || !document::isValidHumanFacingName(name.toUtf8().toStdString())) {
        return false;
    }
    commands::Transaction transaction(QStringLiteral("Rename Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::SetCompositionName>(id, name.toStdString());
    return session.executeTransaction(std::move(transaction)).succeeded();
}

} // namespace bloom::ui
