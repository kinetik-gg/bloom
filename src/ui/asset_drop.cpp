#include "asset_drop.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsView>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>

namespace bloom::ui {
document::AssetId assetFromMime(const QMimeData& mime, const CompositionSession& session) {
    const auto parts = mime.data(kAssetMimeType).split(':');
    if (parts.size() != 2 || parts[0].toULongLong() != session.snapshot().project().id().value())
        return {};
    const auto id = document::AssetId::fromRaw(parts[1].toULongLong());
    return session.snapshot().project().findAsset(id) ? id : document::AssetId{};
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
        auto added = commands::AddNode(composition_, "bloom.image-source", position_).apply(draft);
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
                                                   "Image node asset parameter is missing");
    }

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};
class AssetDropTarget final : public QObject {
  public:
    AssetDropTarget(QWidget& widget, CompositionSession& session, QGraphicsView* view)
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
        const auto id = assetFromMime(*drop->mimeData(), session_);
        if (!id.isValid()) {
            drop->ignore();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            commands::Transaction transaction(view_ ? "Add Image Source" : "Add Image Layer",
                                              session_.snapshot().revision());
            if (view_) {
                const auto point = view_->mapToScene(drop->position().toPoint());
                transaction.emplace<AddImageNode>(session_.compositionId(), id,
                                                  document::Vec2d{point.x(), point.y()});
            } else
                transaction.emplace<commands::AddImageLayer>(session_.compositionId(), id);
            static_cast<void>(session_.executeTransaction(std::move(transaction)));
        }
        drop->acceptProposedAction();
        return true;
    }

  private:
    CompositionSession& session_;
    QGraphicsView* view_;
};
} // namespace
void installAssetDropTarget(QWidget& widget, CompositionSession& session, QGraphicsView* view) {
    new AssetDropTarget(widget, session, view);
}
} // namespace bloom::ui
