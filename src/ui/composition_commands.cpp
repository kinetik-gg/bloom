#include <bloom/ui/composition_commands.hpp>

#include "composition_editor_support.hpp"
#include "composition_settings_dialog.hpp"
#include "project_item_delete.hpp"
#include <bloom/ui/kit/dialog_prompts.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/safe_parse.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace

std::optional<document::CompositionId>
showNewCompositionDialog(CompositionSession& session, QWidget& owner,
                         const std::optional<document::AssetFolderId> folder) {
    // A new composition starts from the current one's frame, rate and duration, or the defaults.
    const auto* current = session.composition();
    commands::CompositionSettings initial;
    initial.name = QObject::tr("Composition %1")
                       .arg(session.snapshot().project().compositions().size() + 1)
                       .toStdString();
    initial.format = current == nullptr ? document::CompositionFormat{} : current->format();
    initial.duration =
        current == nullptr ? core::RationalTime::fromInteger(10) : current->duration();
    CompositionSettingsDialog dialog(CompositionSettingsDialog::Mode::Create, std::move(initial),
                                     workingColorSpaceChoices(session.colorSettings()), owner);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    const auto settings = dialog.settings();
    if (!settings)
        return std::nullopt;
    commands::Transaction transaction(QStringLiteral("Add Composition").toStdString(),
                                      session.snapshot().revision());
    if (folder)
        transaction.emplace<commands::AddCompositionFromAssets>(
            *settings, std::vector<document::AssetId>{}, folder);
    else
        transaction.emplace<commands::AddCompositionWithSettings>(*settings);
    const auto result = session.executeTransaction(std::move(transaction));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kAddCompositionOutput)
               : std::nullopt;
}

bool showCompositionSettingsDialog(CompositionSession& session, const document::CompositionId id,
                                   QWidget& owner) {
    const auto* composition = session.snapshot().project().findComposition(id);
    if (composition == nullptr)
        return false;
    CompositionSettingsDialog dialog(CompositionSettingsDialog::Mode::Edit,
                                     commands::compositionSettingsOf(*composition),
                                     workingColorSpaceChoices(session.colorSettings()), owner);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    const auto settings = dialog.settings();
    if (!settings)
        return false;
    return applyCompositionSettings(session, id, *settings, dialog.anchor());
}

bool applyCompositionSettings(CompositionSession& session, const document::CompositionId id,
                              const commands::CompositionSettings& settings,
                              const commands::ResizeAnchor anchor) {
    const auto* composition = session.snapshot().project().findComposition(id);
    if (composition == nullptr)
        return false;
    commands::Transaction transaction(QStringLiteral("Composition Settings").toStdString(),
                                      session.snapshot().revision());
    auto backgroundOnly = commands::compositionSettingsOf(*composition);
    backgroundOnly.background = settings.background;
    if (settings == backgroundOnly)
        transaction.emplace<commands::SetCompositionBackgroundColor>(id, settings.background);
    else
        transaction.emplace<commands::SetCompositionSettings>(id, settings, anchor);
    return session.executeTransaction(std::move(transaction)).succeeded();
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

void requestDeleteComposition(CompositionSession& session, const document::CompositionId id,
                              QWidget& owner) {
    if (!id.isValid())
        return;
    ProjectItemSelection selection;
    selection.compositions = {id};
    requestDeleteProjectItems(session, owner, std::move(selection));
}

void renameComposition(CompositionSession& session, const document::CompositionId id,
                       QWidget& owner) {
    const auto* composition = session.snapshot().project().findComposition(id);
    if (composition == nullptr)
        return;
    kit::KTextPromptSpec spec;
    spec.title = QObject::tr("Rename Composition");
    spec.objectName = QStringLiteral("compositionRenamePrompt");
    spec.label = QObject::tr("Name");
    spec.text = QString::fromStdString(composition->name());
    spec.acceptText = QObject::tr("Rename");
    spec.validate = [](const QString& name) -> std::optional<QString> {
        if (!document::isValidHumanFacingName(name.toUtf8().toStdString()))
            return QObject::tr("Enter a non-empty name up to 128 UTF-8 bytes.");
        return std::nullopt;
    };
    QPointer<CompositionSession> guard(&session);
    kit::promptText(owner, spec, [guard, id](const std::optional<QString>& name) {
        if (!guard || !name || guard->snapshot().project().findComposition(id) == nullptr)
            return;
        commands::Transaction transaction(QStringLiteral("Rename Composition").toStdString(),
                                          guard->snapshot().revision());
        transaction.emplace<commands::SetCompositionName>(id, name->toStdString());
        (void)guard->executeTransaction(std::move(transaction));
    });
}

} // namespace bloom::ui
