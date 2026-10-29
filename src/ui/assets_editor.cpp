#include "assets_editor_internal.hpp"
#include <QAbstractItemDelegate>
#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <memory>

namespace bloom::ui {
using namespace assets;

AssetsEditor::AssetsEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("assetsEditor"));
    setAccessibleName(tr("Project assets editor"));

    auto* layout = new QVBoxLayout(this);
    const int gutter = kit::px(kit::Spacing::S);
    layout->setContentsMargins(gutter, gutter, gutter, gutter);
    layout->setSpacing(kit::px(kit::Spacing::Gutter));

    buildSelectionHeader();
    layout->addWidget(selectionHeader_);

    search_ = new kit::KSearchField(this);
    search_->setObjectName(QStringLiteral("assetsSearchField"));
    search_->setAccessibleName(tr("Search assets"));
    search_->setPlaceholderText(tr("Search names and tags…"));
    search_->setToolTip(tr("Matches names and tags; tag:name matches tags only"));
    layout->addWidget(search_);

    auto* tree = new AssetTree(session_, this);
    tree_ = tree;
    tree_->setObjectName(QStringLiteral("assetsTree"));
    tree_->setAccessibleName(tr("Assets"));
    tree_->setColumnCount(ColumnCount);
    tree_->setHeaderLabels({tr("Name"), tr("Type"), tr("Size"), tr("Duration"), tr("Frame Rate"),
                            tr("Tags"), tr("File Location")});
    tree_->setRootIsDecorated(true);
    tree_->setIndentation(kit::px(kit::Size::ToggleCell));
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setAllColumnsShowFocus(true);
    tree_->setAlternatingRowColors(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSortingEnabled(true);
    auto* header = tree_->header();
    header->setObjectName(QStringLiteral("assetsTreeHeader"));
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(false);
    header->setStretchLastSection(false);
    header->setHighlightSections(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setToolTip(
        tr("Click to sort, drag to resize or reorder; right-click to choose columns"));
    const int column = kit::px(kit::Size::DropdownWidth);
    tree_->setColumnWidth(NameColumn, column * 2);
    tree_->setColumnWidth(LocationColumn, column * 2);
    // Duration carries the frame count beside the timecode (#380).
    tree_->setColumnWidth(DurationColumn, column * 2);
    for (const auto index : {TypeColumn, SizeColumn, FrameRateColumn, TagsColumn})
        tree_->setColumnWidth(index, column);
    header->setSectionHidden(TagsColumn, true);
    tree_->sortByColumn(NameColumn, Qt::AscendingOrder);
    restoreHeaderState();
    connect(
        header, &QHeaderView::customContextMenuRequested, this,
        [this, header](const QPoint position) { showColumnMenu(header->mapToGlobal(position)); });
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this] { saveHeaderState(); });
    connect(header, &QHeaderView::sectionMoved, this, [this] { saveHeaderState(); });
    layout->addWidget(tree_, 1);

    connect(search_, &QLineEdit::textChanged, this, &AssetsEditor::applyFilter);
    // Double-click opens a composition only, as in After Effects; Composition Settings is the
    // context menu, the Composition menu and Ctrl+Alt+K (#376). Folders expand.
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { openComposition(compositionId(item)); });
    // A tag cell filters by its first tag, as the tag chips did.
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, const int index) {
        const auto tags = item ? item->data(0, kTagsRole).toStringList() : QStringList{};
        if (index == TagsColumn && !tags.isEmpty())
            search_->setText(QStringLiteral("tag:") + tags.front());
    });
    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, const int index) { commitRename(item, index); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint position) { showContextMenu(position); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &AssetsEditor::selectionChanged);
    connect(tree_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
            [this] { QTimer::singleShot(0, this, &AssetsEditor::rebuild); });
    tree->deleteRequested = [this] { removeSelected(); };
    const auto expanded = [this](QTreeWidgetItem* item) {
        if (!search_->text().trimmed().isEmpty() || rebuilding_)
            return;
        if (const auto folder = folderId(item)) {
            if (item->isExpanded())
                collapsedFolders_.remove(folder->value());
            else
                collapsedFolders_.insert(folder->value());
        } else if (isData(item) && item->parent() == nullptr)
            dataCollapsed_ = !item->isExpanded();
    };
    connect(tree_, &QTreeWidget::itemExpanded, this, expanded);
    connect(tree_, &QTreeWidget::itemCollapsed, this, expanded);

    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        refreshRows();
        refreshSelectionHeader();
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        updateSelection();
        refreshRows();
        refreshSelectionHeader();
    });
    if (auto* controller = session_.assetController())
        connect(controller, &AssetController::changed, this, [this] {
            refreshRows();
            refreshSelectionHeader();
        });
    if (auto* thumbnails = session_.compositionThumbnails())
        connect(thumbnails, &CompositionThumbnailController::changed, this,
                &AssetsEditor::refreshSelectionHeader);
    buildHeaderMenus();
    buildFooter();
    rebuild();
}

AssetsEditor::~AssetsEditor() { saveHeaderState(); }

void AssetsEditor::saveHeaderState() const {
    if (tree_ != nullptr)
        QSettings().setValue(QString::fromLatin1(kHeaderStateKey), tree_->header()->saveState());
}

void AssetsEditor::restoreHeaderState() {
    const auto state = QSettings().value(QString::fromLatin1(kHeaderStateKey)).toByteArray();
    if (!state.isEmpty() && tree_->header()->restoreState(state)) {
        // Name is never hidden, whatever an older state says.
        tree_->header()->setSectionHidden(NameColumn, false);
        tree_->setSortingEnabled(true);
    }
}

void AssetsEditor::fillColumnMenu(QMenu& menu) {
    menu.clear();
    for (int index = TypeColumn; index < ColumnCount; ++index) {
        auto* action = menu.addAction(tree_->headerItem()->text(index));
        action->setObjectName(QStringLiteral("assetsColumnAction%1").arg(index));
        action->setCheckable(true);
        action->setChecked(!tree_->header()->isSectionHidden(index));
        connect(action, &QAction::toggled, this,
                [this, index](const bool visible) { setColumnVisible(index, visible); });
    }
}

void AssetsEditor::showColumnMenu(const QPoint globalPosition) {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("assetsHeaderColumnMenu"));
    fillColumnMenu(*menu);
    menu->exec(globalPosition);
}

void AssetsEditor::setColumnVisible(const int column, const bool visible) {
    if (column <= NameColumn || column >= ColumnCount)
        return;
    tree_->header()->setSectionHidden(column, !visible);
    if (visible && tree_->columnWidth(column) == 0)
        tree_->setColumnWidth(column, kit::px(kit::Size::DropdownWidth));
    saveHeaderState();
}

void AssetsEditor::openComposition(const document::CompositionId id) {
    if (id.isValid())
        (void)session_.setComposition(id);
}

void AssetsEditor::beginRename(QTreeWidgetItem* item) {
    static_cast<AssetTree*>(tree_)->beginRename(item);
}

void AssetsEditor::commitRename(QTreeWidgetItem* item, const int column) {
    if (rebuilding_ || !item || column != NameColumn)
        return;
    const auto composition = compositionId(item);
    const auto asset = assetId(item);
    const auto folder = folderId(item);
    const auto name = item->text(NameColumn).toStdString();
    // Let the delegate finish before the snapshot signal replaces its item.
    QTimer::singleShot(0, this,
                       [this, composition, asset, folder, name, snapshot = session_.snapshot()] {
                           if (&snapshot.project() != &session_.snapshot().project()) {
                               rebuild();
                               return;
                           }
                           commands::Transaction transaction("Rename", snapshot.revision());
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

void AssetsEditor::addCommonActions(QMenu& menu) {
    auto* folder = menu.addAction(tr("New Folder"));
    folder->setObjectName(QStringLiteral("assetsContextNewFolderAction"));
    connect(folder, &QAction::triggered, this, &AssetsEditor::createFolder);
    auto* composition = menu.addAction(tr("New Composition…"));
    composition->setObjectName(QStringLiteral("assetsContextNewCompositionAction"));
    connect(composition, &QAction::triggered, this, &AssetsEditor::showNewCompositionDialog);
    auto* import = menu.addAction(tr("Import…"));
    import->setObjectName(QStringLiteral("assetsContextImportAction"));
    connect(import, &QAction::triggered, this, &AssetsEditor::requestImport);
}

void AssetsEditor::showContextMenu(const QPoint position) {
    auto* item = tree_->itemAt(position);
    // As in After Effects, a right-click acts on the clicked row; empty space on the root.
    if (item == nullptr)
        tree_->clearSelection();
    else if (!item->isSelected()) {
        tree_->clearSelection();
        tree_->setCurrentItem(item);
    }
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("assetsContextMenu"));
    const auto add = [&](const QString& text, const char* name, auto&& slot) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    const bool single = tree_->selectedItems().size() == 1;
    const auto rename = [&] {
        add(tr("Rename"), "assetsRenameAction", [this] {
            beginRename(tree_->currentItem());
        })->setEnabled(single);
    };
    const auto remove = [&] {
        add(tr("Delete"), "assetsDeleteAction", [this] { removeSelected(); });
    };
    const auto& project = session_.snapshot().project();
    if (isData(item))
        return;
    if (const auto id = assetId(item); id.isValid()) {
        const auto* asset = project.findAsset(id);
        const auto layerable = [](const document::AssetRecord& record) {
            return record.kind == document::AssetKind::Image ||
                   record.kind == document::AssetKind::Sequence ||
                   record.kind == document::AssetKind::Video ||
                   record.kind == document::AssetKind::Audio;
        };
        const auto selected = selectedAssets();
        add(tr("New Composition from Selection"), "assetsNewCompositionFromSelectionAction",
            [this] { newCompositionFromSelection(); })
            ->setEnabled(std::ranges::any_of(selected, [&](const auto asset) {
                const auto* record = project.findAsset(asset);
                return record && layerable(*record);
            }));
        menu->addSeparator();
        if (asset && (asset->kind == document::AssetKind::Image ||
                      asset->kind == document::AssetKind::Sequence ||
                      asset->kind == document::AssetKind::Video))
            add(tr("Input Colour…"), "assetsInputColorAction", [this, id] {
                editInputColor(id);
            })->setToolTip(tr("Interpret the footage's colour"));
        add(tr("Replace Footage…"), "assetsReplaceFootageAction", [this, id] {
            if (auto* controller = session_.assetController())
                controller->relink(id, this);
        })->setEnabled(single);
        auto* controller = session_.assetController();
        const bool font = asset && asset->kind == document::AssetKind::Font;
        add(tr("Reload Footage"), "assetsReloadAction", [this, selected] {
            if (auto* assets = session_.assetController())
                for (const auto asset : selected)
                    assets->reload(asset);
        })->setEnabled(controller != nullptr && !font);
        add(tr("Reveal in File Manager"), "assetsRevealAction", [this, id] {
            revealInFileManager(id);
        })->setEnabled(controller != nullptr && !controller->sourcePath(id).empty());
        menu->addSeparator();
        rename();
        add(tr("Edit Tags…"), "assetsEditTagsAction", [this, id] { editTags(id); });
        remove();
        menu->addSeparator();
    } else if (const auto composition = compositionId(item); composition.isValid()) {
        add(tr("Open"), "assetsOpenAction", [this, composition] { openComposition(composition); });
        add(tr("Composition Settings…"), "assetsCompositionSettingsAction", [this, composition] {
            (void)bloom::ui::showCompositionSettingsDialog(session_, composition, this);
        });
        menu->addSeparator();
        rename();
        add(tr("Duplicate"), "assetsDuplicateAction",
            [this, composition] { duplicateComposition(composition); });
        remove();
        menu->addSeparator();
    } else if (folderId(item)) {
        rename();
        remove();
        menu->addSeparator();
    }
    addCommonActions(*menu);
    menu->exec(tree_->viewport()->mapToGlobal(position));
}

void AssetsEditor::duplicateComposition(const document::CompositionId id) {
    if (const auto copyId = bloom::ui::duplicateComposition(session_, id); copyId.has_value())
        openComposition(*copyId);
}

void AssetsEditor::showNewCompositionDialog() {
    if (const auto id = bloom::ui::showNewCompositionDialog(session_, this, selectedFolder());
        id.has_value())
        openComposition(*id);
}

void AssetsEditor::requestImport() {
    if (auto* controller = session_.assetController()) {
        controller->setImportFolder(selectedFolder());
        controller->requestImport(this);
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
    connect(expand, &QAction::triggered, this, [this] {
        collapsedFolders_.clear();
        dataCollapsed_ = false;
        tree_->expandAll();
    });
    auto* collapse = view->addAction(tr("Collapse All"));
    collapse->setObjectName(QStringLiteral("assetsCollapseAllAction"));
    connect(collapse, &QAction::triggered, this, [this] {
        // Remembered like a folder collapsed by hand, so a rebuild keeps them closed.
        for (const auto& folder : session_.snapshot().project().assetFolders())
            collapsedFolders_.insert(folder.id.value());
        dataCollapsed_ = true;
        tree_->collapseAll();
    });
    view->addSeparator();
    columnMenu_ = kit::makeMenu(tr("Columns"), view);
    columnMenu_->setObjectName(QStringLiteral("assetsColumnMenu"));
    view->addMenu(columnMenu_);
    connect(columnMenu_, &QMenu::aboutToShow, this, [this] { fillColumnMenu(*columnMenu_); });
    fillColumnMenu(*columnMenu_);

    auto* add = bar->addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("assetsAddMenu"));
    auto* newComposition = add->addAction(tr("New Composition…"));
    newComposition->setObjectName(QStringLiteral("assetsNewCompositionAction"));
    connect(newComposition, &QAction::triggered, this, &AssetsEditor::showNewCompositionDialog);
    auto* newFolder = add->addAction(tr("New Folder"));
    newFolder->setObjectName(QStringLiteral("assetsNewFolderAction"));
    newFolder->setToolTip(tr("New Folder"));
    connect(newFolder, &QAction::triggered, this, &AssetsEditor::createFolder);
    auto* import = add->addAction(tr("Import…"));
    import->setObjectName(QStringLiteral("assetsImportAction"));
    connect(import, &QAction::triggered, this, &AssetsEditor::requestImport);

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
    connect(import, &kit::KIconButton::clicked, this, &AssetsEditor::requestImport);
    layout->addStretch(1);
    auto* remove =
        button(kit::IconId::DeleteAsset, tr("Delete"), QStringLiteral("assetsDeleteButton"));
    connect(remove, &kit::KIconButton::clicked, this, &AssetsEditor::removeSelected);
    footerWidget_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
}

} // namespace bloom::ui
