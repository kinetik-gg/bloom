#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include "network_share_paths.hpp"
#include <QCollator>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QStyledItemDelegate>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <set>
#include <unordered_set>

namespace bloom::ui::assets {
namespace {
// True when at least one URL in `mime` is something a drop can actually act on: a local file, a
// resolvable `smb://` mount, or an `smb://` URL naming a share this machine has not mounted yet
// (dropEvent() turns that last case into a "Connect to <host>/<share>" notice rather than a
// silent no-op). Anything else -- another scheme entirely, or a malformed `smb://` URL with no
// share segment -- is not something this tree can do anything with, so the cursor must say so.
bool anyActionableUrl(const QMimeData& mime, const QString& gvfsRoot) {
    for (const auto& url : mime.urls())
        if (localPathForUrl(url, gvfsRoot) || smbShareLabel(url))
            return true;
    return false;
}
// Only the Name column renames, through the kit line edit.
class AssetNameDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        if (index.column() != NameColumn)
            return QStyledItemDelegate::createEditor(parent, option, index);
        auto* editor = new kit::KLineEdit(parent);
        editor->setObjectName(QStringLiteral("assetsRenameField"));
        editor->setAccessibleName(QObject::tr("Name"));
        return editor;
    }

  protected:
    // Escape confirms everywhere (owner, 2026-10-29): it commits the typed name exactly as Return
    // and clicking away do, instead of the stock delegate's revert. Undo is the discard.
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* editor = qobject_cast<QWidget*>(object);
        if (editor != nullptr && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            Q_EMIT commitData(editor);
            Q_EMIT closeEditor(editor, QAbstractItemDelegate::NoHint);
            return true;
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }
};
// Keys the tree owns while it has focus, so a window-level shortcut cannot take them.
bool treeKey(const QKeyEvent& event) {
    const int key = event.key();
    if (event.modifiers() == Qt::NoModifier || event.modifiers() == Qt::KeypadModifier)
        return key == Qt::Key_F2 || key == Qt::Key_Return || key == Qt::Key_Enter ||
               key == Qt::Key_Delete || key == Qt::Key_Backspace;
    return event.modifiers() == Qt::ControlModifier && key == Qt::Key_A;
}
} // namespace

Usage assetUsage(const document::Project& project, const document::AssetId id) {
    Usage usage;
    const auto text = std::to_string(id.value());
    for (const auto& composition : project.compositions()) {
        std::size_t uses = 0;
        for (const auto& node : composition.graph().nodes())
            for (const auto& binding : node.parameters) {
                const bool stringReference =
                    binding.role == "asset" || binding.role == document::kTextFontParameterRole;
                if (!stringReference && binding.role != "lut")
                    continue;
                const auto* parameter = composition.parameters().find(binding.parameterId);
                const auto* constant =
                    parameter ? std::get_if<document::ConstantValueSource>(&parameter->source)
                              : nullptr;
                if (constant == nullptr)
                    continue;
                if (const auto* value = std::get_if<std::string>(&constant->value);
                    value && stringReference && *value == text)
                    ++uses;
                else if (const auto* number = std::get_if<std::int64_t>(&constant->value);
                         number && !stringReference && *number > 0 &&
                         static_cast<std::uint64_t>(*number) == id.value())
                    ++uses;
            }
        usage.uses += uses;
        usage.compositions += uses > 0 ? 1 : 0;
    }
    return usage;
}

Usage compositionUsage(const document::Project& project, const document::CompositionId id) {
    Usage usage;
    for (const auto& composition : project.compositions()) {
        std::size_t uses = 0;
        for (const auto& node : composition.graph().nodes())
            if (node.typeId == document::kCompositionSourceNodeType &&
                compositionSourceId(composition, node) == id)
                ++uses;
        usage.uses += uses;
        usage.compositions += uses > 0 ? 1 : 0;
    }
    return usage;
}

std::optional<document::AssetFolderId> targetFolder(const QTreeWidgetItem* item) {
    if (item == nullptr)
        return std::nullopt;
    if (const auto folder = folderId(item))
        return folder;
    return item->parent() ? folderId(item->parent()) : std::nullopt;
}

bool AssetItem::operator<(const QTreeWidgetItem& other) const {
    const auto* tree = treeWidget();
    const int column = tree ? tree->sortColumn() : NameColumn;
    const bool ascending =
        tree == nullptr || tree->header()->sortIndicatorOrder() == Qt::AscendingOrder;
    // A rule that must hold in both directions: Qt sorts descending by asking `other < this`.
    const auto first = [ascending](const bool thisFirst) {
        return ascending ? thisFirst : !thisFirst;
    };
    const auto rank = [](const QTreeWidgetItem& item) {
        return isData(&item) ? 2 : folderId(&item) ? 0 : 1;
    };
    if (rank(*this) != rank(other))
        return first(rank(*this) < rank(other));
    const auto left = data(column, kSortRole);
    const auto right = other.data(column, kSortRole);
    if (left.isValid() != right.isValid())
        return first(left.isValid());
    if (left.isValid() && left.toDouble() != right.toDouble())
        return left.toDouble() < right.toDouble();
    static thread_local const QCollator collator = [] {
        QCollator value;
        value.setNumericMode(true);
        value.setCaseSensitivity(Qt::CaseInsensitive);
        return value;
    }();
    const auto byText = [&](const int index) {
        return collator.compare(text(index), other.text(index));
    };
    if (!left.isValid()) {
        if (text(column).isEmpty() != other.text(column).isEmpty())
            return first(!text(column).isEmpty());
        if (const auto order = byText(column); order != 0)
            return order < 0;
    }
    return column != NameColumn && byText(NameColumn) < 0;
}

AssetTree::AssetTree(CompositionSession& session, QWidget* parent)
    : QTreeWidget(parent), session_(session), token_(QUuid::createUuid().toByteArray(QUuid::Id128)),
      gvfsRoot_(defaultGvfsRoot()) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            [this] { token_ = QUuid::createUuid().toByteArray(QUuid::Id128); });
    setItemDelegate(new AssetNameDelegate(this));
    setAcceptDrops(true);
    setDragEnabled(true);
    setDropIndicatorShown(true);
    // Rows are sorted by their columns, so a drop names a folder, never a position between rows.
    setDragDropOverwriteMode(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    invisibleRootItem()->setFlags(invisibleRootItem()->flags() | Qt::ItemIsDropEnabled);
}
void AssetTree::beginRename(QTreeWidgetItem* item) {
    if (!item || !item->flags().testFlag(Qt::ItemIsEditable))
        return;
    scrollToItem(item);
    editItem(item, NameColumn);
}
bool AssetTree::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && state() != EditingState &&
        treeKey(*static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return QTreeWidget::event(event);
}
void AssetTree::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const bool plain =
        event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier;
    if (plain && state() != EditingState &&
        (key == Qt::Key_F2 || key == Qt::Key_Return || key == Qt::Key_Enter)) {
        beginRename(currentItem());
        event->accept();
    } else if (plain && (key == Qt::Key_Delete || key == Qt::Key_Backspace)) {
        if (deleteRequested)
            deleteRequested();
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
    const auto carried = [&items](const QTreeWidgetItem* item) {
        // A folder already carries what it holds.
        for (auto* parent = item->parent(); parent; parent = parent->parent())
            if (items.contains(parent))
                return false;
        return items.contains(item);
    };
    QByteArray ids;
    document::AssetId firstAsset;
    document::CompositionId firstComposition;
    for (QTreeWidgetItemIterator it(const_cast<AssetTree*>(this)); *it; ++it) {
        if (!carried(*it))
            continue;
        if (const auto asset = assetId(*it); asset.isValid()) {
            firstAsset = firstAsset.isValid() ? firstAsset : asset;
            ids += ":a" + QByteArray::number(static_cast<qulonglong>(asset.value()));
        } else if (const auto composition = compositionId(*it); composition.isValid()) {
            firstComposition = firstComposition.isValid() ? firstComposition : composition;
            ids += ":c" + QByteArray::number(static_cast<qulonglong>(composition.value()));
        } else if (const auto folder = folderId(*it)) {
            ids += ":f" + QByteArray::number(static_cast<qulonglong>(folder->value()));
        }
    }
    // Timeline and Viewer drops make a layer of every asset and composition, in the order they
    // were selected (`items` is the selection order); Nodes reads the first asset, or the first
    // composition when no asset.
    std::vector<std::variant<document::AssetId, document::CompositionId>> layerItems;
    for (const auto* item : items) {
        if (const auto asset = assetId(item); asset.isValid())
            layerItems.emplace_back(asset);
        else if (const auto composition = compositionId(item); composition.isValid())
            layerItems.emplace_back(composition);
    }
    if (const auto payload = layerItemsMimePayload(session_, layerItems); !payload.isEmpty())
        mime->setData(kLayerItemsMimeType, payload);
    if (firstAsset.isValid())
        mime->setData(kAssetMimeType, assetMimePayload(session_, firstAsset));
    else if (firstComposition.isValid())
        mime->setData(kCompositionMimeType,
                      QByteArray::number(static_cast<qulonglong>(firstComposition.value())));
    if (!ids.isEmpty())
        mime->setData(kInternalMimeType, token_ + ':' +
                                             QByteArray::number(static_cast<qulonglong>(
                                                 session_.snapshot().revision().value())) +
                                             ids);
    return mime;
}
void AssetTree::startDrag(Qt::DropActions) {
    QDrag drag(this);
    drag.setMimeData(mimeData(selectedItems()));
    // The transaction rebuilds the projection. QTreeWidget's default Move cleanup must not
    // subsequently remove rows from that freshly rebuilt tree.
    (void)drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
}
AssetTree::Payload AssetTree::internalPayload(const QMimeData& mime) const {
    const auto payload = mime.data(kInternalMimeType);
    if (payload.size() > 2200000)
        return {};
    const auto parts = payload.split(':');
    if (parts.size() < 3 || parts.front() != token_ ||
        parts[1] !=
            QByteArray::number(static_cast<qulonglong>(session_.snapshot().revision().value())))
        return {};
    const auto& project = session_.snapshot().project();
    Payload result;
    std::set<QByteArray> unique;
    for (qsizetype index = 2; index < parts.size(); ++index) {
        const auto& part = parts[index];
        bool ok = false;
        const auto raw = part.size() > 1 ? part.mid(1).toULongLong(&ok) : 0;
        if (!ok || raw == 0 || !unique.insert(part).second)
            return {};
        if (part.front() == 'a' && project.findAsset(document::AssetId::fromRaw(raw)))
            result.assets.push_back(document::AssetId::fromRaw(raw));
        else if (part.front() == 'c' &&
                 project.findComposition(document::CompositionId::fromRaw(raw)))
            result.compositions.push_back(document::CompositionId::fromRaw(raw));
        else if (part.front() == 'f' &&
                 project.findAssetFolder(document::AssetFolderId::fromRaw(raw)))
            result.folders.push_back(document::AssetFolderId::fromRaw(raw));
        else
            return {};
    }
    return result;
}
bool AssetTree::acceptsMove(const Payload& payload,
                            const std::optional<document::AssetFolderId> folder) const {
    const auto& project = session_.snapshot().project();
    for (auto cursor = folder; cursor;) {
        if (std::ranges::find(payload.folders, *cursor) != payload.folders.end())
            return false;
        const auto* record = project.findAssetFolder(*cursor);
        cursor = record ? record->parent : std::nullopt;
    }
    return true;
}
std::optional<std::optional<document::AssetFolderId>>
AssetTree::dropFolder(const QPoint position) const {
    const auto* item = itemAt(position);
    if (isData(item))
        return std::nullopt;
    return std::optional<std::optional<document::AssetFolderId>>(targetFolder(item));
}
void AssetTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        if (anyActionableUrl(*event->mimeData(), gvfsRoot_))
            event->acceptProposedAction();
        else
            event->ignore();
    } else if (!internalPayload(*event->mimeData()).empty()) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else
        event->ignore();
}
void AssetTree::dragMoveEvent(QDragMoveEvent* event) {
    const auto folder = dropFolder(event->position().toPoint());
    if (event->mimeData()->hasUrls()) {
        if (folder && anyActionableUrl(*event->mimeData(), gvfsRoot_))
            event->acceptProposedAction();
        else
            event->ignore();
        return;
    }
    const auto payload = internalPayload(*event->mimeData());
    // The base class scrolls near the edges and draws the indicator; acceptance is ours.
    QTreeWidget::dragMoveEvent(event);
    if (payload.empty() || !folder || !acceptsMove(payload, *folder)) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}
void AssetTree::dropEvent(QDropEvent* event) {
    const auto folder = dropFolder(event->position().toPoint());
    if (event->mimeData()->hasUrls()) {
        QStringList files;
        QStringList unresolvedShares;
        for (const auto& url : event->mimeData()->urls()) {
            if (auto local = localPathForUrl(url, gvfsRoot_)) {
                files.push_back(*local);
                continue;
            }
            if (auto label = smbShareLabel(url); label && !unresolvedShares.contains(*label))
                unresolvedShares.push_back(*label);
        }
        // Files land in the folder they were dropped on (#378), the root for empty space.
        if (auto* controller = session_.assetController(); controller && !files.empty())
            controller->importFiles(files, folder.value_or(std::nullopt));
        // No mount for this share yet: say so explicitly through the same notice path a refused
        // command uses (CompositionSession::commandRejected -> WindowStatusBar's transient
        // message), rather than dropping the URL on the floor.
        for (const auto& label : unresolvedShares)
            emit session_.commandRejected(
                tr("Connect to %1 in your file manager first").arg(label));
        if (!files.empty() || !unresolvedShares.empty())
            event->acceptProposedAction();
        else
            event->ignore();
        return;
    }
    const auto payload = internalPayload(*event->mimeData());
    if (payload.empty() || !folder || !acceptsMove(payload, *folder)) {
        event->ignore();
        return;
    }
    const auto& project = session_.snapshot().project();
    commands::Transaction transaction("Move to Folder", session_.snapshot().revision());
    if (!payload.assets.empty()) {
        // Moved assets follow the destination's own, after removing the moved ones.
        const std::unordered_set<document::AssetId> moving(payload.assets.begin(),
                                                           payload.assets.end());
        const auto staying = std::ranges::count_if(project.assets(), [&](const auto& asset) {
            return asset.folder == *folder && !moving.contains(asset.id);
        });
        transaction.emplace<commands::MoveAssets>(payload.assets, *folder,
                                                  static_cast<std::size_t>(staying));
    }
    if (!payload.compositions.empty())
        transaction.emplace<commands::MoveCompositions>(payload.compositions, *folder);
    if (!payload.folders.empty())
        transaction.emplace<commands::MoveAssetFolders>(payload.folders, *folder);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else
        event->ignore();
}
} // namespace bloom::ui::assets
