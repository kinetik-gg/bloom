#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QStyledItemDelegate>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <tuple>
#include <unordered_set>

namespace bloom::ui::assets {
namespace {
class AssetNameDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* editor = new kit::KLineEdit(parent);
        editor->setObjectName(QStringLiteral("assetsRenameField"));
        editor->setAccessibleName(QObject::tr("Name"));
        return editor;
    }
};
} // namespace

AssetTree::AssetTree(CompositionSession& session, QWidget* parent)
    : QTreeWidget(parent), session_(session),
      token_(QUuid::createUuid().toByteArray(QUuid::Id128)) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            [this] { token_ = QUuid::createUuid().toByteArray(QUuid::Id128); });
    setStyleSheet(QStringLiteral("QTreeView::item:selected { background: %1; }")
                      .arg(kit::color(kit::Color::SurfaceRaised).name()));
    setItemDelegate(new AssetNameDelegate(this));
    setAcceptDrops(true);
    setDragEnabled(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
}
void AssetTree::beginRename(QTreeWidgetItem* item) {
    if (!item || !item->flags().testFlag(Qt::ItemIsEditable))
        return;
    removeItemWidget(item, 0);
    editItem(item, 0);
}
void AssetTree::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F2) {
        beginRename(currentItem());
        event->accept();
    } else
        QTreeWidget::keyPressEvent(event);
}
QStringList AssetTree::mimeTypes() const {
    return {QString::fromLatin1(kAssetMimeType), QString::fromLatin1(kInternalMimeType),
            QString::fromLatin1(kCompositionMimeType)};
}
QMimeData* AssetTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    auto* mime = new QMimeData;
    QByteArray ids;
    for (QTreeWidgetItemIterator it(const_cast<AssetTree*>(this)); *it; ++it) {
        if (!items.contains(*it))
            continue;
        const auto id = assetId(*it);
        if (!id.isValid())
            continue;
        if (ids.isEmpty())
            mime->setData(kAssetMimeType, assetMimePayload(session_, id));
        ids += ':' + QByteArray::number(static_cast<qulonglong>(id.value()));
    }
    if (!ids.isEmpty())
        mime->setData(kInternalMimeType, token_ + ':' +
                                             QByteArray::number(static_cast<qulonglong>(
                                                 session_.snapshot().revision().value())) +
                                             ids);
    else
        for (const auto* item : items)
            if (compositionId(item).isValid()) {
                mime->setData(kCompositionMimeType, QByteArray::number(static_cast<qulonglong>(
                                                        compositionId(item).value())));
                break;
            }
    return mime;
}
void AssetTree::startDrag(Qt::DropActions) {
    QDrag drag(this);
    drag.setMimeData(mimeData(selectedItems()));
    // The transaction rebuilds the projection. QTreeWidget's default Move cleanup must not
    // subsequently remove rows from that freshly rebuilt tree.
    (void)drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
}
std::vector<document::AssetId> AssetTree::internalAssets(const QMimeData& mime) const {
    const auto payload = mime.data(kInternalMimeType);
    if (payload.size() > 2200000)
        return {};
    const auto parts = payload.split(':');
    if (parts.size() < 3 || parts.front() != token_ ||
        parts[1] !=
            QByteArray::number(static_cast<qulonglong>(session_.snapshot().revision().value())))
        return {};
    std::vector<document::AssetId> ids;
    std::unordered_set<document::AssetId> unique;
    for (qsizetype index = 2; index < parts.size(); ++index) {
        bool ok = false;
        const auto raw = parts[index].toULongLong(&ok);
        const auto id = document::AssetId::fromRaw(raw);
        if (!ok || !session_.snapshot().project().findAsset(id) || !unique.insert(id).second)
            return {};
        ids.push_back(id);
    }
    return ids;
}
void AssetTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else if (!internalAssets(*event->mimeData()).empty()) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else
        event->ignore();
}
void AssetTree::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    const auto* item = itemAt(event->position().toPoint());
    if (internalAssets(*event->mimeData()).empty() || compositionId(item).isValid() ||
        (item && item->data(0, kCompositionRootRole).toBool())) {
        event->ignore();
        return;
    }
    QTreeWidget::dragMoveEvent(event);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}
void AssetTree::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        QStringList files;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile())
                files.push_back(url.toLocalFile());
        if (auto* controller = session_.assetController(); controller && !files.empty()) {
            controller->importFiles(files);
            event->acceptProposedAction();
        }
        return;
    }
    const auto ids = internalAssets(*event->mimeData());
    auto* target = itemAt(event->position().toPoint());
    if (ids.empty() || compositionId(target).isValid() ||
        (target && target->data(0, kCompositionRootRole).toBool())) {
        event->ignore();
        return;
    }
    const auto& project = session_.snapshot().project();
    auto folder = folderId(target);
    const auto* targetAsset = project.findAsset(assetId(target));
    const auto indicator = dropIndicatorPosition();
    if (targetAsset)
        folder = targetAsset->folder;
    else if (folder && (indicator == AboveItem || indicator == BelowItem))
        folder = project.findAssetFolder(*folder)->parent;
    std::vector<const document::AssetRecord*> siblings;
    for (const auto& asset : project.assets())
        if (asset.folder == folder)
            siblings.push_back(&asset);
    std::ranges::sort(siblings, [](const auto* left, const auto* right) {
        return std::tie(left->order, left->id) < std::tie(right->order, right->id);
    });
    const std::unordered_set<document::AssetId> moving(ids.begin(), ids.end());
    std::size_t index = 0;
    for (const auto* sibling : siblings) {
        if (targetAsset == sibling) {
            if (indicator == BelowItem && !moving.contains(sibling->id))
                ++index;
            break;
        }
        if (!moving.contains(sibling->id))
            ++index;
    }
    if (!targetAsset && target && (indicator == AboveItem || indicator == BelowItem))
        index = 0;
    commands::Transaction transaction("Move Assets", session_.snapshot().revision());
    transaction.emplace<commands::MoveAssets>(ids, folder, index);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else
        event->ignore();
}
} // namespace bloom::ui::assets
