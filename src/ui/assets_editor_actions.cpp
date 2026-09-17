#include "assets_editor_internal.hpp"
#include <QDialog>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <set>

namespace bloom::ui {
void AssetsEditor::createFolder() {
    auto parent = assets::folderId(tree_->currentItem());
    if (const auto* asset =
            session_.snapshot().project().findAsset(assets::assetId(tree_->currentItem())))
        parent = asset->folder;
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
            tree_->scrollToItem(*it);
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
void AssetsEditor::removeSelected() {
    commands::Transaction transaction("Remove Assets", session_.snapshot().revision());
    bool any = false;
    for (const auto* item : tree_->selectedItems()) {
        const auto asset = assets::assetId(item);
        if (asset.isValid()) {
            transaction.emplace<commands::RemoveAsset>(asset);
            any = true;
        } else if (const auto folder = assets::folderId(item)) {
            transaction.emplace<commands::RemoveAssetFolder>(*folder);
            any = true;
        }
    }
    if (any)
        (void)session_.executeTransaction(std::move(transaction));
    else if (const auto id = assets::compositionId(tree_->currentItem()); id.isValid())
        deleteComposition(id);
}
} // namespace bloom::ui
