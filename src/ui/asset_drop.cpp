#include "asset_drop.hpp"
#include "layer_drop.hpp"
#include "network_share_paths.hpp"
#include "node_editor_add.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsView>
#include <QMimeData>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>

namespace bloom::ui {
QByteArray assetMimePayload(const CompositionSession& session, document::AssetId asset) {
    const auto* controller = session.assetController();
    if (!controller || !controller->acceptsEdits())
        return {};
    return controller->dragToken() + ':' +
           QByteArray::number(static_cast<qulonglong>(session.snapshot().revision().value())) +
           ':' + QByteArray::number(static_cast<qulonglong>(asset.value()));
}
document::AssetId assetFromMime(const QMimeData& mime, const CompositionSession& session) {
    const auto data = mime.data(kAssetMimeType);
    const auto parts = data.split(':');
    if (parts.size() != 3)
        return {};
    const auto id = document::AssetId::fromRaw(parts[2].toULongLong());
    const auto expected = assetMimePayload(session, id);
    if (expected.isEmpty() || data != expected)
        return {};
    return session.snapshot().project().findAsset(id) ? id : document::AssetId{};
}
document::CompositionId compositionFromMime(const QMimeData& mime,
                                            const CompositionSession& session) {
    bool valid = false;
    const auto raw = mime.data(kCompositionMimeType).toULongLong(&valid);
    const auto id = document::CompositionId::fromRaw(raw);
    return valid && session.snapshot().project().findComposition(id) ? id
                                                                     : document::CompositionId{};
}
QByteArray layerItemsMimePayload(
    const CompositionSession& session,
    const std::vector<std::variant<document::AssetId, document::CompositionId>>& items) {
    const auto* controller = session.assetController();
    if (!controller || !controller->acceptsEdits() || items.empty())
        return {};
    auto payload =
        controller->dragToken() + ':' +
        QByteArray::number(static_cast<qulonglong>(session.snapshot().revision().value()));
    for (const auto& item : items) {
        if (const auto* asset = std::get_if<document::AssetId>(&item))
            payload += ":a" + QByteArray::number(static_cast<qulonglong>(asset->value()));
        else
            payload += ":c" + QByteArray::number(static_cast<qulonglong>(
                                  std::get<document::CompositionId>(item).value()));
    }
    return payload;
}
std::vector<std::variant<document::AssetId, document::CompositionId>>
layerItemsFromMime(const QMimeData& mime, const CompositionSession& session) {
    const auto* controller = session.assetController();
    const auto data = mime.data(kLayerItemsMimeType);
    const auto parts = data.split(':');
    if (!controller || parts.size() < 3 || parts[0] != controller->dragToken() ||
        parts[1] !=
            QByteArray::number(static_cast<qulonglong>(session.snapshot().revision().value())))
        return {};
    const auto& project = session.snapshot().project();
    std::vector<std::variant<document::AssetId, document::CompositionId>> items;
    for (qsizetype index = 2; index < parts.size(); ++index) {
        const auto& part = parts[index];
        bool ok = false;
        const auto raw = part.size() > 1 ? part.mid(1).toULongLong(&ok) : 0;
        if (!ok)
            return {};
        if (part.front() == 'a' && project.findAsset(document::AssetId::fromRaw(raw)))
            items.emplace_back(document::AssetId::fromRaw(raw));
        else if (part.front() == 'c' &&
                 project.findComposition(document::CompositionId::fromRaw(raw)))
            items.emplace_back(document::CompositionId::fromRaw(raw));
        else
            return {};
    }
    return items;
}
document::CompositionId compositionSourceId(const document::Composition& composition,
                                            const document::NodeRecord& node) {
    if (node.typeId != document::kCompositionSourceNodeType)
        return {};
    for (const auto& binding : node.parameters) {
        if (binding.role != "composition")
            continue;
        const auto* parameter = composition.parameters().find(binding.parameterId);
        const auto* constant =
            parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
        const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
        if (value && *value > 0)
            return document::CompositionId::fromRaw(static_cast<std::uint64_t>(*value));
    }
    return {};
}
namespace {
class AddImageNode final : public commands::Operation {
  public:
    AddImageNode(document::CompositionId composition, document::AssetId asset,
                 document::Vec2d position)
        : composition_(composition), asset_(asset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-image";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        if (!draft.project().findAsset(asset_))
            return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                       "Image asset does not exist");
        const auto* asset = draft.project().findAsset(asset_);
        auto added = commands::AddNode(composition_,
                                       asset && asset->kind == document::AssetKind::Video
                                           ? "bloom.video-source"
                                           : "bloom.image-source",
                                       position_)
                         .apply(draft);
        if (added.status != commands::OperationStatus::Applied)
            return added;
        for (const auto& output : added.outputs)
            if (output.name == commands::kAddNodeOutput) {
                const auto* id = std::get_if<document::NodeId>(&output.id);
                const auto* composition = draft.project().findComposition(composition_);
                const auto* node = id && composition ? composition->graph().findNode(*id) : nullptr;
                if (!node)
                    break;
                // The new node inherits the asset's probed alpha association (see AddImageLayer).
                for (const auto& binding : node->parameters)
                    if (binding.role == "premultiply") {
                        auto result = commands::SetParameterSource(
                                          composition_, binding.parameterId,
                                          document::ConstantValueSource{
                                              asset->interpretation.alphaAssociation !=
                                              document::AssetAlphaAssociation::Premultiplied})
                                          .apply(draft);
                        if (result.status == commands::OperationStatus::Rejected)
                            return result;
                    }
                for (const auto& binding : node->parameters)
                    if (binding.role == "asset") {
                        auto result =
                            commands::SetParameterSource(
                                composition_, binding.parameterId,
                                document::ConstantValueSource{std::to_string(asset_.value())})
                                .apply(draft);
                        if (result.status == commands::OperationStatus::Rejected)
                            return result;
                        return added;
                    }
            }
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Image node asset parameter is missing");
    }

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};
class AddAudioNode final : public commands::Operation {
  public:
    AddAudioNode(document::CompositionId composition, document::AssetId asset,
                 document::Vec2d position)
        : composition_(composition), asset_(asset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-audio";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        const auto* asset = draft.project().findAsset(asset_);
        if (asset == nullptr || asset->kind != document::AssetKind::Audio)
            return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                       "Audio asset does not exist");
        auto added = commands::AddNode(composition_, "bloom.audio-source", position_).apply(draft);
        if (added.status != commands::OperationStatus::Applied)
            return added;
        for (const auto& output : added.outputs)
            if (output.name == commands::kAddNodeOutput) {
                const auto* id = std::get_if<document::NodeId>(&output.id);
                const auto* composition = draft.project().findComposition(composition_);
                const auto* node = id && composition ? composition->graph().findNode(*id) : nullptr;
                if (!node)
                    break;
                for (const auto& binding : node->parameters)
                    if (binding.role == "asset") {
                        auto result =
                            commands::SetParameterSource(
                                composition_, binding.parameterId,
                                document::ConstantValueSource{std::to_string(asset_.value())})
                                .apply(draft);
                        if (result.status == commands::OperationStatus::Rejected)
                            return result;
                        return added;
                    }
            }
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Audio node asset parameter is missing");
    }

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};
class AssetDropTarget final : public QObject {
  public:
    AssetDropTarget(QWidget& widget, CompositionSession& session, QGraphicsView& view)
        : QObject(&widget), session_(session), view_(view) {
        widget.setAcceptDrops(true);
        widget.installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
            event->type() != QEvent::Drop)
            return false;
        auto* drop = static_cast<QDropEvent*>(event);
        const auto* mime = drop->mimeData();
        if (mime->hasUrls() && !mime->hasFormat(kAssetMimeType) &&
            !mime->hasFormat(kCompositionMimeType)) {
            // Files and folders from a file manager import into the selected Assets folder with
            // the same import progress and summary as Assets itself (#377); no node is added.
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
                drop->ignore();
                return true;
            }
            if (event->type() == QEvent::Drop) {
                if (!files.isEmpty())
                    controller->importFiles(files);
                for (const auto& label : unresolvedShares)
                    emit session_.commandRejected(
                        QObject::tr("Connect to %1 in your file manager first").arg(label));
            }
            drop->acceptProposedAction();
            return true;
        }
        if (mime->hasFormat(kCompositionMimeType)) {
            const auto id = compositionFromMime(*drop->mimeData(), session_);
            if (!id.isValid()) {
                drop->ignore();
                return true;
            }
            if (event->type() == QEvent::Drop) {
                const auto point = view_.mapToScene(drop->position().toPoint());
                commands::Transaction transaction("Add Composition Source",
                                                  session_.snapshot().revision());
                transaction.emplace<node_editor::AddCompositionSource>(
                    session_.compositionId(), id, document::Vec2d{point.x(), point.y()});
                if (!session_.executeTransaction(std::move(transaction)).succeeded()) {
                    drop->ignore();
                    return true;
                }
            }
            drop->acceptProposedAction();
            return true;
        }
        const auto id = assetFromMime(*drop->mimeData(), session_);
        const auto* droppedAsset = session_.snapshot().project().findAsset(id);
        if (!id.isValid() || (droppedAsset && droppedAsset->kind == document::AssetKind::Lut)) {
            drop->ignore();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            const auto* asset = session_.snapshot().project().findAsset(id);
            const bool audio = asset != nullptr && asset->kind == document::AssetKind::Audio;
            const bool video = asset != nullptr && asset->kind == document::AssetKind::Video;
            commands::Transaction transaction(audio   ? "Add Audio Source"
                                              : video ? "Add Video Source"
                                                      : "Add Image Source",
                                              session_.snapshot().revision());
            const auto point = view_.mapToScene(drop->position().toPoint());
            if (audio)
                transaction.emplace<AddAudioNode>(session_.compositionId(), id,
                                                  document::Vec2d{point.x(), point.y()});
            else
                transaction.emplace<AddImageNode>(session_.compositionId(), id,
                                                  document::Vec2d{point.x(), point.y()});
            static_cast<void>(session_.executeTransaction(std::move(transaction)));
        }
        drop->acceptProposedAction();
        return true;
    }

  private:
    CompositionSession& session_;
    QGraphicsView& view_;
};
} // namespace
void installAssetDropTarget(QWidget& widget, CompositionSession& session, QGraphicsView* view) {
    if (view == nullptr)
        installLayerDropTarget(widget, session);
    else
        new AssetDropTarget(widget, session, *view);
}
} // namespace bloom::ui
