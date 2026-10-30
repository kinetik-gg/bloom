#include "layer_drop.hpp"
#include "asset_drop.hpp"
#include "network_share_paths.hpp"
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QObject>
#include <QWidget>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <set>

namespace bloom::ui {

std::string layerDropLabel(const std::vector<LayerDropItem>& items) {
    if (items.size() > 1)
        return QObject::tr("Add %1 Layers").arg(items.size()).toStdString();
    if (!items.empty() && std::holds_alternative<document::CompositionId>(items.front()))
        return QObject::tr("Add Composition Layer").toStdString();
    return QObject::tr("Add Layer").toStdString();
}

std::vector<LayerDropItem> layerDropItems(const QMimeData& mime,
                                          const CompositionSession& session) {
    const auto& project = session.snapshot().project();
    std::vector<LayerDropItem> items;
    if (mime.hasFormat(kLayerItemsMimeType)) {
        for (const auto& item : layerItemsFromMime(mime, session))
            if (droppableAsLayer(project, session.compositionId(), item))
                items.push_back(item);
        return items;
    }
    // A drag that names a single asset or composition (another source than the Assets tree).
    if (const auto asset = assetFromMime(mime, session); asset.isValid())
        items.emplace_back(asset);
    else if (const auto composition = compositionFromMime(mime, session); composition.isValid())
        items.emplace_back(composition);
    std::erase_if(items, [&](const auto& item) {
        return !droppableAsLayer(project, session.compositionId(), item);
    });
    return items;
}

namespace {
// As in After Effects, the new layers end up selected (a session change, not a document edit),
// the uppermost one primary, so Properties shows it at once.
void selectDroppedLayers(CompositionSession& session, const commands::CommandResult& result) {
    std::vector<document::LayerId> layers;
    for (const auto& output : result.outputs)
        if (output.output.name == "layer")
            if (const auto* id = std::get_if<document::LayerId>(&output.output.id))
                layers.push_back(*id);
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        session.selectLayer(*it, it != layers.rbegin());
}

class LayerDropTarget final : public QObject {
  public:
    LayerDropTarget(QObject& target, CompositionSession& session, LayerDropHooks hooks)
        : QObject(&target), session_(session), hooks_(std::move(hooks)) {
        if (auto* widget = qobject_cast<QWidget*>(&target))
            widget->setAcceptDrops(true);
        target.installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::DragLeave) {
            indicate(std::nullopt);
            return false;
        }
        if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
            event->type() != QEvent::Drop)
            return false;
        auto* drop = static_cast<QDropEvent*>(event);
        const auto* mime = drop->mimeData();
        const bool dropping = event->type() == QEvent::Drop;
        const auto position = drop->position().toPoint();
        if (mime->hasFormat(kLayerItemsMimeType) || mime->hasFormat(kAssetMimeType) ||
            mime->hasFormat(kCompositionMimeType)) {
            const auto items = layerDropItems(*mime, session_);
            if (items.empty() || session_.composition() == nullptr) {
                indicate(std::nullopt);
                drop->ignore();
                return true;
            }
            const auto hit = hitAt(position);
            if (!dropping) {
                indicate(hit);
                drop->acceptProposedAction();
                return true;
            }
            indicate(std::nullopt);
            commands::Transaction transaction(layerDropLabel(items),
                                              session_.snapshot().revision());
            transaction.emplace<AddDroppedLayers>(session_.compositionId(), items,
                                                  placement(hit, drop->modifiers()));
            const auto result = session_.executeTransaction(std::move(transaction));
            if (!result.succeeded()) {
                drop->ignore();
                return true;
            }
            selectDroppedLayers(session_, result);
            drop->acceptProposedAction();
            return true;
        }
        if (!mime->hasUrls()) {
            drop->ignore();
            return true;
        }
        // Files and folders from a file manager: the import progress flow (ADR 0029) into the
        // selected Assets folder, then layers for what it imported, as a second undoable step.
        QStringList files;
        QStringList unresolvedShares;
        for (const auto& url : mime->urls()) {
            if (auto local = localPathForUrl(url)) {
                files.push_back(*local);
                continue;
            }
            if (auto label = smbShareLabel(url); label && !unresolvedShares.contains(*label))
                unresolvedShares.push_back(*label);
        }
        auto* controller = session_.assetController();
        if (controller == nullptr || (files.isEmpty() && unresolvedShares.isEmpty())) {
            indicate(std::nullopt);
            drop->ignore();
            return true;
        }
        const auto hit = hitAt(position);
        if (!dropping) {
            indicate(hit);
            drop->acceptProposedAction();
            return true;
        }
        indicate(std::nullopt);
        if (!files.isEmpty()) {
            AssetController::ImportFollowUp followUp;
            if (session_.composition() != nullptr)
                followUp = importedLayersFollowUp(session_, session_.compositionId(),
                                                  placement(hit, drop->modifiers()));
            controller->importFiles(files, controller->importFolder(), std::move(followUp));
        }
        for (const auto& label : unresolvedShares)
            emit session_.commandRejected(
                QObject::tr("Connect to %1 in your file manager first").arg(label));
        drop->acceptProposedAction();
        return true;
    }

  private:
    [[nodiscard]] LayerDropHit hitAt(const QPoint position) const {
        return hooks_.hit ? hooks_.hit(position) : LayerDropHit{};
    }
    void indicate(const std::optional<LayerDropHit>& hit) const {
        if (hooks_.indicate)
            hooks_.indicate(hit);
    }
    [[nodiscard]] LayerDropPlacement placement(const LayerDropHit& hit,
                                               const Qt::KeyboardModifiers modifiers) const {
        return {.stack = hit.stack,
                .before = hit.before,
                .start = hit.time.value_or(session_.currentTime()),
                .sequence = modifiers.testFlag(Qt::AltModifier)};
    }

    CompositionSession& session_;
    LayerDropHooks hooks_;
};
} // namespace

std::function<QString(const std::vector<document::AssetId>&)>
importedLayersFollowUp(CompositionSession& session, const document::CompositionId composition,
                       const LayerDropPlacement placement) {
    auto* target = &session;
    return [target, composition,
            placement](const std::vector<document::AssetId>& imported) -> QString {
        if (target->compositionId() != composition || target->composition() == nullptr)
            return {};
        std::vector<LayerDropItem> items;
        for (const auto id : imported)
            if (droppableAsLayer(target->snapshot().project(), composition, id))
                items.emplace_back(id);
        if (items.empty())
            return {};
        commands::Transaction transaction(layerDropLabel(items), target->snapshot().revision());
        transaction.emplace<AddDroppedLayers>(composition, items, placement);
        const auto result = target->executeTransaction(std::move(transaction));
        if (!result.succeeded())
            return QObject::tr("No layers were added");
        selectDroppedLayers(*target, result);
        return items.size() == 1 ? QObject::tr("Added 1 layer")
                                 : QObject::tr("Added %1 layers").arg(items.size());
    };
}

void installLayerDropTarget(QObject& target, CompositionSession& session, LayerDropHooks hooks) {
    new LayerDropTarget(target, session, std::move(hooks));
}
} // namespace bloom::ui
