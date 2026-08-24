#include <bloom/ui/composition_session.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QThread>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui {
namespace {

QString statusMessage(const commands::CommandResult& result) {
    if (!result.operationFailures.empty()) {
        return QString::fromStdString(result.operationFailures.front().issue.message);
    }
    if (!result.validation.ok()) {
        return QString::fromStdString(result.validation.issues().front().message);
    }

    switch (result.status) {
    case commands::CommandStatus::Succeeded:
    case commands::CommandStatus::NoChange:
        return {};
    case commands::CommandStatus::Rejected:
        return QStringLiteral("The edit was rejected");
    case commands::CommandStatus::ValidationFailed:
        return QStringLiteral("The edit would make the composition invalid");
    case commands::CommandStatus::StaleRevision:
        return QStringLiteral("The composition changed before the edit could be applied");
    case commands::CommandStatus::ForeignDocument:
        return QStringLiteral("The command belongs to a different document");
    case commands::CommandStatus::DraftBaseMismatch:
        return QStringLiteral("The command draft no longer matches its document snapshot");
    case commands::CommandStatus::RevisionOverflow:
        return QStringLiteral("The document revision limit was reached");
    case commands::CommandStatus::NothingToUndo:
        return QStringLiteral("There is nothing to undo");
    case commands::CommandStatus::NothingToRedo:
        return QStringLiteral("There is nothing to redo");
    }
    return QStringLiteral("The edit could not be applied");
}

[[nodiscard]] document::Vec2d compositionCenter(const document::Composition& composition) {
    const auto format = composition.format();
    return {static_cast<double>(format.width()) * 0.5, static_cast<double>(format.height()) * 0.5};
}

} // namespace

CompositionSession::CompositionSession(document::Document& document,
                                       commands::CommandStack& commandStack,
                                       document::CompositionId compositionId, QObject* parent)
    : QObject(parent), document_(document), commandStack_(commandStack),
      snapshot_(document.snapshot()), compositionId_(compositionId) {
    if (composition() == nullptr && !snapshot_.project().compositions().empty()) {
        compositionId_ = snapshot_.project().compositions().front().id();
    }
}

const document::Snapshot& CompositionSession::snapshot() const noexcept { return snapshot_; }

document::CompositionId CompositionSession::compositionId() const noexcept {
    return compositionId_;
}

const document::Composition* CompositionSession::composition() const noexcept {
    return snapshot_.project().findComposition(compositionId_);
}

const CompositionSelection& CompositionSession::selection() const noexcept { return selection_; }

core::RationalTime CompositionSession::currentTime() const noexcept { return currentTime_; }

bool CompositionSession::setComposition(const document::CompositionId compositionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (compositionId == compositionId_) {
        return true;
    }
    if (snapshot_.project().findComposition(compositionId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected composition is no longer available"));
        return false;
    }

    compositionId_ = compositionId;
    const bool timeChanged = currentTime_ != core::RationalTime::fromInteger(0);
    currentTime_ = core::RationalTime::fromInteger(0);
    const bool hadSelection = selection_.primary.index() != 0;
    selection_ = {};
    emit compositionChanged();
    if (timeChanged) {
        emit currentTimeChanged();
    }
    if (hadSelection) {
        emit selectionChanged();
    }
    return true;
}

bool CompositionSession::setCurrentTime(const core::RationalTime time) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (currentTime_ == time) {
        return false;
    }
    currentTime_ = time;
    emit currentTimeChanged();
    return true;
}

void CompositionSession::clearSelection() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (selection_ == CompositionSelection{}) {
        return;
    }
    selection_ = {};
    emit selectionChanged();
}

void CompositionSession::selectLayer(const document::LayerId layerId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto boundary = boundaryNodeForLayer(layerId);
    if (!boundary.has_value()) {
        reportUnavailable(QStringLiteral("The selected layer is no longer available"));
        return;
    }
    CompositionSelection next{.primary = layerId, .contextualLayer = layerId};
    if (selection_ != next) {
        selection_ = next;
        emit selectionChanged();
    }
}

void CompositionSession::selectNode(const document::NodeId nodeId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr || current->graph().findNode(nodeId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected node is no longer available"));
        return;
    }
    CompositionSelection next{.primary = nodeId, .contextualLayer = layerForNode(nodeId)};
    if (selection_ != next) {
        selection_ = next;
        emit selectionChanged();
    }
}

void CompositionSession::selectParameter(const document::ParameterId parameterId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr || current->parameters().find(parameterId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected parameter is no longer available"));
        return;
    }

    std::optional<document::LayerId> contextualLayer;
    for (const auto& node : current->graph().nodes()) {
        const auto binding = std::ranges::find_if(node.parameters, [parameterId](const auto& item) {
            return item.parameterId == parameterId;
        });
        if (binding != node.parameters.end()) {
            contextualLayer = layerForNode(node.id);
            break;
        }
    }

    CompositionSelection next{.primary = parameterId, .contextualLayer = contextualLayer};
    if (selection_ != next) {
        selection_ = next;
        emit selectionChanged();
    }
}

const document::NodeRecord* CompositionSession::selectedNode() const noexcept {
    return nodeForSelection(selection_);
}

std::optional<document::LayerId>
CompositionSession::layerForNode(const document::NodeId nodeId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }

    std::vector<document::NodeId> pending{nodeId};
    std::vector<document::NodeId> visited;
    std::optional<document::LayerId> resolvedLayer;
    while (!pending.empty()) {
        const auto currentNodeId = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, currentNodeId) != visited.end()) {
            continue;
        }
        visited.push_back(currentNodeId);

        for (const auto& boundary : current->graph().layerOutputs()) {
            if (boundary.nodeId != currentNodeId) {
                continue;
            }
            if (resolvedLayer.has_value() && *resolvedLayer != boundary.layerId) {
                return std::nullopt;
            }
            resolvedLayer = boundary.layerId;
        }

        for (const auto& edge : current->graph().edges()) {
            if (edge.source.nodeId != currentNodeId) {
                continue;
            }
            if (const auto* destination = std::get_if<document::NodeInputRef>(&edge.destination)) {
                pending.push_back(destination->nodeId);
            }
        }
    }
    return resolvedLayer;
}

std::optional<document::NodeId>
CompositionSession::boundaryNodeForLayer(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto boundary =
        std::ranges::find_if(current->graph().layerOutputs(), [layerId](const auto& candidate) {
            return candidate.layerId == layerId;
        });
    if (boundary == current->graph().layerOutputs().end()) {
        return std::nullopt;
    }
    return boundary->nodeId;
}

std::optional<document::NodeId>
CompositionSession::directSourceNodeForLayer(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    const auto boundaryNodeId = boundaryNodeForLayer(layerId);
    if (current == nullptr || !boundaryNodeId.has_value()) {
        return std::nullopt;
    }

    std::optional<document::NodeId> sourceNodeId;
    for (const auto& edge : current->graph().edges()) {
        const auto* destination = std::get_if<document::NodeInputRef>(&edge.destination);
        if (destination == nullptr || destination->nodeId != *boundaryNodeId ||
            destination->port != document::kLayerOutputContentInputPort) {
            continue;
        }
        const auto* sourceNode = current->graph().findNode(edge.source.nodeId);
        if (sourceNodeId.has_value() || sourceNode == nullptr) {
            return std::nullopt;
        }
        if ((sourceNode->typeId == document::kSolidSourceNodeType &&
             sourceNode->schemaVersion == document::kSolidSourceNodeSchemaVersion &&
             edge.source.port != document::kSolidSourceOutputPort) ||
            (sourceNode->typeId == document::kTextSourceNodeType &&
             sourceNode->schemaVersion == document::kTextSourceNodeSchemaVersion &&
             edge.source.port != document::kTextSourceOutputPort)) {
            return std::nullopt;
        }
        sourceNodeId = edge.source.nodeId;
    }
    return sourceNodeId;
}

const document::ParameterRecord*
CompositionSession::parameterForSelection(const std::string_view role) const noexcept {
    const auto* current = composition();
    const auto* node = nodeForSelection(selection_);
    if (current == nullptr) {
        return nullptr;
    }
    if (node != nullptr) {
        if (const auto* parameter = parameterForNode(*node, role)) {
            return parameter;
        }
        if (const auto* layerId = std::get_if<document::LayerId>(&selection_.primary)) {
            const auto sourceNodeId = directSourceNodeForLayer(*layerId);
            const auto* sourceNode =
                sourceNodeId.has_value() ? current->graph().findNode(*sourceNodeId) : nullptr;
            return sourceNode == nullptr ? nullptr : parameterForNode(*sourceNode, role);
        }
    }
    if (const auto* parameterId = std::get_if<document::ParameterId>(&selection_.primary)) {
        for (const auto& candidate : current->graph().nodes()) {
            const auto binding =
                std::ranges::find_if(candidate.parameters, [parameterId, role](const auto& item) {
                    return item.parameterId == *parameterId && item.role == role;
                });
            if (binding != candidate.parameters.end()) {
                return current->parameters().find(*parameterId);
            }
        }
    }
    return nullptr;
}

std::optional<double>
CompositionSession::constantValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<double>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<double>(*value);
}

std::optional<document::Vec2d>
CompositionSession::constantVec2Value(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<document::Vec2d>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<document::Vec2d>(*value);
}

std::optional<core::Color4d>
CompositionSession::constantColorValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<core::Color4d>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<core::Color4d>(*value);
}

bool CompositionSession::addSolidLayer(const QString& name, const core::Color4d color) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr) {
        reportUnavailable(QStringLiteral("No composition is available for the new solid layer"));
        return false;
    }
    commands::Transaction transaction("Add Solid Layer", snapshot_.revision());
    transaction.emplace<commands::AddSolidLayer>(compositionId_, name.toStdString(), color,
                                                 compositionCenter(*current));
    const auto result = commandStack_.execute(std::move(transaction));
    const auto layerId = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    if (!handleResult(result)) {
        return false;
    }
    if (layerId.has_value()) {
        selectLayer(*layerId);
    }
    return true;
}

bool CompositionSession::addTextLayer(const QString& name, const QString& text) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr) {
        reportUnavailable(QStringLiteral("No composition is available for the new text layer"));
        return false;
    }
    commands::Transaction transaction("Add Text Layer", snapshot_.revision());
    transaction.emplace<commands::AddTextLayer>(compositionId_, name.toStdString(),
                                                text.toStdString(), compositionCenter(*current));
    const auto result = commandStack_.execute(std::move(transaction));
    const auto layerId = result.outputId<document::LayerId>(commands::kAddTextLayerLayerOutput);
    if (!handleResult(result)) {
        return false;
    }
    if (layerId.has_value()) {
        selectLayer(*layerId);
    }
    return true;
}

bool CompositionSession::setSelectionScalarParameter(const std::string_view role,
                                                     const double value,
                                                     const QString& commandLabel) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!std::isfinite(value)) {
        reportUnavailable(QStringLiteral("The parameter value must be finite"));
        return false;
    }

    const auto* parameter = parameterForSelection(role);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    if (const auto* constantSource =
            std::get_if<document::ConstantValueSource>(&parameter->source)) {
        if (std::get_if<double>(&constantSource->value) == nullptr) {
            reportUnavailable(QStringLiteral("The parameter value does not match its schema"));
            return false;
        }
        transaction.emplace<commands::SetParameterSource>(compositionId_, parameter->id,
                                                          document::ConstantValueSource{value});
    } else if (const auto* animationSource =
                   std::get_if<document::AnimationCurveSource>(&parameter->source)) {
        transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, animationSource->curveId,
                                                         currentTime_, value);
    } else {
        reportUnavailable(
            QStringLiteral("Disconnect the driven parameter before editing its value"));
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::setSelectedPosition(const double x, const double y) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* position = parameterForSelection(document::kPositionParameterRole);
    if (position == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose a position"));
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        reportUnavailable(QStringLiteral("Position values must be finite"));
        return false;
    }
    const document::Vec2d value{x, y};
    commands::Transaction transaction("Set Position", snapshot_.revision());
    if (const auto* constantSource =
            std::get_if<document::ConstantValueSource>(&position->source)) {
        if (std::get_if<document::Vec2d>(&constantSource->value) == nullptr) {
            reportUnavailable(QStringLiteral("The position value does not match its schema"));
            return false;
        }
        transaction.emplace<commands::SetParameterSource>(compositionId_, position->id,
                                                          document::ConstantValueSource{value});
    } else if (const auto* animationSource =
                   std::get_if<document::AnimationCurveSource>(&position->source)) {
        transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, animationSource->curveId,
                                                         currentTime_, value);
    } else {
        reportUnavailable(
            QStringLiteral("Disconnect the driven position before editing its value"));
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::setSelectedOpacity(const double opacity) {
    if (opacity < 0.0 || opacity > 1.0) {
        reportUnavailable(QStringLiteral("Opacity must be between zero and one"));
        return false;
    }
    return setSelectionScalarParameter(document::kOpacityParameterRole, opacity,
                                       QStringLiteral("Set Opacity"));
}

bool CompositionSession::moveLayerBefore(const document::LayerSlotId slotId,
                                         const std::optional<document::LayerSlotId> beforeSlotId) {
    Q_ASSERT(QThread::currentThread() == thread());
    commands::Transaction transaction("Reorder Layer", snapshot_.revision());
    transaction.emplace<commands::MoveLayerBefore>(compositionId_, slotId, beforeSlotId);
    return execute(std::move(transaction));
}

bool CompositionSession::canUndo() const noexcept { return commandStack_.canUndo(); }

bool CompositionSession::canRedo() const noexcept { return commandStack_.canRedo(); }

QString CompositionSession::undoLabel() const {
    const auto label = commandStack_.undoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

QString CompositionSession::redoLabel() const {
    const auto label = commandStack_.redoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

bool CompositionSession::undo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_.undo());
}

bool CompositionSession::redo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_.redo());
}

bool CompositionSession::execute(commands::Transaction&& transaction) {
    return handleResult(commandStack_.execute(std::move(transaction)));
}

bool CompositionSession::handleResult(const commands::CommandResult& result) {
    const auto previousRevision = snapshot_.revision();
    snapshot_ = document_.snapshot();

    if (!result.succeeded()) {
        const auto message = statusMessage(result);
        if (!message.isEmpty()) {
            emit commandRejected(message);
        }
        if (snapshot_.revision() != previousRevision) {
            normalizeSelection();
            emit snapshotChanged();
        }
        emit historyChanged();
        return false;
    }

    if (snapshot_.revision() != previousRevision) {
        normalizeSelection();
        emit snapshotChanged();
    }
    emit historyChanged();
    return true;
}

const document::NodeRecord*
CompositionSession::nodeForSelection(const CompositionSelection& selection) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return nullptr;
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&selection.primary)) {
        return current->graph().findNode(*nodeId);
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&selection.primary)) {
        const auto nodeId = boundaryNodeForLayer(*layerId);
        return nodeId.has_value() ? current->graph().findNode(*nodeId) : nullptr;
    }
    return nullptr;
}

const document::ParameterRecord*
CompositionSession::parameterForNode(const document::NodeRecord& node,
                                     const std::string_view role) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return nullptr;
    }
    const auto binding = std::ranges::find_if(
        node.parameters, [role](const auto& item) { return item.role == role; });
    return binding == node.parameters.end() ? nullptr
                                            : current->parameters().find(binding->parameterId);
}

bool CompositionSession::selectionExists(const CompositionSelection& selection) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return false;
    }
    if (std::holds_alternative<std::monostate>(selection.primary)) {
        return true;
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&selection.primary)) {
        return boundaryNodeForLayer(*layerId).has_value();
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&selection.primary)) {
        return current->graph().findNode(*nodeId) != nullptr;
    }
    const auto* parameterId = std::get_if<document::ParameterId>(&selection.primary);
    return parameterId != nullptr && current->parameters().find(*parameterId) != nullptr;
}

void CompositionSession::normalizeSelection() {
    if (selectionExists(selection_)) {
        return;
    }
    selection_ = {};
    emit selectionChanged();
}

void CompositionSession::reportUnavailable(const QString& message) {
    emit commandRejected(message);
}

} // namespace bloom::ui
