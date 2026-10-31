#include "animation_components.hpp"
#include "timeline_key_time.hpp"
#include "timeline_keyframe_time.hpp"
#include <QThread>
#include <algorithm>
#include <array>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/layer_masks.hpp>
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
    for (const auto& layer : composition->graph().layerOutputs())
        for (const auto& mask : layer.masks)
            for (const auto& [owned, schemaKey] : document::layerMaskParameters(mask)) {
                (void)schemaKey;
                if (owned == parameter && layer.locked)
                    return false;
            }
    return true;
}
} // namespace
void CompositionSession::selectKeyframe(document::AnimationCurveId curve, document::KeyframeId key,
                                        bool extend) {
    auto keys = extend ? selection_.keyframes : std::vector<KeyframeSelection>{};
    // A vector or colour key is addressed by {curve, key, component}: this overload is the
    // convenience that does not make its caller name the component, not a whole-value selection.
    const KeyframeSelection target{curve, key, componentForKeyframe(curve, key)};
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
                        if constexpr (document::kWholeValueAnimationCurve<Curve>)
                            return static_cast<const document::ComponentAnimationCurve*>(nullptr);
                        else
                            return curve.component(*address.component);
                    }();
                    if (component != nullptr) {
                        for (const auto& key : component->keyframes)
                            if (key.id == address.keyframeId)
                                result.push_back({*parameter, key.time, key.value,
                                                  key.outgoingInterpolation, address.component,
                                                  key.outgoingHandle, key.incomingHandle,
                                                  key.easing});
                    }
                    return;
                }
                if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                    for (const auto& key : curve.keyframes)
                        if (key.id == address.keyframeId)
                            result.push_back({*parameter, key.time, key.value,
                                              key.outgoingInterpolation, std::nullopt,
                                              key.outgoingHandle, key.incomingHandle,
                                              key.easing});
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
bool CompositionSession::easeSelectedKeyframes(const bool easeIn, const bool easeOut) {
    if (!composition() || (!easeIn && !easeOut) || selection_.keyframes.empty())
        return false;
    std::vector<commands::KeyframeAddress> keys;
    keys.reserve(selection_.keyframes.size());
    for (const auto& key : selection_.keyframes) {
        const auto parameter = parameterForCurve(key.curveId);
        if (!parameter || !editableParameter(*this, *parameter)) {
            reportUnavailable(tr("A keyframe target is missing or locked"));
            return false;
        }
        keys.push_back({key.curveId, key.keyframeId, key.component});
    }
    const auto kind = easeIn && easeOut ? commands::EasyEase::Both
                      : easeIn          ? commands::EasyEase::In
                                        : commands::EasyEase::Out;
    const char* label = easeIn && easeOut ? "Easy Ease" : easeIn ? "Easy Ease In" : "Easy Ease Out";
    commands::Transaction transaction(label, snapshot_.revision());
    transaction.emplace<commands::ApplyEasyEase>(compositionId_, std::move(keys), kind);
    return execute(std::move(transaction));
}
bool CompositionSession::previewKeyframeTransaction(const commands::Transaction& transaction) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr || transaction.empty() || valueEdit_.has_value()) {
        clearKeyframePreview();
        return false;
    }
    // The same operations the release will execute, on a scratch draft that no document owns and
    // that therefore can never be committed (#357's probe pattern).
    auto draft = document::Draft::isolatedFrom(snapshot_);
    for (const auto& operation : transaction.operations())
        if (operation->apply(draft).status == commands::OperationStatus::Rejected) {
            clearKeyframePreview();
            return false;
        }
    const auto* previewed = draft.valid() ? draft.project().findComposition(compositionId_)
                                          : nullptr;
    if (previewed == nullptr) {
        clearKeyframePreview();
        return false;
    }
    std::vector<document::AnimationCurveId> changed;
    for (const auto& record : previewed->animationCurves().records()) {
        const auto id = std::visit([](const auto& curve) { return curve.id; }, record);
        const auto* live = current->animationCurves().find(id);
        if (live != nullptr && *live == record)
            continue;
        const auto parameter = parameterForCurve(id);
        if (!parameter || !editableParameter(*this, *parameter)) {
            clearKeyframePreview();
            return false;
        }
        changed.push_back(id);
    }
    // The Viewer's half: one override per previewed parameter at the playhead, at most the eight a
    // compile request admits. A wider drag still previews every curve on the canvas; only the
    // Viewer waits for the release.
    std::vector<runtime::SnapshotParameterOverride> overrides;
    constexpr std::size_t kOverrideAdmission = 8;
    for (const auto curveId : changed) {
        const auto parameterId = parameterForCurve(curveId);
        const auto* parameter = parameterId ? current->parameters().find(*parameterId) : nullptr;
        if (parameter == nullptr)
            continue;
        const auto sample = sampleParameterValueIn(previewed->animationCurves(), *parameter,
                                                   keyTimeAtPlayhead(parameter->id));
        if (!sample.has_value())
            continue;
        overrides.push_back(
            {snapshot_.revision(), parameter->id,
             std::visit([](const auto& value) -> decltype(runtime::SnapshotParameterOverride::value) {
                 return value;
             },
                        *sample)});
    }
    if (overrides.size() > kOverrideAdmission)
        overrides.clear();
    keyframePreview_.reset();
    keyframePreview_.emplace(KeyframePreview{snapshot_.revision(), currentTime_, std::move(draft),
                                             std::move(changed), std::move(overrides)});
    ++keyframePreviewGeneration_;
    emit liveValueChanged();
    return true;
}
void CompositionSession::clearKeyframePreview() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!keyframePreview_)
        return;
    keyframePreview_.reset();
    ++keyframePreviewGeneration_;
    emit liveValueChanged();
}
const document::AnimationCurveStore* CompositionSession::keyframePreviewCurves() const noexcept {
    if (!keyframePreview_)
        return nullptr;
    const auto* previewed = keyframePreview_->draft.project().findComposition(compositionId_);
    return previewed != nullptr ? &previewed->animationCurves() : nullptr;
}
const document::AnimationCurveRecord*
CompositionSession::keyframePreviewCurve(const document::AnimationCurveId curveId) const noexcept {
    if (!keyframePreview_ || std::ranges::find(keyframePreview_->curves, curveId) ==
                                 keyframePreview_->curves.end())
        return nullptr;
    const auto* curves = keyframePreviewCurves();
    return curves != nullptr ? curves->find(curveId) : nullptr;
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
                    if constexpr (!document::kWholeValueAnimationCurve<Curve>) {
                        const auto* component = curve.component(*paste.component);
                        if (component != nullptr) {
                            for (const auto& key : component->keyframes)
                                if (key.time == paste.time)
                                    selection.push_back({curve.id, key.id, paste.component});
                        }
                    }
                    return;
                }
                if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                    for (const auto& key : curve.keyframes)
                        if (key.time == paste.time)
                            selection.push_back({curve.id, key.id, std::nullopt});
                } else {
                    // A whole-value paste lands one key per component at the same exact time, so
                    // the selection it leaves behind names every one of them.
                    constexpr auto names = animationComponentsOf<Curve>();
                    for (std::size_t index = 0; index < curve.components.size(); ++index)
                        for (const auto& key : curve.components[index].keyframes)
                            if (key.time == paste.time)
                                selection.push_back({curve.id, key.id, names[index]});
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
    // Clipboard times are key-clock times (ADR 0024). The anchor is the playhead, a composition
    // time: each key is projected to composition time, shifted by the anchor offset, bounds
    // checked there, and converted back to the clock of the parameter it is pasted onto.
    std::optional<core::RationalTime> firstPlacement;
    for (const auto& key : keys) {
        const auto placement =
            compositionTimeOfKey(keyClockForParameter(key.parameterId), key.time);
        if (!placement)
            return false;
        if (!firstPlacement || *placement < *firstPlacement)
            firstPlacement = placement;
    }
    const auto offset = keyTimeDifference(currentTime_, *firstPlacement);
    if (!offset)
        return false;
    for (auto& key : keys) {
        const auto clock = keyClockForParameter(key.parameterId);
        const auto placement = compositionTimeOfKey(clock, key.time);
        const auto time = placement ? offsetKeyTime(*placement, *offset) : std::nullopt;
        if (!time || *time < core::RationalTime{} || *time >= current->duration()) {
            reportUnavailable(tr("Pasted keys would exceed the composition"));
            return false;
        }
        const auto stored = keyTimeOfCompositionTime(clock, *time);
        if (!stored)
            return false;
        key.time = *stored;
    }
    return pasteKeyframes(keys, snapshot_.revision());
}
} // namespace bloom::ui
