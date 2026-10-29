#include "project_item_delete.hpp"

#include "assets_editor_internal.hpp"
#include "project_change_check.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <memory>

namespace bloom::ui {
namespace {

// Everything a folder holds, deepest folders first so they can be removed bottom-up.
struct FolderContents final {
    std::vector<document::AssetFolderId> folders;
    std::vector<document::AssetId> assets;
    std::vector<document::CompositionId> compositions;
};

void collect(const document::Project& project, const document::AssetFolderId folder,
             FolderContents& contents) {
    for (const auto& child : project.assetFolders())
        if (child.parent == folder)
            collect(project, child.id, contents);
    for (const auto& asset : project.assets())
        if (asset.folder == folder)
            contents.assets.push_back(asset.id);
    for (const auto& composition : project.compositions())
        if (composition.folder() == folder)
            contents.compositions.push_back(composition.id());
    contents.folders.push_back(folder);
}

void commitDelete(CompositionSession& session, const document::Snapshot& snapshot,
                  const ProjectItemSelection& selection, const FolderContents& contents,
                  const bool withContents) {
    const bool wasActive = [&] {
        const auto active = session.compositionId();
        return std::ranges::find(selection.compositions, active) != selection.compositions.end() ||
               (withContents &&
                std::ranges::find(contents.compositions, active) != contents.compositions.end());
    }();
    commands::Transaction transaction("Delete", snapshot.revision());
    for (const auto asset : selection.assets)
        transaction.emplace<commands::RemoveAsset>(asset);
    for (const auto composition : selection.compositions)
        transaction.emplace<commands::DeleteComposition>(composition);
    if (withContents) {
        for (const auto asset : contents.assets)
            transaction.emplace<commands::RemoveAsset>(asset);
        for (const auto composition : contents.compositions)
            transaction.emplace<commands::DeleteComposition>(composition);
        for (const auto folder : contents.folders)
            transaction.emplace<commands::RemoveAssetFolder>(folder);
    } else {
        for (const auto folder : selection.folders)
            transaction.emplace<commands::RemoveAssetFolder>(folder);
    }
    if (!session.executeTransaction(std::move(transaction)).succeeded())
        return;
    const auto remaining = session.snapshot().project().compositions();
    if (wasActive && !remaining.empty())
        (void)session.setComposition(std::ranges::min(remaining, {}, [](const auto& composition) {
                                         return composition.id().value();
                                     }).id());
}

} // namespace

void requestDeleteProjectItems(CompositionSession& session, QWidget& owner,
                               ProjectItemSelection selection) {
    if (selection.assets.empty() && selection.compositions.empty() && selection.folders.empty())
        return;
    auto check = std::make_shared<ProjectChangeCheck>(session);
    const auto& project = check->snapshot().project();
    FolderContents contents;
    for (const auto folder : selection.folders)
        collect(project, folder, contents);
    const bool holdsItems = contents.folders.size() > selection.folders.size() ||
                            !contents.assets.empty() || !contents.compositions.empty();
    // Used items among those that would be deleted, with or without the folders' contents.
    std::size_t usedDirect = 0;
    std::size_t usedInside = 0;
    for (const auto asset : selection.assets)
        usedDirect += assets::assetUsage(project, asset).uses > 0 ? 1 : 0;
    for (const auto composition : selection.compositions)
        usedDirect += assets::compositionUsage(project, composition).uses > 0 ? 1 : 0;
    for (const auto asset : contents.assets)
        usedInside += assets::assetUsage(project, asset).uses > 0 ? 1 : 0;
    for (const auto composition : contents.compositions)
        usedInside += assets::compositionUsage(project, composition).uses > 0 ? 1 : 0;
    if (usedDirect == 0 && !holdsItems) {
        commitDelete(session, check->snapshot(), selection, contents, false);
        return;
    }

    // As After Effects does, a used item is never deleted without saying so first.
    QString heading;
    const auto single =
        selection.assets.size() + selection.compositions.size() == 1 && selection.folders.empty();
    if (single && usedDirect == 1) {
        const auto usage = selection.assets.empty()
                               ? assets::compositionUsage(project, selection.compositions.front())
                               : assets::assetUsage(project, selection.assets.front());
        const auto name = selection.assets.empty()
                              ? project.findComposition(selection.compositions.front())->name()
                              : project.findAsset(selection.assets.front())->name;
        heading = QObject::tr("Delete “%1”? It is used %2 in %3.")
                      .arg(QString::fromStdString(name),
                           usage.uses == 1 ? QObject::tr("once")
                                           : QObject::tr("%1 times").arg(usage.uses),
                           usage.compositions == 1
                               ? QObject::tr("1 composition")
                               : QObject::tr("%1 compositions").arg(usage.compositions));
    } else if (usedDirect > 0) {
        heading = QObject::tr("Delete %n used items?", nullptr, static_cast<int>(usedDirect));
    }
    QString detail;
    if (holdsItems) {
        const auto count = contents.assets.size() + contents.compositions.size() +
                           contents.folders.size() - selection.folders.size();
        if (heading.isEmpty())
            heading = selection.folders.size() == 1
                          ? QObject::tr("Delete the folder and its %1 items?").arg(count)
                          : QObject::tr("Delete the folders and their %1 items?").arg(count);
        else
            detail = selection.folders.size() == 1
                         ? QObject::tr("The folder holds %1 items.").arg(count) + QLatin1Char(' ')
                         : QObject::tr("The folders hold %1 items.").arg(count) + QLatin1Char(' ');
        if (usedInside > 0)
            detail += QObject::tr("%1 of them are used in compositions.").arg(usedInside) +
                      QLatin1Char(' ');
        detail += QObject::tr("Delete only the folders and move their contents up, or delete the "
                              "contents too? Undo restores either.");
    } else {
        detail = QObject::tr("Layers that use it will show it as missing. Undo restores it.");
    }

    kit::KConfirmSpec spec;
    spec.title = QObject::tr("Delete");
    spec.objectName = QStringLiteral("assetsDeleteConfirmation");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = heading;
    spec.detail = detail;
    spec.destructiveText = holdsItems ? QObject::tr("Delete with Contents") : QObject::tr("Delete");
    if (holdsItems)
        spec.alternateText = QObject::tr("Delete Folders Only");
    spec.rejectText = QObject::tr("Cancel");
    // Cancel is the default: a stray Return deletes nothing.
    spec.defaultRole = kit::KDialog::Role::Reject;
    QPointer<CompositionSession> guard(&session);
    kit::confirm(owner, spec,
                 [guard, check, selection = std::move(selection),
                  contents = std::move(contents)](const kit::KDialog::Role role) {
                     if (!guard || (role != kit::KDialog::Role::Destructive &&
                                    role != kit::KDialog::Role::Alternate))
                         return;
                     if (check->projectChanged()) {
                         emit guard->commandRejected(
                             QObject::tr("The project changed while confirming. Delete again."));
                         return;
                     }
                     commitDelete(*guard, check->snapshot(), selection, contents,
                                  role == kit::KDialog::Role::Destructive);
                 });
}

} // namespace bloom::ui
