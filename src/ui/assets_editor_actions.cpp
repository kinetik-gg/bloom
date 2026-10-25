#include "asset_input_picker_guard.hpp"
#include "assets_editor_internal.hpp"
#include "reveal_in_file_manager.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <filesystem>
#include <set>
#include <tuple>

namespace bloom::ui {
std::optional<document::AssetFolderId> AssetsEditor::selectedFolder() const {
    return assets::targetFolder(tree_->currentItem() && tree_->currentItem()->isSelected()
                                    ? tree_->currentItem()
                                    : nullptr);
}
std::vector<document::AssetId> AssetsEditor::selectedAssets() const {
    std::vector<document::AssetId> ids;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it)
        if (const auto id = assets::assetId(*it); id.isValid())
            ids.push_back(id);
    return ids;
}
void AssetsEditor::createFolder() {
    const auto parent = selectedFolder();
    std::set<std::string> names;
    for (const auto& folder : session_.snapshot().project().assetFolders())
        if (folder.parent == parent)
            names.insert(folder.name);
    auto name = tr("New Folder").toStdString();
    for (std::size_t suffix = 2; names.contains(name); ++suffix)
        name = tr("New Folder %1").arg(suffix).toStdString();
    search_->clear();
    if (parent)
        collapsedFolders_.remove(parent->value());
    commands::Transaction transaction("New Folder", session_.snapshot().revision());
    transaction.emplace<commands::CreateAssetFolder>(name, parent);
    const auto result = session_.executeTransaction(std::move(transaction));
    const auto id = result.outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    if (!id)
        return;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (assets::folderId(*it) == id) {
            tree_->clearSelection();
            tree_->setCurrentItem(*it);
            beginRename(*it);
            break;
        }
}
void AssetsEditor::editTags(document::AssetId id) {
    const auto* asset = session_.snapshot().project().findAsset(id);
    if (!asset)
        return;
    std::vector<document::AssetId> ids;
    for (const auto* item : tree_->selectedItems()) {
        const auto selected = assets::assetId(item);
        if (selected.isValid())
            ids.push_back(selected);
    }
    if (std::ranges::find(ids, id) == ids.end())
        ids = {id};
    const auto tags = asset->tags;
    const auto snapshot = session_.snapshot();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("assetsTagsDialog"));
    dialog.setWindowTitle(tr("Edit Tags"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));
    auto* label =
        new kit::KLabel(ids.size() > 1 ? tr("Replace tags on %1 selected assets").arg(ids.size())
                                       : tr("One tag per field"),
                        &dialog);
    layout->addWidget(label);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(kit::px(kit::Size::DropdownWidth) * 3, kit::px(kit::Size::ListRow) * 4);
    auto* fields = new QWidget(scroll);
    auto* rows = new QVBoxLayout(fields);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kit::px(kit::Spacing::XS));
    rows->addStretch();
    scroll->setWidget(fields);
    layout->addWidget(scroll);
    auto* add = new kit::KButton(tr("Add Tag"), &dialog);
    add->setObjectName(QStringLiteral("assetsAddTagButton"));
    layout->addWidget(add);
    std::vector<kit::KLineEdit*> editors;
    const auto addTag = [&](const QString& text) {
        if (editors.size() >= 64)
            return;
        auto* row = new QWidget(fields);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(kit::px(kit::Spacing::XS));
        auto* edit = new kit::KLineEdit(text, row);
        edit->setObjectName(QStringLiteral("assetsTagField"));
        edit->setAccessibleName(tr("Tag"));
        edit->setPlaceholderText(tr("Tag"));
        auto* remove = new kit::KIconButton(row);
        remove->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Chrome));
        remove->setToolTip(tr("Remove Tag"));
        remove->setAccessibleName(tr("Remove Tag"));
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(remove);
        rows->insertWidget(rows->count() - 1, row);
        editors.push_back(edit);
        connect(remove, &kit::KIconButton::clicked, &dialog, [&, row, edit] {
            std::erase(editors, edit);
            row->hide();
            row->deleteLater();
            add->setEnabled(true);
        });
        add->setEnabled(editors.size() < 64);
        edit->setFocus();
    };
    for (const auto& tag : tags)
        addTag(QString::fromStdString(tag));
    if (tags.empty())
        addTag({});
    connect(add, &kit::KButton::clicked, &dialog, [&] { addTag({}); });
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new kit::KButton(tr("Cancel"), &dialog);
    auto* apply = new kit::KButton(tr("Apply"), &dialog);
    apply->setVariant(kit::KButton::Variant::Primary);
    apply->setObjectName(QStringLiteral("assetsApplyTagsButton"));
    buttons->addWidget(cancel);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    connect(cancel, &kit::KButton::clicked, &dialog, &QDialog::reject);
    connect(apply, &kit::KButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return;
    std::vector<std::string> edited;
    for (const auto* field : editors) {
        const auto tag = field->text().trimmed();
        if (!tag.isEmpty())
            edited.push_back(tag.toStdString());
    }
    if (&snapshot.project() != &session_.snapshot().project()) {
        emit session_.commandRejected(
            tr("Assets changed while editing tags. Open Edit Tags again."));
        return;
    }
    commands::Transaction transaction("Edit Asset Tags", snapshot.revision());
    transaction.emplace<commands::SetAssetTags>(ids, std::move(edited));
    (void)session_.executeTransaction(std::move(transaction));
}
void AssetsEditor::editInputColor(const document::AssetId id) {
    const auto snapshot = session_.snapshot();
    const auto* asset = snapshot.project().findAsset(id);
    auto* controller = session_.assetController();
    if (!asset || !controller || !controller->acceptsEdits())
        return;
    const auto initialInterpretation = asset->interpretation;
    const auto initialColorIntent = runtime::captureEvaluationColorIntent(session_.colorIntent());
    if (!initialColorIntent) {
        emit session_.commandRejected(tr("The project colour configuration is unavailable."));
        return;
    }
    QDialog dialog(this);
    bool documentRebound = false;
    connect(&session_, &CompositionSession::documentRebound, &dialog,
            [&] { documentRebound = true; });
    dialog.setObjectName(QStringLiteral("assetsInputColorDialog"));
    dialog.setWindowTitle(tr("Input Colour — %1").arg(QString::fromStdString(asset->name)));
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new kit::KLabel(
        tr("Choose how this asset's pixels are interpreted. A source node can override this "
           "choice in Properties."),
        &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* selector = new kit::KDropdown(&dialog);
    selector->setObjectName(QStringLiteral("assetsInputColorSelector"));
    selector->setSearchable(true);
    selector->setWidthFloor(kit::px(kit::Size::DropdownWidth) * 2);
    layout->addWidget(selector);
    auto* status = new kit::KLabel(&dialog);
    status->setObjectName(QStringLiteral("assetsInputColorStatus"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    buttons->setObjectName(QStringLiteral("assetsInputColorButtons"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    const auto initial = [&] {
        if (!asset->interpretation.inputColorSpaceId.empty())
            return QStringLiteral("id:") +
                   QString::fromStdString(asset->interpretation.inputColorSpaceId);
        switch (asset->interpretation.colorSpace) {
        case document::AssetColorSpace::Raw:
            return QStringLiteral("raw");
        case document::AssetColorSpace::Srgb:
            return QStringLiteral("srgb");
        case document::AssetColorSpace::Linear:
            return QStringLiteral("linear");
        case document::AssetColorSpace::Auto:
            return QStringLiteral("auto");
        }
        return QStringLiteral("auto");
    }();
    const auto refreshChoices = [&] {
        const QString selected = selector->count() ? selector->currentData().toString() : initial;
        const QSignalBlocker blocked(selector);
        selector->clearItems();
        selector->addItem(tr("Automatic (from media tags)"), QStringLiteral("auto"));
        selector->addItem(tr("Raw data (no colour conversion)"), QStringLiteral("raw"));
        selector->addItem(tr("Legacy sRGB"), QStringLiteral("srgb"));
        selector->addItem(tr("Legacy Linear"), QStringLiteral("linear"));
        if (const auto* spaces = controller->inputColorSpaces()) {
            auto ordered = *spaces;
            std::ranges::sort(ordered, [](const auto& left, const auto& right) {
                return std::tie(left.family, left.id) < std::tie(right.family, right.id);
            });
            for (const auto& space : ordered) {
                const auto name = QString::fromStdString(space.id);
                const auto family = QString::fromStdString(space.family);
                selector->addItem(family.isEmpty() ? name : family + QStringLiteral(" / ") + name,
                                  QStringLiteral("id:") + name);
            }
            status->setText(tr("OCIO colour spaces from the selected project configuration."));
        } else if (!controller->inputColorCatalogDiagnostic().isEmpty()) {
            status->setText(controller->inputColorCatalogDiagnostic());
        } else {
            status->setText(tr("Preparing OCIO colour spaces…"));
        }
        if (selector->findData(selected) < 0 && selected.startsWith(QStringLiteral("id:"))) {
            selector->addItem(tr("Current space unavailable: %1").arg(selected.mid(3)), selected);
        }
        const auto index = selector->findData(selected);
        selector->setCurrentIndex(index < 0 ? 0 : index);
    };
    connect(controller, &AssetController::changed, &dialog, refreshChoices);
    controller->requestInputColorSpaces();
    refreshChoices();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto live = session_.snapshot();
    if (documentRebound ||
        !detail::assetInputPickerTargetMatches(snapshot.project().id(), id, initialInterpretation,
                                               live.project()) ||
        session_.colorIntent() != *initialColorIntent) {
        emit session_.commandRejected(tr("This asset or its colour configuration changed while "
                                         "choosing input colour. Open the picker again."));
        return;
    }
    const auto choice = selector->currentData().toString();
    if (choice == initial)
        return;
    document::AssetColorSpace legacy = document::AssetColorSpace::Auto;
    std::string inputId;
    if (choice == QStringLiteral("raw"))
        legacy = document::AssetColorSpace::Raw;
    else if (choice == QStringLiteral("srgb"))
        legacy = document::AssetColorSpace::Srgb;
    else if (choice == QStringLiteral("linear"))
        legacy = document::AssetColorSpace::Linear;
    else if (choice.startsWith(QStringLiteral("id:"))) {
        inputId = choice.mid(3).toStdString();
        const auto* spaces = controller->inputColorSpaces();
        if (!spaces ||
            std::ranges::none_of(*spaces, [&](const auto& space) { return space.id == inputId; })) {
            emit session_.commandRejected(tr("That OCIO colour space is unavailable. Reopen the "
                                             "picker after the configuration is ready."));
            return;
        }
    } else if (choice != QStringLiteral("auto"))
        return;
    commands::Transaction transaction("Set Asset Input Colour", live.revision());
    transaction.emplace<commands::SetAssetInputColor>(id, legacy, std::move(inputId));
    (void)session_.executeTransaction(std::move(transaction));
}
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
} // namespace
void AssetsEditor::removeSelected() {
    const auto snapshot = session_.snapshot();
    const auto& project = snapshot.project();
    std::vector<document::AssetId> assets;
    std::vector<document::CompositionId> compositions;
    std::vector<document::AssetFolderId> folders;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it) {
        // A selected folder already accounts for what it holds.
        bool insideSelectedFolder = false;
        for (auto* parent = (*it)->parent(); parent; parent = parent->parent())
            insideSelectedFolder = insideSelectedFolder || parent->isSelected();
        if (insideSelectedFolder)
            continue;
        if (const auto asset = assets::assetId(*it); asset.isValid())
            assets.push_back(asset);
        else if (const auto composition = assets::compositionId(*it); composition.isValid())
            compositions.push_back(composition);
        else if (const auto folder = assets::folderId(*it))
            folders.push_back(*folder);
    }
    if (assets.empty() && compositions.empty() && folders.empty())
        return;
    FolderContents contents;
    for (const auto folder : folders)
        collect(project, folder, contents);
    const bool holdsItems = contents.folders.size() > folders.size() || !contents.assets.empty() ||
                            !contents.compositions.empty();
    // Used items among those that would be deleted, with or without the folders' contents.
    std::size_t usedDirect = 0, usedInside = 0;
    for (const auto asset : assets)
        usedDirect += assets::assetUsage(project, asset).uses > 0 ? 1 : 0;
    for (const auto composition : compositions)
        usedDirect += assets::compositionUsage(project, composition).uses > 0 ? 1 : 0;
    for (const auto asset : contents.assets)
        usedInside += assets::assetUsage(project, asset).uses > 0 ? 1 : 0;
    for (const auto composition : contents.compositions)
        usedInside += assets::compositionUsage(project, composition).uses > 0 ? 1 : 0;
    bool withContents = false;
    if (usedDirect > 0 || holdsItems) {
        // As After Effects does, a used item is never deleted without saying so first.
        QMessageBox box(this);
        box.setObjectName(QStringLiteral("assetsDeleteConfirmation"));
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Delete"));
        QString text;
        if (assets.size() + compositions.size() == 1 && folders.empty() && usedDirect == 1) {
            const auto usage = assets.empty()
                                   ? assets::compositionUsage(project, compositions.front())
                                   : assets::assetUsage(project, assets.front());
            const auto name = assets.empty() ? project.findComposition(compositions.front())->name()
                                             : project.findAsset(assets.front())->name;
            text =
                tr("\u201c%1\u201d is used %2 in %3.")
                    .arg(QString::fromStdString(name),
                         usage.uses == 1 ? tr("once") : tr("%1 times").arg(usage.uses),
                         usage.compositions == 1 ? tr("1 composition")
                                                 : tr("%1 compositions").arg(usage.compositions));
        } else if (usedDirect > 0)
            text = tr("%n of the selected items are used in compositions.", nullptr,
                      static_cast<int>(usedDirect));
        if (holdsItems) {
            const auto count = contents.assets.size() + contents.compositions.size() +
                               contents.folders.size() - folders.size();
            if (!text.isEmpty())
                text += QStringLiteral("\n");
            text += folders.size() == 1 ? tr("The folder holds %1 items.").arg(count)
                                        : tr("The folders hold %1 items.").arg(count);
            if (usedInside > 0)
                text += QStringLiteral(" ") +
                        tr("%1 of them are used in compositions.").arg(usedInside);
        }
        box.setText(text);
        box.setInformativeText(
            holdsItems ? tr("Delete only the folders and move their contents up, or delete the "
                            "contents too? Undo restores either.")
                       : tr("Layers that use it will show it as missing. Undo restores it."));
        QAbstractButton* keep =
            holdsItems ? box.addButton(tr("Delete Folders Only"), QMessageBox::AcceptRole)
                       : nullptr;
        QAbstractButton* everything = box.addButton(
            holdsItems ? tr("Delete with Contents") : tr("Delete"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        if (keep)
            keep->setObjectName(QStringLiteral("assetsDeleteFoldersOnlyButton"));
        everything->setObjectName(QStringLiteral("assetsDeleteConfirmButton"));
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        const auto* clicked = box.clickedButton();
        if (clicked == nullptr || (clicked != keep && clicked != everything))
            return;
        withContents = clicked == everything;
        if (&snapshot.project() != &session_.snapshot().project()) {
            emit session_.commandRejected(tr("Assets changed while confirming. Delete again."));
            return;
        }
    }
    const bool wasActive = [&] {
        const auto active = session_.compositionId();
        return std::ranges::find(compositions, active) != compositions.end() ||
               (withContents &&
                std::ranges::find(contents.compositions, active) != contents.compositions.end());
    }();
    commands::Transaction transaction("Delete", snapshot.revision());
    for (const auto asset : assets)
        transaction.emplace<commands::RemoveAsset>(asset);
    for (const auto composition : compositions)
        transaction.emplace<commands::DeleteComposition>(composition);
    if (withContents) {
        for (const auto asset : contents.assets)
            transaction.emplace<commands::RemoveAsset>(asset);
        for (const auto composition : contents.compositions)
            transaction.emplace<commands::DeleteComposition>(composition);
        for (const auto folder : contents.folders)
            transaction.emplace<commands::RemoveAssetFolder>(folder);
    } else
        for (const auto folder : folders)
            transaction.emplace<commands::RemoveAssetFolder>(folder);
    if (!session_.executeTransaction(std::move(transaction)).succeeded())
        return;
    const auto remaining = session_.snapshot().project().compositions();
    if (wasActive && !remaining.empty())
        (void)session_.setComposition(std::ranges::min(remaining, {}, [](const auto& composition) {
                                          return composition.id().value();
                                      }).id());
}
void AssetsEditor::revealInFileManager(const document::AssetId id) {
    const auto* controller = session_.assetController();
    const auto path = controller ? controller->sourcePath(id) : std::filesystem::path{};
    // Selects the file where the platform's file manager can, else opens its folder (#380).
    ui::revealInFileManager(path, this);
}
void AssetsEditor::newCompositionFromSelection() {
    const auto snapshot = session_.snapshot();
    const auto& project = snapshot.project();
    std::vector<document::AssetId> footage;
    for (const auto id : selectedAssets())
        if (const auto* asset = project.findAsset(id); asset &&
                                                       asset->kind != document::AssetKind::Lut &&
                                                       asset->kind != document::AssetKind::Font)
            footage.push_back(id);
    if (footage.empty())
        return;
    // Sized from the first visual footage item, like After Effects; the active composition (or
    // the defaults) supplies what the footage does not state.
    commands::CompositionSettings base;
    base.name = tr("Composition %1").arg(project.compositions().size() + 1).toStdString();
    base.duration = core::RationalTime::fromInteger(10);
    if (const auto* current = session_.composition())
        base = commands::compositionSettingsOf(*current);
    const auto* first = project.findAsset(footage.front());
    for (const auto id : footage)
        if (const auto* asset = project.findAsset(id); asset->kind != document::AssetKind::Audio) {
            first = asset;
            break;
        }
    auto settings = commands::compositionSettingsForAsset(*first, base);
    settings.timecode = {};
    commands::Transaction transaction("New Composition from Selection", snapshot.revision());
    transaction.emplace<commands::AddCompositionFromAssets>(
        std::move(settings), footage, assets::targetFolder(tree_->currentItem()));
    const auto result = session_.executeTransaction(std::move(transaction));
    if (const auto id = result.outputId<document::CompositionId>(commands::kAddCompositionOutput))
        openComposition(*id);
}
} // namespace bloom::ui
