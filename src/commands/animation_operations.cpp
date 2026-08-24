#include <bloom/commands/animation_operations.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <utility>

namespace bloom::commands {
namespace {

OperationResult invalidComposition(const document::CompositionId compositionId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition " + std::to_string(compositionId.value()) +
                                         " does not exist");
}

OperationResult invalidParameter(const document::ParameterId parameterId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Parameter " + std::to_string(parameterId.value()) +
                                         " does not exist");
}

OperationResult invalidCurve(const document::AnimationCurveId curveId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Animation curve " + std::to_string(curveId.value()) +
                                         " does not exist");
}

OperationResult invalidKeyframe(const document::KeyframeId keyframeId) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Keyframe " + std::to_string(keyframeId.value()) +
                                         " does not exist on the target curve");
}

OperationResult exhaustedIds() {
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "Document animation ID space is exhausted");
}

[[nodiscard]] std::vector<OperationOutput> keyframeOutput(const document::KeyframeId id) {
    return {{std::string(kKeyframeOutput), DurableObjectId{id}}};
}

[[nodiscard]] bool
validInterpolation(const document::KeyframeInterpolation interpolation) noexcept {
    return interpolation == document::KeyframeInterpolation::Hold ||
           interpolation == document::KeyframeInterpolation::Linear;
}

template <typename Value> [[nodiscard]] bool finiteValue(const Value& value) noexcept {
    if constexpr (std::is_same_v<Value, double>) {
        return std::isfinite(value);
    } else {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }
}

[[nodiscard]] const document::ParameterRecord*
curveOwner(const document::Composition& composition,
           const document::AnimationCurveId curveId) noexcept {
    for (const auto& parameter : composition.parameters().records()) {
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
        if (source != nullptr && source->curveId == curveId) {
            return &parameter;
        }
    }
    return nullptr;
}

template <typename Curve>
[[nodiscard]] const Curve* findCurve(const document::AnimationCurveStore& store,
                                     const document::AnimationCurveId curveId) noexcept {
    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
        return store.findScalar(curveId);
    } else {
        return store.findVec2(curveId);
    }
}

template <typename Curve>
[[nodiscard]] bool curveSchemaMatches(const document::ParameterRecord& owner) noexcept {
    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
        return owner.schemaKey == document::kOpacityParameterSchemaKey;
    } else {
        return owner.schemaKey == document::kPositionParameterSchemaKey;
    }
}

template <typename Curve, typename Value>
[[nodiscard]] bool validValueForCurve(const document::Composition& composition,
                                      const document::AnimationCurveId curveId,
                                      const Value& value) noexcept {
    const auto* owner = curveOwner(composition, curveId);
    if (owner == nullptr || !curveSchemaMatches<Curve>(*owner) || !finiteValue(value)) {
        return false;
    }
    if constexpr (std::is_same_v<Value, double>) {
        return value >= 0.0 && value <= 1.0;
    }
    return true;
}

template <typename Curve, typename Keyframe>
OperationResult insertKeyframe(document::Draft& draft, const document::CompositionId compositionId,
                               const document::AnimationCurveId curveId, Keyframe keyframe) {
    auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr) {
        return invalidComposition(compositionId);
    }
    if (composition->animationCurves().find(curveId) == nullptr) {
        return invalidCurve(curveId);
    }
    const auto* curve = findCurve<Curve>(composition->animationCurves(), curveId);
    if (curve == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation curve has the wrong value kind");
    }
    if (!validValueForCurve<Curve>(*composition, curveId, keyframe.value) ||
        !validInterpolation(keyframe.outgoingInterpolation)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe value or interpolation is invalid");
    }
    if (std::ranges::find(curve->keyframes, keyframe.time, &Keyframe::time) !=
        curve->keyframes.end()) {
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "A keyframe already exists at the exact time");
    }

    const auto keyframeId = draft.ids().allocateKeyframe();
    if (!keyframeId.has_value()) {
        return exhaustedIds();
    }
    keyframe.id = *keyframeId;
    if (!composition->animationCurves().insertKeyframe(curveId, keyframe)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe could not be inserted");
    }
    return OperationResult::applied(keyframeOutput(*keyframeId));
}

template <typename Curve, typename Keyframe>
OperationResult updateKeyframe(document::Draft& draft, const document::CompositionId compositionId,
                               const document::AnimationCurveId curveId, Keyframe keyframe) {
    auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr) {
        return invalidComposition(compositionId);
    }
    if (composition->animationCurves().find(curveId) == nullptr) {
        return invalidCurve(curveId);
    }
    const auto* curve = findCurve<Curve>(composition->animationCurves(), curveId);
    if (curve == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation curve has the wrong value kind");
    }
    if (std::ranges::find(curve->keyframes, keyframe.id, &Keyframe::id) == curve->keyframes.end()) {
        return invalidKeyframe(keyframe.id);
    }
    if (!validValueForCurve<Curve>(*composition, curveId, keyframe.value) ||
        !validInterpolation(keyframe.outgoingInterpolation)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe value or interpolation is invalid");
    }

    const Curve before = *curve;
    if (!composition->animationCurves().updateKeyframe(curveId, keyframe)) {
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "Keyframe time is occupied by another key");
    }
    const auto* after = findCurve<Curve>(composition->animationCurves(), curveId);
    if (after != nullptr && *after == before) {
        return OperationResult::noChange(keyframeOutput(keyframe.id));
    }
    return OperationResult::applied(keyframeOutput(keyframe.id));
}

template <typename Curve, typename Keyframe, typename Value>
OperationResult setKeyframeAtTime(document::Draft& draft,
                                  const document::CompositionId compositionId,
                                  const document::AnimationCurveId curveId,
                                  const core::RationalTime time, const Value& value) {
    auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr) {
        return invalidComposition(compositionId);
    }
    if (composition->animationCurves().find(curveId) == nullptr) {
        return invalidCurve(curveId);
    }
    const auto* curve = findCurve<Curve>(composition->animationCurves(), curveId);
    if (curve == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation curve has the wrong value kind");
    }
    if (!validValueForCurve<Curve>(*composition, curveId, value)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe value is invalid for the curve schema");
    }

    const auto existing = std::ranges::find(curve->keyframes, time, &Keyframe::time);
    if (existing != curve->keyframes.end()) {
        if (existing->value == value) {
            return OperationResult::noChange(keyframeOutput(existing->id));
        }
        const auto existingId = existing->id;
        auto updated = *existing;
        updated.value = value;
        if (!composition->animationCurves().updateKeyframe(curveId, updated)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Existing keyframe could not be updated");
        }
        return OperationResult::applied(keyframeOutput(existingId));
    }

    const auto keyframeId = draft.ids().allocateKeyframe();
    if (!keyframeId.has_value()) {
        return exhaustedIds();
    }
    if (!composition->animationCurves().insertKeyframe(
            curveId, Keyframe{*keyframeId, time, value, document::KeyframeInterpolation::Linear})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe could not be inserted");
    }
    return OperationResult::applied(keyframeOutput(*keyframeId));
}

} // namespace

std::string_view CreateAnimationForParameter::typeId() const noexcept {
    return "bloom.animation.create-for-parameter";
}

OperationResult CreateAnimationForParameter::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return invalidParameter(parameterId_);
    }
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (constant == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter must have a constant source before animation");
    }

    std::variant<double, document::Vec2d> initialValue;
    if (parameter->schemaKey == document::kOpacityParameterSchemaKey) {
        const auto* value = std::get_if<double>(&constant->value);
        if (value == nullptr || !std::isfinite(*value) || *value < 0.0 || *value > 1.0) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Opacity constant is invalid");
        }
        initialValue = *value;
    } else if (parameter->schemaKey == document::kPositionParameterSchemaKey) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        if (value == nullptr || !finiteValue(*value)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Position constant is invalid");
        }
        initialValue = *value;
    } else {
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Parameter schema does not support animation");
    }

    const auto curveId = draft.ids().allocateAnimationCurve();
    const auto keyframeId = draft.ids().allocateKeyframe();
    if (!curveId.has_value() || !keyframeId.has_value()) {
        return exhaustedIds();
    }

    bool inserted = false;
    if (const auto* value = std::get_if<double>(&initialValue)) {
        inserted = composition->animationCurves().insert(document::ScalarAnimationCurve{
            *curveId,
            {{*keyframeId, initialTime_, *value, document::KeyframeInterpolation::Linear}}});
    } else {
        const auto vectorValue = std::get<document::Vec2d>(initialValue);
        inserted = composition->animationCurves().insert(document::Vec2AnimationCurve{
            *curveId,
            {{*keyframeId, initialTime_, vectorValue, document::KeyframeInterpolation::Linear}}});
    }

    if (!inserted || !composition->parameters().setSource(
                         parameterId_, document::AnimationCurveSource{*curveId})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation could not be attached to the parameter");
    }
    return OperationResult::applied({
        {std::string(kAnimationCurveOutput), DurableObjectId{*curveId}},
        {std::string(kKeyframeOutput), DurableObjectId{*keyframeId}},
    });
}

std::string_view InsertScalarKeyframe::typeId() const noexcept {
    return "bloom.animation.insert-scalar-keyframe";
}

OperationResult InsertScalarKeyframe::apply(document::Draft& draft) const {
    return insertKeyframe<document::ScalarAnimationCurve>(
        draft, compositionId_, curveId_,
        document::ScalarKeyframe{{}, time_, value_, outgoingInterpolation_});
}

std::string_view InsertVec2Keyframe::typeId() const noexcept {
    return "bloom.animation.insert-vec2-keyframe";
}

OperationResult InsertVec2Keyframe::apply(document::Draft& draft) const {
    return insertKeyframe<document::Vec2AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec2Keyframe{{}, time_, value_, outgoingInterpolation_});
}

std::string_view UpdateScalarKeyframe::typeId() const noexcept {
    return "bloom.animation.update-scalar-keyframe";
}

OperationResult UpdateScalarKeyframe::apply(document::Draft& draft) const {
    return updateKeyframe<document::ScalarAnimationCurve>(
        draft, compositionId_, curveId_,
        document::ScalarKeyframe{keyframeId_, time_, value_, outgoingInterpolation_});
}

std::string_view UpdateVec2Keyframe::typeId() const noexcept {
    return "bloom.animation.update-vec2-keyframe";
}

OperationResult UpdateVec2Keyframe::apply(document::Draft& draft) const {
    return updateKeyframe<document::Vec2AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec2Keyframe{keyframeId_, time_, value_, outgoingInterpolation_});
}

std::string_view SetKeyframeAtTime::typeId() const noexcept {
    return "bloom.animation.set-keyframe-at-time";
}

OperationResult SetKeyframeAtTime::apply(document::Draft& draft) const {
    if (const auto* scalar = std::get_if<double>(&value_)) {
        return setKeyframeAtTime<document::ScalarAnimationCurve, document::ScalarKeyframe>(
            draft, compositionId_, curveId_, time_, *scalar);
    }
    return setKeyframeAtTime<document::Vec2AnimationCurve, document::Vec2Keyframe>(
        draft, compositionId_, curveId_, time_, std::get<document::Vec2d>(value_));
}

std::string_view DeleteKeyframe::typeId() const noexcept {
    return "bloom.animation.delete-keyframe";
}

OperationResult DeleteKeyframe::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    const auto* record = composition->animationCurves().find(curveId_);
    if (record == nullptr) {
        return invalidCurve(curveId_);
    }
    const bool contains = std::visit(
        [&](const auto& curve) {
            return std::ranges::any_of(
                curve.keyframes, [&](const auto& keyframe) { return keyframe.id == keyframeId_; });
        },
        *record);
    if (!contains) {
        return invalidKeyframe(keyframeId_);
    }
    const bool isLast =
        std::visit([](const auto& curve) { return curve.keyframes.size() == 1; }, *record);
    if (isLast) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The final keyframe cannot be deleted");
    }
    if (!composition->animationCurves().eraseKeyframe(curveId_, keyframeId_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe could not be deleted");
    }
    return OperationResult::applied(keyframeOutput(keyframeId_));
}

std::string_view ConvertAnimationToConstant::typeId() const noexcept {
    return "bloom.animation.convert-to-constant";
}

OperationResult ConvertAnimationToConstant::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return invalidParameter(parameterId_);
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter does not have an animation source");
    }

    document::ParameterValue constantValue;
    if (const auto* scalar = std::get_if<double>(&value_)) {
        if (parameter->schemaKey != document::kOpacityParameterSchemaKey ||
            composition->animationCurves().findScalar(source->curveId) == nullptr ||
            !std::isfinite(*scalar) || *scalar < 0.0 || *scalar > 1.0) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match scalar animation");
        }
        constantValue = *scalar;
    } else {
        const auto vector = std::get<document::Vec2d>(value_);
        if (parameter->schemaKey != document::kPositionParameterSchemaKey ||
            composition->animationCurves().findVec2(source->curveId) == nullptr ||
            !finiteValue(vector)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match Vec2 animation");
        }
        constantValue = vector;
    }

    const auto curveId = source->curveId;
    if (!composition->parameters().setSource(
            parameterId_, document::ConstantValueSource{std::move(constantValue)}) ||
        !composition->animationCurves().erase(curveId)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation could not be converted to a constant");
    }
    return OperationResult::applied();
}

} // namespace bloom::commands
