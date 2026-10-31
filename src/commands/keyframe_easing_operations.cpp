#include "animation_curve_edits.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::commands {
namespace {

// A component curve is addressed by its index; a whole-value (scalar or Path) curve by nullopt.
using SequenceKey = std::pair<document::AnimationCurveId, std::optional<std::size_t>>;
using Selection = std::map<SequenceKey, std::set<document::KeyframeId>>;

[[nodiscard]] OperationResult invalidComposition(const document::CompositionId id) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition " + std::to_string(id.value()) +
                                         " does not exist");
}

[[nodiscard]] std::optional<std::size_t>
componentIndex(const document::AnimationCurveRecord& record,
               const document::AnimationComponent component) {
    return std::visit(
        [component](const auto& curve) -> std::optional<std::size_t> {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                return std::nullopt;
            } else {
                const auto* selected = curve.component(component);
                if (selected == nullptr)
                    return std::nullopt;
                return static_cast<std::size_t>(selected - curve.components.data());
            }
        },
        record);
}

// Resolves the addresses to concrete keys per sequence. A component-less vector or colour address
// names every component's key at the addressed key's time, as SetKeyframesInterpolation does.
[[nodiscard]] Selection select(const detail::CurveEdits& edits,
                               const std::vector<KeyframeAddress>& keys) {
    Selection selection;
    for (const auto& address : keys) {
        const auto& record = edits.at(address.curveId);
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                    selection[{address.curveId, std::nullopt}].insert(address.keyframeId);
                } else if (address.component.has_value()) {
                    if (const auto index = componentIndex(record, *address.component))
                        selection[{address.curveId, index}].insert(address.keyframeId);
                } else {
                    std::optional<core::RationalTime> time;
                    for (const auto& component : curve.components)
                        for (const auto& key : component.keyframes)
                            if (key.id == address.keyframeId)
                                time = key.time;
                    if (!time)
                        return;
                    for (std::size_t index = 0; index < curve.components.size(); ++index)
                        for (const auto& key : curve.components[index].keyframes)
                            if (key.time == *time)
                                selection[{address.curveId, index}].insert(key.id);
                }
            },
            record);
    }
    return selection;
}

// Calls `edit(keys, componentIndexOrNullopt)` for every key sequence of a staged curve.
template <typename Edit> void forEachSequence(document::AnimationCurveRecord& record, Edit edit) {
    std::visit(
        [&](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                edit(curve.keyframes, std::optional<std::size_t>{});
            } else {
                for (std::size_t index = 0; index < curve.components.size(); ++index)
                    edit(curve.components[index].keyframes, std::optional<std::size_t>{index});
            }
        },
        record);
}

template <typename Keyframe>
[[nodiscard]] double segmentChange(const Keyframe& left, const Keyframe& right) noexcept {
    // A Path key's handle offsets are fractions of the segment's 0 -> 1 progress.
    if constexpr (std::is_same_v<Keyframe, document::PathKeyframe>)
        return 1.0;
    else
        return right.value - left.value;
}

template <typename Keyframe>
void easeSequence(std::vector<Keyframe>& keys, const std::set<document::KeyframeId>& selected,
                  const EasyEase kind) {
    const auto original = keys;
    for (std::size_t index = 0; index + 1 < keys.size(); ++index) {
        const bool easeLeft = kind != EasyEase::In && selected.contains(keys[index].id);
        const bool easeRight = kind != EasyEase::Out && selected.contains(keys[index + 1].id);
        if (!easeLeft && !easeRight)
            continue;
        const bool wasBezier =
            original[index].outgoingInterpolation == document::KeyframeInterpolation::EaseInOut;
        const double third = segmentChange(keys[index], keys[index + 1]) / 3.0;
        keys[index].outgoingInterpolation = document::KeyframeInterpolation::EaseInOut;
        // Zero speed at 33.33% influence is exactly the default handle (1/3, 0).
        // Easy Ease writes explicit handles, so a key it shapes is Free: an Auto key would have
        // them recomputed and an Aligned key would drag its other side along.
        if (easeLeft || !wasBezier)
            keys[index].handleMode = document::KeyframeHandleMode::Free;
        if (easeRight || !wasBezier)
            keys[index + 1].handleMode = document::KeyframeHandleMode::Free;
        if (easeLeft)
            keys[index].outgoingHandle = document::KeyframeHandle{};
        else if (!wasBezier)
            keys[index].outgoingHandle = {document::kDefaultKeyframeHandleTime, third};
        if (easeRight)
            keys[index + 1].incomingHandle = document::KeyframeHandle{};
        else if (!wasBezier)
            keys[index + 1].incomingHandle = {document::kDefaultKeyframeHandleTime, -third};
    }
}

} // namespace

std::string_view SetKeyframesEasing::typeId() const noexcept {
    return "bloom.animation.set-keyframes-easing";
}
OperationResult SetKeyframesEasing::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (interpolation_ && !document::isValidKeyframeInterpolation(*interpolation_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Unsupported interpolation");
    if (edit_.easing && !document::isValidKeyframeEasingMode(*edit_.easing))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Unsupported ease mode");
    detail::CurveEdits edits;
    auto staged = detail::stageKeys(*composition, keys_, edits);
    if (staged.status == OperationStatus::Rejected)
        return staged;
    const auto selection = select(edits, keys_);
    bool valid = true;
    for (auto& [curveId, record] : edits) {
        forEachSequence(record, [&](auto& keys, const std::optional<std::size_t> component) {
            const auto found = selection.find({curveId, component});
            if (found == selection.end())
                return;
            for (auto& key : keys) {
                if (!found->second.contains(key.id))
                    continue;
                if (edit_.easing)
                    key.easing.easing = *edit_.easing;
                if (edit_.back)
                    key.easing.back = *edit_.back;
                if (edit_.amplitude)
                    key.easing.amplitude = *edit_.amplitude;
                if (edit_.period)
                    key.easing.period = *edit_.period;
                if (interpolation_)
                    key.outgoingInterpolation = *interpolation_;
                valid = valid && document::isValidKeyframeEasing(key.easing);
            }
        });
    }
    if (!valid)
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Easing parameters must be finite, with an amplitude of zero or more");
    return detail::publishCurves(*composition, std::move(edits));
}

std::string_view SetKeyframesHandleMode::typeId() const noexcept {
    return "bloom.animation.set-keyframes-handle-mode";
}
OperationResult SetKeyframesHandleMode::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (!document::isValidKeyframeHandleMode(mode_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Unsupported handle mode");
    detail::CurveEdits edits;
    auto staged = detail::stageKeys(*composition, keys_, edits);
    if (staged.status == OperationStatus::Rejected)
        return staged;
    const auto selection = select(edits, keys_);
    for (auto& [curveId, record] : edits) {
        forEachSequence(record, [&](auto& keys, const std::optional<std::size_t> component) {
            const auto found = selection.find({curveId, component});
            if (found == selection.end())
                return;
            for (auto& key : keys)
                if (found->second.contains(key.id))
                    key.handleMode = mode_;
        });
    }
    // publishCurves settles the staged curves: Auto keys get their handles in this same step.
    return detail::publishCurves(*composition, std::move(edits));
}

std::string_view ApplyEasyEase::typeId() const noexcept { return "bloom.animation.easy-ease"; }
OperationResult ApplyEasyEase::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (kind_ != EasyEase::Both && kind_ != EasyEase::In && kind_ != EasyEase::Out)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Unsupported Easy Ease kind");
    detail::CurveEdits edits;
    auto staged = detail::stageKeys(*composition, keys_, edits);
    if (staged.status == OperationStatus::Rejected)
        return staged;
    const auto selection = select(edits, keys_);
    for (auto& [curveId, record] : edits) {
        forEachSequence(record, [&](auto& keys, const std::optional<std::size_t> component) {
            const auto found = selection.find({curveId, component});
            if (found != selection.end())
                easeSequence(keys, found->second, kind_);
        });
    }
    return detail::publishCurves(*composition, std::move(edits));
}

} // namespace bloom::commands
