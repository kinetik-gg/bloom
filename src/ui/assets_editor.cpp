#include "asset_drop.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
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

#include <cstdint>
#include <optional>

namespace bloom::ui {
namespace {

constexpr int kCompositionIdRole = Qt::UserRole + 1;

[[nodiscard]] document::CompositionId compositionIdForItem(const QTreeWidgetItem* item) {
    if (item == nullptr) {
        return {};
    }
    return document::CompositionId::fromRaw(item->data(0, kCompositionIdRole).toULongLong());
}

class AssetTree final : public QTreeWidget {
  public:
    AssetTree(CompositionSession& session, QWidget* parent)
        : QTreeWidget(parent), session_(session) {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragDrop);
    }

  protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kAssetMimeType)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        auto* mime = new QMimeData;
        for (const auto* item : items) {
            const auto id = item->data(0, Qt::UserRole + 2).toULongLong();
            if (id) {
                mime->setData(kAssetMimeType,
                              assetMimePayload(session_, document::AssetId::fromRaw(id)));
                break;
            }
        }
        return mime;
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dropEvent(QDropEvent* event) override {
        QStringList files;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile())
                files.push_back(url.toLocalFile());
        if (auto* controller = session_.assetController(); controller && !files.empty()) {
            controller->importFiles(files);
            event->acceptProposedAction();
        }
    }

  private:
    CompositionSession& session_;
};

} // namespace

AssetsEditor::AssetsEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("assetsEditor"));
    setAccessibleName(tr("Project assets editor"));

    auto* layout = new QVBoxLayout(this);
    const int gutter = kit::px(kit::Spacing::S);
    layout->setContentsMargins(gutter, gutter, gutter, gutter);
    layout->setSpacing(kit::px(kit::Spacing::Gutter));

    search_ = new kit::KSearchField(this);
    search_->setObjectName(QStringLiteral("assetsSearchField"));
    search_->setAccessibleName(tr("Search assets"));
    search_->setPlaceholderText(tr("Search assets…"));
    layout->addWidget(search_);

    tree_ = new AssetTree(session_, this);
    tree_->setObjectName(QStringLiteral("assetsTree"));
    tree_->setProperty("kitRows", true);
    tree_->setAccessibleName(tr("Assets"));
    tree_->setHeaderLabels({tr("Name"), tr("Kind")});
    tree_->setRootIsDecorated(false);
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
                    openComposition(compositionIdForItem(item));
                }
            });
    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, const int column) { commitRename(item, column); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint position) { showContextMenu(position); });

    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetsEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &AssetsEditor::updateSelection);

    if (auto* controller = session_.assetController())
        connect(controller, &AssetController::changed, this, &AssetsEditor::rebuild);
    buildHeaderMenus();
    buildFooter();
    rebuild();
}

void AssetsEditor::rebuild() {
    rebuilding_ = true;
    const auto selectedAsset =
        tree_->currentItem() ? tree_->currentItem()->data(0, Qt::UserRole + 2).toULongLong() : 0;
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(composition.name()));
        item->setText(1, tr("Composition"));
        item->setData(0, kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(0, tr("Composition %1").arg(composition.id().value()));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        auto* row = new kit::KRow(tree_);
        row->setObjectName(QStringLiteral("assetsRow"));
        row->setName(item->text(0), kit::IconId::Composition);
        auto* kind = new kit::KLabel(row);
        kind->setElidedText(item->text(1));
        row->setCells({}, nullptr, {kind});
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        item->setSizeHint(0, QSize(0, kit::px(kit::Size::ListRow)));
        item->setFirstColumnSpanned(true);
        tree_->setItemWidget(item, 0, row);
    }
    for (const auto& asset : session_.snapshot().project().assets()) {
        auto* item = new QTreeWidgetItem(tree_);
        const auto name = asset.kind == document::AssetKind::Sequence
                              ? asset.manifest.pattern
                              : asset.locator.path.substr(asset.locator.path.find_last_of('/') + 1);
        item->setText(0, QString::fromStdString(name));
        item->setText(1, asset.kind == document::AssetKind::Sequence
                             ? tr("Sequence [%1]").arg(asset.manifest.members.size())
                             : tr("Image"));
        item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qulonglong>(asset.id.value()));
        const auto* controller = session_.assetController();
        const bool missing = controller && controller->missing(asset.id);
        item->setToolTip(0, missing ? tr("Missing image — Relink in Assets")
                                    : QString::fromStdString(asset.locator.path));
        auto* row = new kit::KRow(tree_);
        row->setObjectName(QStringLiteral("assetsRow"));
        row->setName(item->text(0), asset.kind == document::AssetKind::Sequence
                                        ? kit::IconId::Images
                                        : kit::IconId::Image);
        auto* kind = new kit::KLabel(row);
        kind->setElidedText(item->text(1));
        auto* warning = new kit::KIconButton(row);
        warning->setObjectName(QStringLiteral("assetsMissingGlyph"));
        warning->setIcon(kit::icon(kit::IconId::Warning, kit::IconRole::Chrome, kit::Color::Warn));
        warning->setToolTip(tr("Missing image"));
        warning->setVisible(missing);
        row->setCells({}, nullptr, {kind}, warning);
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        item->setSizeHint(0, QSize(0, kit::px(kit::Size::ListRow)));
        item->setFirstColumnSpanned(true);
        tree_->setItemWidget(item, 0, row);
    }
    applyFilter(search_->text());
    updateSelection();
    if (selectedAsset)
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            auto* item = tree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole + 2).toULongLong() == selectedAsset) {
                tree_->clearSelection();
                tree_->setCurrentItem(item);
                item->setSelected(true);
                refreshRowSelection();
                break;
            }
        }
    rebuilding_ = false;
}

void AssetsEditor::updateSelection() {
    if (tree_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(tree_);
    tree_->clearSelection();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        if (!item->isHidden() && compositionIdForItem(item) == session_.compositionId()) {
            tree_->setCurrentItem(item);
            item->setSelected(true);
            refreshRowSelection();
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
    refreshRowSelection();
}

void AssetsEditor::refreshRowSelection() {
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        if (auto* row = qobject_cast<kit::KRow*>(tree_->itemWidget(item, 0)))
            row->setRowState(index, item->isSelected());
    }
}

void AssetsEditor::applyFilter(const QString& text) {
    if (tree_ == nullptr) {
        return;
    }
    const QString query = text.trimmed();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        item->setHidden(!query.isEmpty() && !item->text(0).contains(query, Qt::CaseInsensitive));
    }
    if (!rebuilding_) {
        updateSelection();
    }
}

void AssetsEditor::openComposition(const document::CompositionId id) {
    if (id.isValid()) {
        (void)session_.setComposition(id);
    }
}

void AssetsEditor::commitRename(QTreeWidgetItem* item, const int column) {
    if (rebuilding_ || item == nullptr || column != 0) {
        return;
    }
    const auto id = compositionIdForItem(item);
    if (!id.isValid()) {
        return;
    }
    commands::Transaction transaction("Rename Composition", session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionName>(id, item->text(0).toStdString());
    const auto result = session_.executeTransaction(std::move(transaction));
    if (!result.succeeded()) {
        rebuild();
    }
}

void AssetsEditor::showContextMenu(const QPoint position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    tree_->setCurrentItem(item);
    const auto assetId = document::AssetId::fromRaw(item->data(0, Qt::UserRole + 2).toULongLong());
    if (assetId.isValid()) {
        std::unique_ptr<QMenu> menu(kit::makeMenu(this));
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
    const auto id = compositionIdForItem(item);

    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    auto* openAction = menu->addAction(tr("Open"));
    openAction->setObjectName(QStringLiteral("assetsOpenAction"));
    connect(openAction, &QAction::triggered, this, [this, id] { openComposition(id); });
    auto* renameAction = menu->addAction(tr("Rename"));
    renameAction->setObjectName(QStringLiteral("assetsRenameAction"));
    connect(renameAction, &QAction::triggered, this, [this, item] { tree_->editItem(item, 0); });
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
    newFolder->setEnabled(false);
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
    newFolder->setEnabled(false);
    auto* import = button(kit::IconId::Import, tr("Import"), QStringLiteral("assetsImportButton"));
    connect(import, &kit::KIconButton::clicked, this, [this] {
        if (auto* controller = session_.assetController())
            controller->requestImport(this);
    });
    layout->addStretch(1);
    auto* remove =
        button(kit::IconId::DeleteAsset, tr("Delete"), QStringLiteral("assetsDeleteButton"));
    connect(remove, &kit::KIconButton::clicked, this, [this] {
        const auto* item = tree_->currentItem();
        if (!item)
            return;
        const auto asset =
            document::AssetId::fromRaw(item->data(0, Qt::UserRole + 2).toULongLong());
        if (asset.isValid()) {
            if (auto* controller = session_.assetController())
                controller->remove(asset);
        } else
            deleteComposition(compositionIdForItem(item));
    });
    footerWidget_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
}

} // namespace bloom::ui
