#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include <QAbstractItemDelegate>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <functional>
#include <memory>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QAction>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <array>
#include <cstdint>
#include <optional>

namespace bloom::ui {
AssetsEditor::AssetsEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("assetsEditor"));
    setAccessibleName(tr("Project assets editor"));

    auto* layout = new QVBoxLayout(this);
    const int gutter = kit::px(kit::Spacing::S);
    layout->setContentsMargins(gutter, gutter, gutter, gutter);
    layout->setSpacing(kit::px(kit::Spacing::Gutter));

    auto* filters = new QWidget(this);
    filters->setObjectName(QStringLiteral("assetsKindFilterRow"));
    auto* filterLayout = new QHBoxLayout(filters);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(kit::px(kit::Spacing::XS));
    filterButtons_ = new QButtonGroup(this);
    filterButtons_->setExclusive(true);
    const std::array labels{tr("Media"), tr("Data"), tr("Compositions")};
    for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
        auto* button = new kit::KButton(labels[static_cast<std::size_t>(index)], filters);
        button->setObjectName(QStringLiteral("assetsKindFilter%1").arg(index));
        button->setVariant(kit::KButton::Variant::Ghost);
        button->setControlSize(kit::KButton::ControlSize::Compact);
        button->setCheckable(true);
        button->setChecked(index == filterMode_);
        filterButtons_->addButton(button, index);
        filterLayout->addWidget(button);
    }
    filterLayout->addStretch(1);
    connect(filterButtons_, &QButtonGroup::idClicked, this, &AssetsEditor::setFilter);
    layout->addWidget(filters);

    search_ = new kit::KSearchField(this);
    search_->setObjectName(QStringLiteral("assetsSearchField"));
    search_->setAccessibleName(tr("Search assets"));
    search_->setPlaceholderText(tr("Search assets…"));
    layout->addWidget(search_);

    tree_ = new assets::AssetTree(session_, this);
    tree_->setObjectName(QStringLiteral("assetsTree"));
    tree_->setProperty("kitRows", true);
    tree_->setAccessibleName(tr("Assets"));
    tree_->setHeaderLabels({tr("Name"), tr("Kind")});
    tree_->setRootIsDecorated(true);
    tree_->setIndentation(kit::px(kit::Size::ToggleCell));
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->setColumnWidth(1, kit::px(kit::Size::DropdownWidth));
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &AssetsEditor::refreshRowSelection);
    layout->addWidget(tree_, 1);

    connect(search_, &QLineEdit::textChanged, this, &AssetsEditor::applyFilter);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, const int column) {
                if (column == 0) {
                    openComposition(assets::compositionId(item));
                }
            });
    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, const int column) { commitRename(item, column); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint position) { showContextMenu(position); });

    connect(tree_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
            [this] { QTimer::singleShot(0, this, &AssetsEditor::rebuild); });
    const auto expanded = [this](QTreeWidgetItem* item) {
        if (search_->text().trimmed().isEmpty()) {
            if (const auto folder = assets::folderId(item)) {
                if (item->isExpanded())
                    collapsedFolders_.remove(folder->value());
                else
                    collapsedFolders_.insert(folder->value());
            } else if (item->data(0, assets::kCompositionRootRole).toBool())
                compositionsCollapsed_ = !item->isExpanded();
        }
        refreshDisclosure(item);
    };
    connect(tree_, &QTreeWidget::itemExpanded, this, expanded);
    connect(tree_, &QTreeWidget::itemCollapsed, this, expanded);

    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetsEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &AssetsEditor::updateSelection);

    if (auto* controller = session_.assetController())
        connect(controller, &AssetController::changed, this, &AssetsEditor::rebuild);
    buildHeaderMenus();
    buildFooter();
    rebuild();
}

void AssetsEditor::setFilter(const int filter) {
    filterMode_ = filter;
    applyFilter(search_->text());
}

void AssetsEditor::openComposition(const document::CompositionId id) {
    if (id.isValid()) {
        (void)session_.setComposition(id);
    }
}

void AssetsEditor::beginRename(QTreeWidgetItem* item) {
    static_cast<assets::AssetTree*>(tree_)->beginRename(item);
}
void AssetsEditor::commitRename(QTreeWidgetItem* item, int column) {
    if (rebuilding_ || !item || column != 0)
        return;
    const auto composition = assets::compositionId(item);
    const auto asset = assets::assetId(item);
    const auto folder = assets::folderId(item);
    const auto name = item->text(0).toStdString();
    // Let the delegate finish before the snapshot signal replaces its item.
    QTimer::singleShot(0, this,
                       [this, composition, asset, folder, name, snapshot = session_.snapshot()] {
                           if (&snapshot.project() != &session_.snapshot().project()) {
                               rebuild();
                               return;
                           }
                           commands::Transaction transaction("Rename Asset", snapshot.revision());
                           if (asset.isValid())
                               transaction.emplace<commands::RenameAsset>(asset, name);
                           else if (folder)
                               transaction.emplace<commands::RenameAssetFolder>(*folder, name);
                           else if (composition.isValid())
                               transaction.emplace<commands::SetCompositionName>(composition, name);
                           else
                               return;
                           (void)session_.executeTransaction(std::move(transaction));
                           rebuild();
                       });
}

void AssetsEditor::showContextMenu(const QPoint position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    if (!item->isSelected())
        tree_->setCurrentItem(item);
    const auto assetId = document::AssetId::fromRaw(item->data(0, Qt::UserRole + 2).toULongLong());
    if (assetId.isValid()) {
        std::unique_ptr<QMenu> menu(kit::makeMenu(this));
        auto* rename = menu->addAction(tr("Rename"));
        rename->setObjectName(QStringLiteral("assetsRenameAction"));
        connect(rename, &QAction::triggered, this, [this] { beginRename(tree_->currentItem()); });
        auto* tags = menu->addAction(tr("Edit Tags…"));
        tags->setObjectName(QStringLiteral("assetsEditTagsAction"));
        connect(tags, &QAction::triggered, this, [this, assetId] { editTags(assetId); });
        auto* relink = menu->addAction(tr("Relink…"));
        relink->setObjectName(QStringLiteral("assetsRelinkAction"));
        auto* remove = menu->addAction(tr("Remove"));
        remove->setObjectName(QStringLiteral("assetsRemoveAction"));
        connect(relink, &QAction::triggered, this, [this, assetId] {
            if (auto* controller = session_.assetController())
                controller->relink(assetId, this);
        });
        connect(remove, &QAction::triggered, this, [this, assetId] {
            if (auto* controller = session_.assetController())
                controller->remove(assetId);
        });
        menu->exec(tree_->viewport()->mapToGlobal(position));
        return;
    }
    if (assets::folderId(item)) {
        std::unique_ptr<QMenu> menu(kit::makeMenu(this));
        auto* rename = menu->addAction(tr("Rename"));
        rename->setObjectName(QStringLiteral("assetsRenameAction"));
        connect(rename, &QAction::triggered, this, [this] { beginRename(tree_->currentItem()); });
        auto* create = menu->addAction(tr("New Folder"));
        connect(create, &QAction::triggered, this, &AssetsEditor::createFolder);
        auto* remove = menu->addAction(tr("Remove Folder"));
        remove->setObjectName(QStringLiteral("assetsRemoveFolderAction"));
        connect(remove, &QAction::triggered, this, &AssetsEditor::removeSelected);
        menu->exec(tree_->viewport()->mapToGlobal(position));
        return;
    }
    const auto id = assets::compositionId(item);
    if (!id.isValid())
        return;

    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    auto* openAction = menu->addAction(tr("Open"));
    openAction->setObjectName(QStringLiteral("assetsOpenAction"));
    connect(openAction, &QAction::triggered, this, [this, id] { openComposition(id); });
    auto* renameAction = menu->addAction(tr("Rename"));
    renameAction->setObjectName(QStringLiteral("assetsRenameAction"));
    connect(renameAction, &QAction::triggered, this, [this] { beginRename(tree_->currentItem()); });
    auto* duplicateAction = menu->addAction(tr("Duplicate"));
    duplicateAction->setObjectName(QStringLiteral("assetsDuplicateAction"));
    connect(duplicateAction, &QAction::triggered, this, [this, id] { duplicateComposition(id); });
    auto* deleteAction = menu->addAction(tr("Delete"));
    deleteAction->setObjectName(QStringLiteral("assetsDeleteAction"));
    connect(deleteAction, &QAction::triggered, this, [this, id] { deleteComposition(id); });
    menu->exec(tree_->viewport()->mapToGlobal(position));
}

void AssetsEditor::duplicateComposition(const document::CompositionId id) {
    if (const auto copyId = bloom::ui::duplicateComposition(session_, id); copyId.has_value()) {
        openComposition(*copyId);
    }
}

void AssetsEditor::deleteComposition(const document::CompositionId id) {
    (void)bloom::ui::deleteComposition(session_, id);
}

void AssetsEditor::showNewCompositionDialog() {
    if (const auto id = bloom::ui::showNewCompositionDialog(session_, this); id.has_value()) {
        openComposition(*id);
    }
}

void AssetsEditor::buildHeaderMenus() {
    auto* bar = &chrome_.header;
    bar->owner = this;
    bar->objectName = "assetsHeaderMenuBar";
    bar->overflowButtonName = "assetsHeaderOverflowButton";
    bar->overflowMenuName = "assetsHeaderOverflowMenu";

    auto* view = bar->addMenu(tr("View"));
    view->setObjectName(QStringLiteral("assetsViewMenu"));
    auto* expand = view->addAction(tr("Expand All"));
    expand->setObjectName(QStringLiteral("assetsExpandAllAction"));
    connect(expand, &QAction::triggered, tree_, &QTreeWidget::expandAll);
    auto* collapse = view->addAction(tr("Collapse All"));
    collapse->setObjectName(QStringLiteral("assetsCollapseAllAction"));
    connect(collapse, &QAction::triggered, tree_, &QTreeWidget::collapseAll);

    auto* add = bar->addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("assetsAddMenu"));
    auto* newComposition = add->addAction(tr("New Composition…"));
    newComposition->setObjectName(QStringLiteral("assetsNewCompositionAction"));
    connect(newComposition, &QAction::triggered, this, &AssetsEditor::showNewCompositionDialog);
    auto* newFolder = add->addAction(tr("New Folder"));
    newFolder->setObjectName(QStringLiteral("assetsNewFolderAction"));
    connect(newFolder, &QAction::triggered, this, &AssetsEditor::createFolder);
    newFolder->setToolTip(tr("New Folder"));

    auto* select = bar->addMenu(tr("Select"));
    select->setObjectName(QStringLiteral("assetsSelectMenu"));
    auto* selectAll = select->addAction(tr("All"));
    selectAll->setObjectName(QStringLiteral("assetsSelectAllAction"));
    connect(selectAll, &QAction::triggered, tree_, &QTreeWidget::selectAll);
    auto* selectNone = select->addAction(tr("None"));
    selectNone->setObjectName(QStringLiteral("assetsSelectNoneAction"));
    connect(selectNone, &QAction::triggered, tree_, &QTreeWidget::clearSelection);

    headerMenuWidget_ = EditorArea::buildChromeRow(chrome_.header, this);
}

void AssetsEditor::buildFooter() {
    auto* layout = &chrome_.footer;
    layout->objectName = "assetsFooter";
    const auto button = [&](kit::IconId icon, const QString& tooltip, const QString& name) {
        auto* value = new kit::KIconButton(this);
        value->setObjectName(name);
        value->setToolTip(tooltip);
        value->setAccessibleName(tooltip);
        value->setIcon(kit::icon(icon, kit::IconRole::Chrome));
        layout->addWidget(value);
        return value;
    };
    auto* newComposition = button(kit::IconId::Composition, tr("New Composition"),
                                  QStringLiteral("assetsNewCompositionButton"));
    connect(newComposition, &kit::KIconButton::clicked, this,
            &AssetsEditor::showNewCompositionDialog);
    auto* newFolder =
        button(kit::IconId::NewFolder, tr("New Folder"), QStringLiteral("assetsNewFolderButton"));
    connect(newFolder, &kit::KIconButton::clicked, this, &AssetsEditor::createFolder);
    auto* import = button(kit::IconId::Import, tr("Import"), QStringLiteral("assetsImportButton"));
    connect(import, &kit::KIconButton::clicked, this, [this] {
        if (auto* controller = session_.assetController())
            controller->requestImport(this);
    });
    layout->addStretch(1);
    auto* remove =
        button(kit::IconId::DeleteAsset, tr("Delete"), QStringLiteral("assetsDeleteButton"));
    connect(remove, &kit::KIconButton::clicked, this, &AssetsEditor::removeSelected);
    footerWidget_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
}

} // namespace bloom::ui
