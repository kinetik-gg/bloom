#include "assets_editor_internal.hpp"
#include <QAbstractItemDelegate>
#include <QAction>
#include <QHeaderView>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
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

    auto* tree = new AssetTree(session_, this);
    tree_ = tree;
    tree_->setObjectName(QStringLiteral("assetsTree"));
    tree_->setAccessibleName(tr("Assets"));
    columns_ = new ColumnController(*tree_);
    tree_->setRootIsDecorated(true);
    // One indent per level, which is also the caret column (ui-grammar.md, Assets).
    tree_->setIndentation(kit::px(kit::Size::AssetIndent));
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setAllColumnsShowFocus(true);
    // AssetTree::drawRow alternates the rows across the whole viewport itself.
    tree_->setAlternatingRowColors(false);
    tree_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree_->viewport()->setMouseTracking(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSortingEnabled(true);
    columns_->restore();

    summary_ = new SummaryStrip(session_, *tree_, this);
    layout->addWidget(summary_);
    searchBar_ = new SearchBar(this);
    search_ = searchBar_->field();
    layout->addWidget(searchBar_);
    layout->addWidget(tree_, 1);
    setInfoVisible(QSettings().value(QString::fromLatin1(kShowInfoKey), true).toBool());

    auto* header = tree_->header();
    connect(
        header, &QHeaderView::customContextMenuRequested, this,
        [this, header](const QPoint position) { showColumnMenu(header->mapToGlobal(position)); });
    connect(search_, &QLineEdit::textChanged, this, &AssetsEditor::applyFilter);
    searchBar_->closed = [this] { tree_->setFocus(Qt::OtherFocusReason); };
    searchBar_->visibilityChanged = [this](const bool open) {
        if (searchButton_ != nullptr)
            searchButton_->setChecked(open);
    };
    // Ctrl+F (Cmd+F on macOS) while the panel has focus opens the search row. Typing in the tree
    // itself stays the tree's own type-ahead to an item.
    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setObjectName(QStringLiteral("assetsFindShortcut"));
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, &AssetsEditor::openSearch);
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
        // A folder's glyph and the strip's folder glyph follow its open state.
        refreshSelectionHeader();
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

AssetsEditor::~AssetsEditor() {
    if (columns_ != nullptr)
        columns_->save();
}

void AssetsEditor::refreshSelectionHeader() {
    if (summary_ != nullptr && tree_ != nullptr && !summary_->isHidden())
        summary_->refresh(tree_->selectedItems());
}

void AssetsEditor::setInfoVisible(const bool visible) {
    summary_->setVisible(visible);
    QSettings().setValue(QString::fromLatin1(kShowInfoKey), visible);
    refreshSelectionHeader();
}

void AssetsEditor::openSearch() {
    searchBar_->open();
}

void AssetsEditor::showColumnMenu(const QPoint globalPosition) {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("assetsHeaderColumnMenu"));
    columns_->fill(*menu);
    menu->exec(globalPosition);
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
    auto* composition = menu.addAction(kit::icon(kit::IconId::Composition, kit::IconRole::Chrome),
                                       tr("New Composition…"));
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
            (void)bloom::ui::showCompositionSettingsDialog(session_, composition, *this);
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
    if (const auto id = bloom::ui::showNewCompositionDialog(session_, *this, selectedFolder());
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

    // Narrow, the menus fold one at a time into the overflow, Select first and View last, so the
    // Search toggle keeps its place at the trailing end beside Maximize.
    bar->overflowIcon = kit::IconId::ContextMenu;
    bar->overflowToolTip = tr("More panel menus");
    auto* view = bar->addMenu(tr("View"));
    view->setObjectName(QStringLiteral("assetsViewMenu"));
    bar->setCollapse(bar->entries.back().control, 3, view->menuAction());
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
    connect(columnMenu_, &QMenu::aboutToShow, this, [this] { columns_->fill(*columnMenu_); });
    columns_->fill(*columnMenu_);
    auto* showInfo = view->addAction(tr("Show Info"));
    showInfo->setObjectName(QStringLiteral("assetsShowInfoAction"));
    showInfo->setCheckable(true);
    showInfo->setChecked(!summary_->isHidden());
    showInfo->setToolTip(tr("Show the summary of the selection above the list"));
    connect(showInfo, &QAction::toggled, this, &AssetsEditor::setInfoVisible);
    auto* findAction = view->addAction(tr("Search"));
    findAction->setObjectName(QStringLiteral("assetsSearchAction"));
    findAction->setShortcut(QKeySequence::Find);
    // The shortcut text is shown; the panel's own QShortcut handles the key, so it is panel-scoped.
    findAction->setShortcutContext(Qt::WidgetShortcut);
    connect(findAction, &QAction::triggered, this, &AssetsEditor::openSearch);

    auto* add = bar->addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("assetsAddMenu"));
    bar->setCollapse(bar->entries.back().control, 2, add->menuAction());
    auto* newComposition = add->addAction(
        kit::icon(kit::IconId::Composition, kit::IconRole::Chrome), tr("New Composition…"));
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
    bar->setCollapse(bar->entries.back().control, 1, select->menuAction());
    auto* selectAll = select->addAction(tr("All"));
    selectAll->setObjectName(QStringLiteral("assetsSelectAllAction"));
    connect(selectAll, &QAction::triggered, tree_, &QTreeWidget::selectAll);
    auto* selectNone = select->addAction(tr("None"));
    selectNone->setObjectName(QStringLiteral("assetsSelectNoneAction"));
    connect(selectNone, &QAction::triggered, tree_, &QTreeWidget::clearSelection);

    // Search, collapsed to one toggle at the header's trailing end beside Maximize.
    // A header icon button (one Control height, like Maximize beside it), Selected while open.
    searchButton_ = new kit::KIconButton(this);
    searchButton_->setObjectName(QStringLiteral("assetsSearchButton"));
    searchButton_->setIcon(kit::icon(kit::IconId::Search, kit::IconRole::Chrome));
    searchButton_->setCheckable(true);
    searchButton_->setToolTip(
        tr("Search (%1)").arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    searchButton_->setAccessibleName(tr("Search assets"));
    connect(searchButton_, &kit::KIconButton::clicked, this, [this] {
        // A live filter never closes from here: the toggle focuses it instead.
        if (searchBar_->isHidden() || !search_->text().isEmpty())
            searchBar_->open();
        else
            searchBar_->close();
        searchButton_->setChecked(!searchBar_->isHidden());
    });
    bar->addStretch();
    bar->addWidget(searchButton_);

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
    auto* newComposition =
        button(kit::IconId::Composition,
               tr("New Composition (%1)")
                   .arg(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N)
                            .toString(QKeySequence::NativeText)),
               QStringLiteral("assetsNewCompositionButton"));
    newComposition->setAccessibleName(tr("New Composition"));
    connect(newComposition, &kit::KIconButton::clicked, this,
            &AssetsEditor::showNewCompositionDialog);
    auto* newFolder =
        button(kit::IconId::NewFolder, tr("New Folder"), QStringLiteral("assetsNewFolderButton"));
    connect(newFolder, &kit::KIconButton::clicked, this, &AssetsEditor::createFolder);
    auto* import = button(kit::IconId::Import, tr("Import"), QStringLiteral("assetsImportButton"));
    connect(import, &kit::KIconButton::clicked, this, &AssetsEditor::requestImport);
    layout->addStretch(1);
    auto* remove = button(kit::IconId::DeleteAsset,
                          tr("Delete (%1)").arg(QKeySequence(Qt::Key_Delete)
                                                    .toString(QKeySequence::NativeText)),
                          QStringLiteral("assetsDeleteButton"));
    remove->setAccessibleName(tr("Delete"));
    connect(remove, &kit::KIconButton::clicked, this, &AssetsEditor::removeSelected);
    footerWidget_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
}

} // namespace bloom::ui
