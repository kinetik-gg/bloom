#include "layer_drop.hpp"
#include "asset_drop.hpp"
#include "network_share_paths.hpp"
#include "node_editor_add.hpp"
#include "timeline_layer_timing_gesture.hpp"
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
namespace {
using commands::OperationIssueCode;
using commands::OperationResult;
using commands::OperationStatus;

std::optional<document::LayerId> outputLayer(const OperationResult& result) {
    for (const auto& output : result.outputs)
        if (output.name == "layer")
            if (const auto* id = std::get_if<document::LayerId>(&output.id))
                return *id;
    return std::nullopt;
}

// The frame start at or before `time`, inside the composition.
core::RationalTime frameStart(const document::Composition& composition,
                              const core::RationalTime time) {
    const auto rate = composition.format().frameRate();
    const auto index = nearestFrameIndexForTime(rate, composition.duration(), time);
    const auto snapped =
        index ? frameTimeForIndex(rate, composition.duration(), *index) : std::nullopt;
    return snapped.value_or(core::RationalTime{});
}
} // namespace

bool droppableAsLayer(const document::Project& project, const document::CompositionId target,
                      const LayerDropItem& item) {
    if (const auto* composition = std::get_if<document::CompositionId>(&item))
        return *composition != target && project.findComposition(*composition) != nullptr;
    const auto* asset = project.findAsset(std::get<document::AssetId>(item));
    return asset != nullptr &&
           (asset->kind == document::AssetKind::Image ||
            asset->kind == document::AssetKind::Sequence ||
            asset->kind == document::AssetKind::Video || asset->kind == document::AssetKind::Audio);
}

std::string layerDropLabel(const std::vector<LayerDropItem>& items) {
    if (items.size() > 1)
        return QObject::tr("Add %1 Layers").arg(items.size()).toStdString();
    if (!items.empty() && std::holds_alternative<document::CompositionId>(items.front()))
        return QObject::tr("Add Composition Layer").toStdString();
    return QObject::tr("Add Layer").toStdString();
}

OperationResult AddDroppedLayers::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    if (items_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Nothing to add");
    for (const auto& item : items_)
        if (!droppableAsLayer(draft.project(), composition_, item))
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "This item cannot be a layer here");
    // The new layers go directly above `anchor` (nullopt: below every layer), in item order.
    const auto entries = composition->graph().layerStack().entries();
    std::optional<document::LayerSlotId> anchor;
    switch (placement_.stack) {
    case LayerDropPlacement::Stack::Top:
        if (!entries.empty())
            anchor = entries.front().slotId;
        break;
    case LayerDropPlacement::Stack::Before:
        if (composition->graph().layerStack().find(placement_.before) == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "The layer to insert above no longer exists");
        anchor = placement_.before;
        break;
    case LayerDropPlacement::Stack::Bottom:
        break;
    }
    auto start = frameStart(*composition, placement_.start);
    std::vector<commands::OperationOutput> outputs;
    for (const auto& item : items_) {
        OperationResult added;
        if (const auto* nested = std::get_if<document::CompositionId>(&item))
            added = node_editor::AddCompositionSource(composition_, *nested, {}, true).apply(draft);
        else if (const auto id = std::get<document::AssetId>(item);
                 draft.project().findAsset(id)->kind == document::AssetKind::Audio)
            added = commands::AddAudioLayer(composition_, id).apply(draft);
        else
            added = commands::AddImageLayer(composition_, id).apply(draft);
        if (added.status == OperationStatus::Rejected)
            return added;
        const auto layerId = outputLayer(added);
        composition = draft.project().findComposition(composition_);
        const auto* layer = layerId ? composition->graph().findLayer(*layerId) : nullptr;
        const auto stack = composition->graph().layerStack().entries();
        const auto slot = std::ranges::find(stack, *layerId, &document::LayerStackEntry::layerId);
        if (layer == nullptr || slot == stack.end())
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The new layer is not in the layer stack");
        auto moved = commands::MoveLayerBefore(composition_, slot->slotId, anchor).apply(draft);
        if (moved.status == OperationStatus::Rejected)
            return moved;
        // Move the layer to its start the way a Timeline body drag does: start, in and out
        // together, on frames, keeping at least one frame inside the composition.
        composition = draft.project().findComposition(composition_);
        layer = composition->graph().findLayer(*layerId);
        const auto duration = composition->duration();
        const auto proposal =
            proposeLayerTiming({.start = layer->timing.startTime,
                                .in = layer->inPoint,
                                .out = layer->endPoint(duration),
                                .speed = layer->timing.speed,
                                .rate = composition->format().frameRate(),
                                .duration = duration,
                                .extentStart = std::nullopt,
                                .extentEnd = std::nullopt,
                                .handle = 0,
                                .alt = false,
                                .deltaSeconds = start.toSeconds() - layer->inPoint.toSeconds()});
        if (!proposal)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The layer cannot start there");
        auto timed = commands::SetLayerTiming(composition_, *layerId, proposal->start, proposal->in,
                                              proposal->out, proposal->speed)
                         .apply(draft);
        if (timed.status == OperationStatus::Rejected)
            return timed;
        if (placement_.sequence)
            start = proposal->out;
        outputs.push_back({"layer", *layerId});
    }
    return OperationResult::applied(std::move(outputs));
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
            if (!session_.executeTransaction(std::move(transaction)).succeeded()) {
                drop->ignore();
                return true;
            }
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
            const auto composition = session_.compositionId();
            auto* session = &session_;
            AssetController::ImportFollowUp followUp;
            if (session_.composition() != nullptr)
                followUp = [session, composition, place = placement(hit, drop->modifiers())](
                               const std::vector<document::AssetId>& imported) -> QString {
                    if (session->compositionId() != composition ||
                        session->composition() == nullptr)
                        return {};
                    std::vector<LayerDropItem> items;
                    for (const auto id : imported)
                        if (droppableAsLayer(session->snapshot().project(), composition, id))
                            items.emplace_back(id);
                    if (items.empty())
                        return {};
                    commands::Transaction transaction(layerDropLabel(items),
                                                      session->snapshot().revision());
                    transaction.emplace<AddDroppedLayers>(composition, items, place);
                    if (!session->executeTransaction(std::move(transaction)).succeeded())
                        return QObject::tr("No layers were added");
                    return items.size() == 1 ? QObject::tr("Added 1 layer")
                                             : QObject::tr("Added %1 layers").arg(items.size());
                };
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

void installLayerDropTarget(QObject& target, CompositionSession& session, LayerDropHooks hooks) {
    new LayerDropTarget(target, session, std::move(hooks));
}
} // namespace bloom::ui
