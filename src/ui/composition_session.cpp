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

// The contract's "lowest valid CompositionId" (docs/architecture/project-session.md, "Session
// Publication", item 5), mirroring ProjectHost::lowestCompositionId()'s own min-scan (issue #75:
// this used to be `compositions().front().id()`, which is insertion order, not id order -- a
// document whose compositions were inserted out of id order picked the wrong fallback
// composition). Precondition: `project.compositions()` is non-empty; callers below only invoke
// this after checking that.
[[nodiscard]] document::CompositionId lowestCompositionId(const document::Project& project) {
    const auto compositions = project.compositions();
    auto lowest = compositions.front().id();
    for (const auto& candidate : compositions) {
        if (candidate.id().value() < lowest.value()) {
            lowest = candidate.id();
        }
    }
    return lowest;
}

} // namespace

CompositionSession::CompositionSession(document::Document& document,
                                       commands::CommandStack& commandStack,
                                       document::CompositionId compositionId, QObject* parent)
    : QObject(parent), document_(&document), commandStack_(&commandStack),
      snapshot_(document.snapshot()), compositionId_(compositionId) {
    if (composition() == nullptr && !snapshot_.project().compositions().empty()) {
        compositionId_ = lowestCompositionId(snapshot_.project());
    }
}

void CompositionSession::rebind(document::Document& document, commands::CommandStack& commandStack,
                                const document::CompositionId compositionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    document_ = &document;
    commandStack_ = &commandStack;
    snapshot_ = document_->snapshot();
    compositionId_ = compositionId;
    currentTime_ = core::RationalTime::fromInteger(0);
    selection_ = {};
    // The OLD document/command-stack are left untouched, but this session's own interaction state
    // targets them and must not survive the swap.
    positionInteraction_.reset();

    // One coherent transition (docs/architecture/project-session.md, "Session Publication"):
    // observers must never see a new document paired with stale selection/time/history, so every
    // existing changed signal fires here, unconditionally, in this order.
    emit snapshotChanged();
    emit compositionChanged();
    emit currentTimeChanged();
    emit selectionChanged();
    emit historyChanged();
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

    // A composition switch cancels any active interaction (docs/architecture/animation-and-time.md,
    // "Direct Manipulation And Preview Overrides"): its frozen target/mapping belong to the OLD
    // composition.
    cancelPositionInteraction();
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

    CompositionSelection next{.primary = parameterId,
                              .contextualLayer = contextualLayerForParameter(parameterId)};
    if (selection_ != next) {
        selection_ = next;
        emit selectionChanged();
    }
}

void CompositionSession::selectKeyframe(const document::AnimationCurveId curveId,
                                        const document::KeyframeId keyframeId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const KeyframeSelection target{curveId, keyframeId};
    if (!keyframeSelectionExists(target)) {
        reportUnavailable(QStringLiteral("The selected keyframe is no longer available"));
        return;
    }

    const auto parameterId = parameterForCurve(curveId);
    const auto contextualLayer =
        parameterId.has_value() ? contextualLayerForParameter(*parameterId) : std::nullopt;
    CompositionSelection next{.primary = target, .contextualLayer = contextualLayer};
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
    const auto result = commandStack_->execute(std::move(transaction));
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
    const auto result = commandStack_->execute(std::move(transaction));
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
    return executePositionCommand(position->id, currentTime_, document::Vec2d{x, y},
                                  QStringLiteral("Set Position"));
}

bool CompositionSession::executePositionCommand(const document::ParameterId parameterId,
                                                const core::RationalTime time,
                                                const document::Vec2d value,
                                                const QString& commandLabel) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto* position = current == nullptr ? nullptr : current->parameters().find(parameterId);
    if (position == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose a position"));
        return false;
    }

    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
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
                                                         time, value);
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

bool CompositionSession::deleteSelectedKeyframe() {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    if (keySelection == nullptr) {
        return false;
    }
    commands::Transaction transaction("Delete Keyframe", snapshot_.revision());
    transaction.emplace<commands::DeleteKeyframe>(compositionId_, keySelection->curveId,
                                                  keySelection->keyframeId);
    return execute(std::move(transaction));
}

bool CompositionSession::moveSelectedKeyframe(const core::RationalTime newTime) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    if (keySelection == nullptr) {
        return false;
    }
    const auto* current = composition();
    const auto* record =
        current == nullptr ? nullptr : current->animationCurves().find(keySelection->curveId);
    if (record == nullptr) {
        return false;
    }

    commands::Transaction transaction("Move Keyframe", snapshot_.revision());
    if (const auto* scalar = std::get_if<document::ScalarAnimationCurve>(record)) {
        const auto key = std::ranges::find(scalar->keyframes, keySelection->keyframeId,
                                           &document::ScalarKeyframe::id);
        if (key == scalar->keyframes.end()) {
            return false;
        }
        if (key->time == newTime) {
            // Zero effective change commits nothing (docs/architecture/animation-and-time.md's
            // zero-move precedent -- mirrors commitPositionInteraction()'s zero-move branch).
            return true;
        }
        transaction.emplace<commands::UpdateScalarKeyframe>(compositionId_, keySelection->curveId,
                                                            keySelection->keyframeId, newTime,
                                                            key->value, key->outgoingInterpolation);
    } else if (const auto* vec2 = std::get_if<document::Vec2AnimationCurve>(record)) {
        const auto key = std::ranges::find(vec2->keyframes, keySelection->keyframeId,
                                           &document::Vec2Keyframe::id);
        if (key == vec2->keyframes.end()) {
            return false;
        }
        if (key->time == newTime) {
            return true;
        }
        transaction.emplace<commands::UpdateVec2Keyframe>(compositionId_, keySelection->curveId,
                                                          keySelection->keyframeId, newTime,
                                                          key->value, key->outgoingInterpolation);
    } else {
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::positionInteractionActive() const noexcept {
    return positionInteraction_.has_value();
}

std::optional<runtime::SnapshotParameterOverride>
CompositionSession::positionInteractionOverride() const {
    if (!positionInteraction_.has_value()) {
        return std::nullopt;
    }
    return runtime::SnapshotParameterOverride{
        .sourceRevision = positionInteraction_->baseRevision,
        .parameterId = positionInteraction_->parameterId,
        .value = positionInteraction_->currentOverride,
    };
}

std::optional<PositionInteractionRejection>
CompositionSession::beginPositionInteraction(PositionInteractionMapping mapping) {
    Q_ASSERT(QThread::currentThread() == thread());
    // A stray second begin (should not happen given the Viewer is the sole caller) restarts state
    // cleanly rather than layering interactions.
    cancelPositionInteraction();

    const auto* layerId = std::get_if<document::LayerId>(&selection_.primary);
    if (layerId == nullptr) {
        return PositionInteractionRejection::NoLayerSelected;
    }
    const auto* position = parameterForSelection(document::kPositionParameterRole);
    if (position == nullptr) {
        return PositionInteractionRejection::NoResolvablePosition;
    }

    document::Vec2d baseValue{};
    if (const auto* constantSource =
            std::get_if<document::ConstantValueSource>(&position->source)) {
        const auto* value = std::get_if<document::Vec2d>(&constantSource->value);
        if (value == nullptr) {
            return PositionInteractionRejection::NoResolvablePosition;
        }
        baseValue = *value;
    } else if (const auto* animationSource =
                   std::get_if<document::AnimationCurveSource>(&position->source)) {
        // CompositionSession cannot sample runtime's exact rational interpolation, so an animated
        // position only resolves a base value when the playhead sits exactly on a key (docs/
        // architecture/animation-and-time.md; see PositionInteractionRejection::
        // AnimatedWithoutExactKey).
        const auto* current = composition();
        const auto* curve = current == nullptr
                                ? nullptr
                                : current->animationCurves().findVec2(animationSource->curveId);
        if (curve == nullptr) {
            return PositionInteractionRejection::NoResolvablePosition;
        }
        const auto exactKey = std::ranges::find_if(
            curve->keyframes, [this](const auto& key) { return key.time == currentTime_; });
        if (exactKey == curve->keyframes.end()) {
            return PositionInteractionRejection::AnimatedWithoutExactKey;
        }
        baseValue = exactKey->value;
    } else {
        return PositionInteractionRejection::DrivenParameter;
    }

    if (mapping.displayRect.isEmpty()) {
        return PositionInteractionRejection::EmptyMapping;
    }

    positionInteraction_ = PositionInteraction{.baseRevision = snapshot_.revision(),
                                               .parameterId = position->id,
                                               .layerId = *layerId,
                                               .time = currentTime_,
                                               .baseValue = baseValue,
                                               .currentOverride = baseValue,
                                               .mapping = mapping};
    emit positionInteractionChanged();
    return std::nullopt;
}

void CompositionSession::updatePositionInteraction(const double screenDx, const double screenDy) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!positionInteraction_.has_value()) {
        return;
    }

    const auto& mapping = positionInteraction_->mapping;
    const double displayWidth = mapping.displayRect.width();
    const double displayHeight = mapping.displayRect.height();
    if (displayWidth <= 0.0 || displayHeight <= 0.0) {
        cancelPositionInteraction();
        return;
    }

    // docs/architecture/animation-and-time.md: base value plus TOTAL gesture displacement, never a
    // chain of already-rounded intermediates.
    const double compositionWidth = static_cast<double>(mapping.compositionFormat.width());
    const double compositionHeight = static_cast<double>(mapping.compositionFormat.height());
    const double compositionDx = screenDx / displayWidth * compositionWidth;
    const double compositionDy = screenDy / displayHeight * compositionHeight;
    positionInteraction_->currentOverride =
        document::Vec2d{positionInteraction_->baseValue.x + compositionDx,
                        positionInteraction_->baseValue.y + compositionDy};
    emit positionInteractionChanged();
}

void CompositionSession::cancelPositionInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!positionInteraction_.has_value()) {
        return;
    }
    positionInteraction_.reset();
    emit positionInteractionChanged();
}

void CompositionSession::invalidatePositionInteraction() { cancelPositionInteraction(); }

bool CompositionSession::commitPositionInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!positionInteraction_.has_value()) {
        return false;
    }
    const PositionInteraction interaction = *positionInteraction_;
    positionInteraction_.reset();
    emit positionInteractionChanged();

    if (interaction.baseRevision != snapshot_.revision()) {
        // The invalidation hooks below should already have cancelled a stale interaction; treat
        // this defensively as a no-op rather than mutate against a revision the caller no longer
        // recognizes.
        return false;
    }
    if (interaction.currentOverride == interaction.baseValue) {
        // A zero move commits nothing (docs/architecture/animation-and-time.md).
        return true;
    }

    // Same command-selection decision as setSelectedPosition() (see executePositionCommand()),
    // targeted at the FROZEN parameter/time rather than re-deriving from live selection (docs/
    // architecture/animation-and-time.md: "On release, a constant position receives one
    // set-constant transaction. An animated position updates the exact-time key or inserts one.").
    return executePositionCommand(interaction.parameterId, interaction.time,
                                  interaction.currentOverride, QStringLiteral("Move Layer"));
}

bool CompositionSession::canUndo() const noexcept { return commandStack_->canUndo(); }

bool CompositionSession::canRedo() const noexcept { return commandStack_->canRedo(); }

QString CompositionSession::undoLabel() const {
    const auto label = commandStack_->undoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

QString CompositionSession::redoLabel() const {
    const auto label = commandStack_->redoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

bool CompositionSession::undo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_->undo());
}

bool CompositionSession::redo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_->redo());
}

bool CompositionSession::execute(commands::Transaction&& transaction) {
    return handleResult(commandStack_->execute(std::move(transaction)));
}

bool CompositionSession::handleResult(const commands::CommandResult& result) {
    const auto previousRevision = snapshot_.revision();
    snapshot_ = document_->snapshot();
    invalidatePositionInteractionOnStaleRevision();

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

bool CompositionSession::selectionExists(const CompositionSelection& selection) const {
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
    if (const auto* keySelection = std::get_if<KeyframeSelection>(&selection.primary)) {
        return keyframeSelectionExists(*keySelection);
    }
    const auto* parameterId = std::get_if<document::ParameterId>(&selection.primary);
    return parameterId != nullptr && current->parameters().find(*parameterId) != nullptr;
}

bool CompositionSession::keyframeSelectionExists(const KeyframeSelection& selection) const {
    const auto* current = composition();
    if (current == nullptr) {
        return false;
    }
    const auto* record = current->animationCurves().find(selection.curveId);
    if (record == nullptr) {
        return false;
    }
    return std::visit(
        [&](const auto& curve) {
            return std::ranges::any_of(
                curve.keyframes, [&](const auto& key) { return key.id == selection.keyframeId; });
        },
        *record);
}

std::optional<document::ParameterId>
CompositionSession::parameterForCurve(const document::AnimationCurveId curveId) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    for (const auto& parameter : current->parameters().records()) {
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
        if (source != nullptr && source->curveId == curveId) {
            return parameter.id;
        }
    }
    return std::nullopt;
}

std::optional<document::LayerId>
CompositionSession::contextualLayerForParameter(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    for (const auto& node : current->graph().nodes()) {
        const auto binding = std::ranges::find_if(node.parameters, [parameterId](const auto& item) {
            return item.parameterId == parameterId;
        });
        if (binding != node.parameters.end()) {
            return layerForNode(node.id);
        }
    }
    return std::nullopt;
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

void CompositionSession::invalidatePositionInteractionOnStaleRevision() {
    if (positionInteraction_.has_value() &&
        positionInteraction_->baseRevision != snapshot_.revision()) {
        cancelPositionInteraction();
    }
}

} // namespace bloom::ui
