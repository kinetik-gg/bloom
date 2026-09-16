#include "timeline_keyframe_time.hpp"
#include <QThread>
#include <algorithm>
#include <array>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <type_traits>
#include <vector>

namespace bloom::ui {
namespace {
bool editableParameter(const CompositionSession& session, document::ParameterId parameter) {
    const auto* composition = session.composition();
    if (!composition || !composition->parameters().find(parameter))
        return false;
    for (const auto& node : composition->graph().nodes())
        for (const auto& binding : node.parameters) {
            if (binding.parameterId != parameter)
                continue;
            const auto layerId = session.layerForNode(node.id);
            const auto* layer = layerId ? composition->graph().findLayer(*layerId) : nullptr;
            if (layer && layer->locked)
                return false;
        }
    return true;
}
} // namespace
void CompositionSession::selectKeyframe(document::AnimationCurveId curve, document::KeyframeId key,
                                        bool extend) {
    auto keys = extend ? selection_.keyframes : std::vector<KeyframeSelection>{};
    const KeyframeSelection target{curve, key, std::nullopt};
    if (!keyframeSelectionExists(target)) {
        reportUnavailable(tr("The selected keyframe is no longer available"));
        return;
    }
    std::erase(keys, target);
    keys.push_back(target);
    selectKeyframes(keys);
}
void CompositionSession::selectKeyframe(document::AnimationCurveId curve,
                                        document::AnimationComponent component,
                                        document::KeyframeId key, bool extend) {
    auto keys = extend ? selection_.keyframes : std::vector<KeyframeSelection>{};
    const KeyframeSelection target{curve, key, component};
    if (!keyframeSelectionExists(target)) {
        reportUnavailable(tr("The selected component keyframe is no longer available"));
        return;
    }
    std::erase(keys, target);
    keys.push_back(target);
    selectKeyframes(keys);
}
void CompositionSession::selectKeyframes(const std::vector<KeyframeSelection>& keys) {
    Q_ASSERT(QThread::currentThread() == thread());
    std::vector<KeyframeSelection> valid;
    for (const auto& key : keys) {
        if (!keyframeSelectionExists(key))
            continue;
        if (std::ranges::find(valid, key) == valid.end())
            valid.push_back(key);
    }
    if (valid.empty()) {
        clearSelection();
        return;
    }
    const auto primary = valid.back();
    const auto parameter = parameterForCurve(primary.curveId);
    CompositionSelection next{.primary = primary,
                              .contextualLayer = parameter ? contextualLayerForParameter(*parameter)
                                                           : std::nullopt,
                              .keyframes = std::move(valid)};
    if (selection_ != next || !selectedNodes_.empty()) {
        selection_ = std::move(next);
        selectedNodes_.clear();
        Q_EMIT selectionChanged();
    }
}
std::vector<commands::KeyframePaste> CompositionSession::selectedKeyframeData() const {
    std::vector<commands::KeyframePaste> result;
    const auto* current = composition();
    if (!current)
        return result;
    for (const auto& address : selection_.keyframes) {
        const auto parameter = parameterForCurve(address.curveId);
        const auto* record = current->animationCurves().find(address.curveId);
        if (!parameter || !record)
            continue;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if (address.component.has_value()) {
                    const auto* component = [&]() {
                        if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>)
                            return static_cast<const document::ComponentAnimationCurve*>(nullptr);
                        else
                            return curve.component(*address.component);
                    }();
                    if (component != nullptr) {
                        for (const auto& key : component->keyframes)
                            if (key.id == address.keyframeId)
                                result.push_back({*parameter, key.time, key.value,
                                                  key.outgoingInterpolation, address.component,
                                                  key.outgoingHandle, key.incomingHandle});
                    }
                    return;
                }
                for (const auto& key : curve.keyframes)
                    if (key.id == address.keyframeId) {
                        if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                            result.push_back({*parameter, key.time, key.value,
                                              key.outgoingInterpolation, std::nullopt,
                                              key.outgoingHandle, key.incomingHandle});
                        } else {
                            result.push_back(
                                {*parameter, key.time, key.value, key.outgoingInterpolation});
                        }
                    }
            },
            *record);
    }
    return result;
}
bool CompositionSession::moveKeyframes(std::vector<commands::KeyframeMove> keys,
                                       document::Revision revision) {
    for (const auto& key : keys) {
        const auto parameter = parameterForCurve(key.key.curveId);
        if (!parameter || !editableParameter(*this, *parameter)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    }
    commands::Transaction transaction("Move Keyframes", revision);
    transaction.emplace<commands::MoveKeyframes>(compositionId_, std::move(keys));
    return execute(std::move(transaction));
}
bool CompositionSession::moveKeyframesAndValues(std::vector<commands::KeyframeMove> moves,
                                                std::vector<commands::KeyframeValueEdit> values,
                                                document::Revision revision) {
    if (moves.empty() && values.empty())
        return false;
    const auto editable = [&](const document::AnimationCurveId curveId) {
        const auto parameter = parameterForCurve(curveId);
        return parameter.has_value() && editableParameter(*this, *parameter);
    };
    for (const auto& move : moves)
        if (!editable(move.key.curveId)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    for (const auto& value : values)
        if (!editable(value.key.curveId)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    commands::Transaction transaction("Move Keyframes", revision);
    if (!moves.empty())
        transaction.emplace<commands::MoveKeyframes>(compositionId_, std::move(moves));
    if (!values.empty())
        transaction.emplace<commands::SetKeyframeValues>(compositionId_, std::move(values));
    return execute(std::move(transaction));
}
bool CompositionSession::setKeyframeHandles(std::vector<commands::KeyframeHandleEdit> edits,
                                            document::Revision revision) {
    if (edits.empty())
        return false;
    for (const auto& edit : edits) {
        const auto parameter = parameterForCurve(edit.key.curveId);
        if (!parameter || !editableParameter(*this, *parameter)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    }
    commands::Transaction transaction("Set Keyframe Handles", revision);
    transaction.emplace<commands::SetKeyframeHandles>(compositionId_, std::move(edits));
    return execute(std::move(transaction));
}
bool CompositionSession::resetSelectedKeyframeHandles() {
    const auto* current = composition();
    if (!current)
        return false;
    std::vector<commands::KeyframeHandleEdit> edits;
    for (const auto& address : selection_.keyframes) {
        const auto* record = current->animationCurves().find(address.curveId);
        if (record == nullptr)
            continue;
        const auto* keyframes = std::visit(
            [&](const auto& curve) -> const std::vector<document::ScalarKeyframe>* {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    return address.component.has_value() ? nullptr : &curve.keyframes;
                } else {
                    if (!address.component.has_value())
                        return nullptr;
                    const auto* component = curve.component(*address.component);
                    return component == nullptr ? nullptr : &component->keyframes;
                }
            },
            *record);
        if (keyframes == nullptr)
            continue;
        const auto at =
            std::ranges::find(*keyframes, address.keyframeId, &document::ScalarKeyframe::id);
        if (at == keyframes->end())
            continue;
        const auto index = static_cast<std::size_t>(at - keyframes->begin());
        commands::KeyframeHandleEdit edit{{address.curveId, address.keyframeId, address.component}};
        if (index + 1 < keyframes->size())
            edit.outgoing = document::KeyframeHandle{};
        if (index > 0)
            edit.incoming = document::KeyframeHandle{};
        if (edit.outgoing.has_value() || edit.incoming.has_value())
            edits.push_back(edit);
    }
    if (edits.empty())
        return false;
    return setKeyframeHandles(std::move(edits), snapshot_.revision());
}
bool CompositionSession::pasteKeyframes(const std::vector<commands::KeyframePaste>& keys,
                                        document::Revision revision) {
    for (const auto& key : keys)
        if (!editableParameter(*this, key.parameterId)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    commands::Transaction transaction("Paste Keyframes", revision);
    transaction.emplace<commands::PasteKeyframes>(compositionId_, keys);
    if (!execute(std::move(transaction)))
        return false;
    std::vector<KeyframeSelection> selection;
    const auto* current = composition();
    if (!current)
        return true;
    for (const auto& paste : keys) {
        const auto* parameter = current->parameters().find(paste.parameterId);
        const auto* source =
            parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
        const auto* record = source ? current->animationCurves().find(source->curveId) : nullptr;
        if (!record)
            continue;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if (paste.component.has_value()) {
                    if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                        const auto* component = curve.component(*paste.component);
                        if (component != nullptr) {
                            for (const auto& key : component->keyframes)
                                if (key.time == paste.time)
                                    selection.push_back({curve.id, key.id, paste.component});
                        }
                    }
                    return;
                }
                for (const auto& key : curve.keyframes)
                    if (key.time == paste.time)
                        selection.push_back({curve.id, key.id, std::nullopt});
                if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    if (curve.keyframes.empty()) {
                        for (std::size_t index = 0; index < curve.components.size(); ++index) {
                            for (const auto& key : curve.components[index].keyframes) {
                                if (key.time != paste.time)
                                    continue;
                                const auto component = [&] {
                                    if constexpr (std::is_same_v<Curve,
                                                                 document::Vec2AnimationCurve>)
                                        return std::array{document::AnimationComponent::X,
                                                          document::AnimationComponent::Y}[index];
                                    else if constexpr (std::is_same_v<Curve,
                                                                      document::Vec3AnimationCurve>)
                                        return std::array{document::AnimationComponent::X,
                                                          document::AnimationComponent::Y,
                                                          document::AnimationComponent::Z}[index];
                                    else
                                        return std::array{
                                            document::AnimationComponent::Red,
                                            document::AnimationComponent::Green,
                                            document::AnimationComponent::Blue,
                                            document::AnimationComponent::Alpha}[index];
                                }();
                                selection.push_back({curve.id, key.id, component});
                            }
                        }
                    }
                }
            },
            *record);
    }
    selectKeyframes(selection);
    return true;
}
bool CompositionSession::deleteSelectedKeyframes() {
    std::vector<commands::KeyframeAddress> keys;
    keys.reserve(selection_.keyframes.size());
    for (const auto& key : selection_.keyframes)
        keys.push_back({key.curveId, key.keyframeId, key.component});
    if (keys.empty())
        return false;
    for (const auto& key : keys) {
        const auto parameter = parameterForCurve(key.curveId);
        if (!parameter || !editableParameter(*this, *parameter)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    }
    commands::Transaction transaction("Delete Keyframes", snapshot_.revision());
    transaction.emplace<commands::DeleteKeyframes>(compositionId_, std::move(keys));
    return execute(std::move(transaction));
}
bool CompositionSession::setSelectedKeyframesInterpolation(
    document::KeyframeInterpolation interpolation) {
    std::vector<commands::KeyframeAddress> keys;
    keys.reserve(selection_.keyframes.size());
    for (const auto& key : selection_.keyframes)
        keys.push_back({key.curveId, key.keyframeId, key.component});
    if (keys.empty())
        return false;
    for (const auto& key : keys) {
        const auto parameter = parameterForCurve(key.curveId);
        if (!parameter || !editableParameter(*this, *parameter)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
    }
    commands::Transaction transaction("Set Keyframes Interpolation", snapshot_.revision());
    transaction.emplace<commands::SetKeyframesInterpolation>(compositionId_, std::move(keys),
                                                             interpolation);
    return execute(std::move(transaction));
}
void CompositionSession::copySelectedKeyframes() { keyframeClipboard_ = selectedKeyframeData(); }
bool CompositionSession::pasteCopiedKeyframes() {
    const auto* current = composition();
    if (!current || keyframeClipboard_.empty())
        return false;
    auto keys = keyframeClipboard_;
    const auto first = std::ranges::min_element(keys, {}, &commands::KeyframePaste::time)->time;
    const auto offset = keyTimeDifference(currentTime_, first);
    if (!offset)
        return false;
    for (auto& key : keys) {
        const auto time = offsetKeyTime(key.time, *offset);
        if (!time || *time < core::RationalTime{} || *time >= current->duration()) {
            reportUnavailable(tr("Pasted keys would exceed the composition"));
            return false;
        }
        key.time = *time;
    }
    return pasteKeyframes(keys, snapshot_.revision());
}
} // namespace bloom::ui
