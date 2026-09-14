#include <bloom/commands/animation_operations.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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
           interpolation == document::KeyframeInterpolation::Linear ||
           interpolation == document::KeyframeInterpolation::EaseInOut;
}

template <typename Value> [[nodiscard]] bool finiteValue(const Value& value) noexcept {
    if constexpr (std::is_same_v<Value, double>) {
        return std::isfinite(value);
    } else if constexpr (std::is_same_v<Value, core::Color4d>) {
        // The authoring-colour contract, exactly as a constant colour satisfies it.
        return value.isValid();
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
    } else if constexpr (std::is_same_v<Curve, document::Color4AnimationCurve>) {
        return store.findColor4(curveId);
    } else {
        return store.findVec2(curveId);
    }
}

template <typename Curve>
[[nodiscard]] bool curveSchemaMatches(const document::ParameterRecord& owner) noexcept {
    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
        return document::isScalarAnimatableSchemaKey(owner.schemaKey);
    } else if constexpr (std::is_same_v<Curve, document::Color4AnimationCurve>) {
        return document::isColor4AnimatableSchemaKey(owner.schemaKey);
    } else {
        return document::isVec2AnimatableSchemaKey(owner.schemaKey);
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
        // A scalar DOMAIN is the schema's, not every scalar curve's: opacity is confined to [0, 1]
        // and text size to (0, kMaximumTextSizePixels], while a rotation key measures degrees and
        // is accepted anywhere on the real line. One shared predicate, so a key and a constant are
        // admitted identically.
        return document::isScalarWithinSchemaDomain(owner->schemaKey, value);
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

    // Which curve kind a parameter gets is decided entirely by the shared schema predicates: the
    // Vec2d transform values (position, anchor, scale) seed a Vec2 curve, the scalar ones
    // (rotation, opacity) seed a scalar curve, and anything else is refused as unsupported. The
    // seeded key is always the parameter's existing constant, so turning a parameter into an
    // animation never changes the picture at the initial time.
    std::variant<double, document::Vec2d, core::Color4d> initialValue;
    if (document::isScalarAnimatableSchemaKey(parameter->schemaKey)) {
        const auto* value = std::get_if<double>(&constant->value);
        const bool withinDomain =
            value != nullptr && std::isfinite(*value) &&
            document::isScalarWithinSchemaDomain(parameter->schemaKey, *value);
        if (!withinDomain) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Scalar constant is invalid for its schema");
        }
        initialValue = *value;
    } else if (document::isVec2AnimatableSchemaKey(parameter->schemaKey)) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        if (value == nullptr || !finiteValue(*value)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Vec2 constant is invalid for its schema");
        }
        initialValue = *value;
    } else if (document::isColor4AnimatableSchemaKey(parameter->schemaKey)) {
        const auto* value = std::get_if<core::Color4d>(&constant->value);
        if (value == nullptr || !finiteValue(*value)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Color constant is invalid for its schema");
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
    } else if (const auto* vectorValue = std::get_if<document::Vec2d>(&initialValue)) {
        inserted = composition->animationCurves().insert(document::Vec2AnimationCurve{
            *curveId,
            {{*keyframeId, initialTime_, *vectorValue, document::KeyframeInterpolation::Linear}}});
    } else {
        const auto colorValue = std::get<core::Color4d>(initialValue);
        inserted = composition->animationCurves().insert(document::Color4AnimationCurve{
            *curveId,
            {{*keyframeId, initialTime_, colorValue, document::KeyframeInterpolation::Linear}}});
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

std::string_view InsertColor4Keyframe::typeId() const noexcept {
    return "bloom.animation.insert-color4-keyframe";
}

OperationResult InsertColor4Keyframe::apply(document::Draft& draft) const {
    return insertKeyframe<document::Color4AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Color4Keyframe{{}, time_, value_, outgoingInterpolation_});
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

std::string_view UpdateColor4Keyframe::typeId() const noexcept {
    return "bloom.animation.update-color4-keyframe";
}

OperationResult UpdateColor4Keyframe::apply(document::Draft& draft) const {
    return updateKeyframe<document::Color4AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Color4Keyframe{keyframeId_, time_, value_, outgoingInterpolation_});
}

std::string_view SetKeyframeInterpolation::typeId() const noexcept {
    return "bloom.animation.set-keyframe-interpolation";
}

OperationResult SetKeyframeInterpolation::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    if (!validInterpolation(interpolation_)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Keyframe interpolation mode is unsupported");
    }
    const auto* record = composition->animationCurves().find(curveId_);
    if (record == nullptr) {
        return invalidCurve(curveId_);
    }
    // One visit over the curve-kind variant: the rule is identical for every value kind, because an
    // interpolation change never touches a value. The key keeps its KeyframeId, exact time, and
    // value bit-for-bit; only the outgoing mode moves, and the write goes back through the store's
    // own updateKeyframe() overload for that kind rather than mutating the record in place.
    return std::visit(
        [&](const auto& curve) {
            using Keyframe = std::decay_t<decltype(curve.keyframes.front())>;
            const auto key = std::ranges::find(curve.keyframes, keyframeId_, &Keyframe::id);
            if (key == curve.keyframes.end()) {
                return invalidKeyframe(keyframeId_);
            }
            if (key->outgoingInterpolation == interpolation_) {
                return OperationResult::noChange(keyframeOutput(keyframeId_));
            }
            // The final key's outgoing interpolation is canonical Linear and every mutation
            // normalizes it back, so accepting anything else here would produce a transaction that
            // silently did nothing -- refuse it instead.
            if (key + 1 == curve.keyframes.end() &&
                interpolation_ != document::KeyframeInterpolation::Linear) {
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "The final keyframe interpolation must stay canonical Linear");
            }
            Keyframe updated = *key;
            updated.outgoingInterpolation = interpolation_;
            if (!composition->animationCurves().updateKeyframe(curveId_, updated)) {
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Keyframe interpolation could not be updated");
            }
            return OperationResult::applied(keyframeOutput(keyframeId_));
        },
        *record);
}

std::string_view SetKeyframeAtTime::typeId() const noexcept {
    return "bloom.animation.set-keyframe-at-time";
}

OperationResult SetKeyframeAtTime::apply(document::Draft& draft) const {
    if (const auto* scalar = std::get_if<double>(&value_)) {
        return setKeyframeAtTime<document::ScalarAnimationCurve, document::ScalarKeyframe>(
            draft, compositionId_, curveId_, time_, *scalar);
    }
    if (const auto* vector = std::get_if<document::Vec2d>(&value_)) {
        return setKeyframeAtTime<document::Vec2AnimationCurve, document::Vec2Keyframe>(
            draft, compositionId_, curveId_, time_, *vector);
    }
    return setKeyframeAtTime<document::Color4AnimationCurve, document::Color4Keyframe>(
        draft, compositionId_, curveId_, time_, std::get<core::Color4d>(value_));
}

std::string_view SetKeyframeAtTimeForParameter::typeId() const noexcept {
    return "bloom.animation.set-keyframe-at-time-for-parameter";
}

OperationResult SetKeyframeAtTimeForParameter::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr) {
        return invalidComposition(compositionId_);
    }
    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr) {
        return invalidParameter(parameterId_);
    }
    // Resolved from the DRAFT, so an earlier CreateAnimationForParameter in the same transaction is
    // already visible here. A parameter that is still constant (or is driven) has no curve to key.
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter does not have an animation source");
    }
    if (const auto* scalar = std::get_if<double>(&value_)) {
        return setKeyframeAtTime<document::ScalarAnimationCurve, document::ScalarKeyframe>(
            draft, compositionId_, source->curveId, time_, *scalar);
    }
    if (const auto* vector = std::get_if<document::Vec2d>(&value_)) {
        return setKeyframeAtTime<document::Vec2AnimationCurve, document::Vec2Keyframe>(
            draft, compositionId_, source->curveId, time_, *vector);
    }
    return setKeyframeAtTime<document::Color4AnimationCurve, document::Color4Keyframe>(
        draft, compositionId_, source->curveId, time_, std::get<core::Color4d>(value_));
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
        if (!document::isScalarAnimatableSchemaKey(parameter->schemaKey) ||
            composition->animationCurves().findScalar(source->curveId) == nullptr ||
            !std::isfinite(*scalar) ||
            !document::isScalarWithinSchemaDomain(parameter->schemaKey, *scalar)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match scalar animation");
        }
        constantValue = *scalar;
    } else if (const auto* vector = std::get_if<document::Vec2d>(&value_)) {
        if (!document::isVec2AnimatableSchemaKey(parameter->schemaKey) ||
            composition->animationCurves().findVec2(source->curveId) == nullptr ||
            !finiteValue(*vector)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match Vec2 animation");
        }
        constantValue = *vector;
    } else {
        const auto color = std::get<core::Color4d>(value_);
        if (!document::isColor4AnimatableSchemaKey(parameter->schemaKey) ||
            composition->animationCurves().findColor4(source->curveId) == nullptr ||
            !finiteValue(color)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match color animation");
        }
        constantValue = color;
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

namespace {
using CurveEdits = std::map<document::AnimationCurveId, document::AnimationCurveRecord>;
OperationResult stageKeys(const document::Composition& composition,
                          const std::vector<KeyframeAddress>& keys, CurveEdits& edits) {
    std::set<std::pair<document::AnimationCurveId, document::KeyframeId>> seen;
    for (const auto& key : keys) {
        if (!seen.emplace(key.curveId, key.keyframeId).second)
            return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                             "Duplicate selected key");
        const auto* curve = composition.animationCurves().find(key.curveId);
        if (!curve)
            return invalidCurve(key.curveId);
        const bool found = std::visit(
            [&](const auto& record) {
                return std::ranges::any_of(record.keyframes,
                                           [&](const auto& k) { return k.id == key.keyframeId; });
            },
            *curve);
        if (!found)
            return invalidKeyframe(key.keyframeId);
        edits.emplace(key.curveId, *curve);
    }
    return OperationResult::applied();
}
OperationResult publishCurves(document::Composition& composition, CurveEdits edits) {
    auto staged = composition.animationCurves();
    bool changed = false;
    for (auto& [id, record] : edits) {
        std::visit(
            [](auto& curve) {
                using Key = typename std::decay_t<decltype(curve.keyframes)>::value_type;
                std::ranges::sort(curve.keyframes, {}, &Key::time);
                if (!curve.keyframes.empty())
                    curve.keyframes.back().outgoingInterpolation =
                        document::KeyframeInterpolation::Linear;
            },
            record);
        const auto* original = staged.find(id);
        if (original && *original == record)
            continue;
        changed = true;
        if (!staged.erase(id) || !staged.insert(record))
            return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                             "Key times collide or curve is invalid");
    }
    if (!changed)
        return OperationResult::noChange();
    composition.animationCurves() = std::move(staged);
    return OperationResult::applied();
}
} // namespace

std::string_view MoveKeyframes::typeId() const noexcept { return "bloom.animation.move-keyframes"; }
OperationResult MoveKeyframes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    std::vector<KeyframeAddress> addresses;
    addresses.reserve(keys_.size());
    for (const auto& key : keys_)
        addresses.push_back(key.key);
    CurveEdits edits;
    auto result = stageKeys(*composition, addresses, edits);
    if (result.status == OperationStatus::Rejected)
        return result;
    for (const auto& move : keys_) {
        if (move.time < core::RationalTime::fromInteger(0) || move.time >= composition->duration())
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Key time is outside the composition");
        std::visit(
            [&](auto& curve) {
                for (auto& key : curve.keyframes)
                    if (key.id == move.key.keyframeId)
                        key.time = move.time;
            },
            edits.at(move.key.curveId));
    }
    return publishCurves(*composition, std::move(edits));
}

std::string_view DeleteKeyframes::typeId() const noexcept {
    return "bloom.animation.delete-keyframes";
}
OperationResult DeleteKeyframes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    CurveEdits edits;
    auto result = stageKeys(*composition, keys_, edits);
    if (result.status == OperationStatus::Rejected)
        return result;
    // Stage parameter/curve stores together: removing every key restores the earliest key's value.
    auto parameters = composition->parameters();
    auto curves = composition->animationCurves();
    for (auto& [id, record] : edits) {
        bool valid = std::visit(
            [&](auto& curve) {
                const auto fallback = curve.keyframes.front().value;
                std::erase_if(curve.keyframes, [&](const auto& key) {
                    return std::ranges::find(keys_, KeyframeAddress{id, key.id}) != keys_.end();
                });
                if (curve.keyframes.empty()) {
                    for (const auto& parameter : composition->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source && source->curveId == id &&
                            !parameters.setSource(parameter.id,
                                                  document::ConstantValueSource{fallback}))
                            return false;
                    }
                    return curves.erase(id);
                }
                return curves.erase(id) && curves.insert(curve);
            },
            record);
        if (!valid)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Keys could not be removed");
    }
    if (keys_.empty())
        return OperationResult::noChange();
    composition->parameters() = std::move(parameters);
    composition->animationCurves() = std::move(curves);
    return OperationResult::applied();
}

std::string_view SetKeyframesInterpolation::typeId() const noexcept {
    return "bloom.animation.set-keyframes-interpolation";
}
OperationResult SetKeyframesInterpolation::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    if (!validInterpolation(interpolation_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Unsupported interpolation");
    CurveEdits edits;
    auto result = stageKeys(*composition, keys_, edits);
    if (result.status == OperationStatus::Rejected)
        return result;
    for (const auto& address : keys_)
        std::visit(
            [&](auto& curve) {
                for (auto& key : curve.keyframes)
                    if (key.id == address.keyframeId)
                        key.outgoingInterpolation = interpolation_;
            },
            edits.at(address.curveId));
    return publishCurves(*composition, std::move(edits));
}

std::string_view PasteKeyframes::typeId() const noexcept {
    return "bloom.animation.paste-keyframes";
}
OperationResult PasteKeyframes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    CurveEdits edits;
    std::set<document::AnimationCurveId> created;
    for (const auto& paste : keys_) {
        if (paste.time < core::RationalTime::fromInteger(0) ||
            paste.time >= composition->duration() || !validInterpolation(paste.interpolation))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid pasted key time or interpolation");
        const auto* parameter = composition->parameters().find(paste.parameterId);
        if (!parameter)
            return invalidParameter(paste.parameterId);
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (!source) {
            auto result = CreateAnimationForParameter(composition_, paste.parameterId, paste.time)
                              .apply(draft);
            if (result.status == OperationStatus::Rejected)
                return result;
            parameter = composition->parameters().find(paste.parameterId);
            source = std::get_if<document::AnimationCurveSource>(&parameter->source);
            if (!source)
                return invalidParameter(paste.parameterId);
            created.insert(source->curveId);
        }
        const auto curveId = source->curveId;
        if (!edits.contains(curveId)) {
            edits.emplace(curveId, *composition->animationCurves().find(curveId));
            if (created.contains(curveId))
                std::visit([](auto& curve) { curve.keyframes.clear(); }, edits.at(curveId));
        }
        auto result = std::visit(
            [&](auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                using Key = typename std::decay_t<decltype(curve.keyframes)>::value_type;
                using Value = decltype(Key{}.value);
                const auto* value = std::get_if<Value>(&paste.value);
                if (!value || !validValueForCurve<Curve>(*composition, curveId, *value))
                    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                     "Pasted value does not match parameter");
                if (std::ranges::any_of(curve.keyframes,
                                        [&](const auto& key) { return key.time == paste.time; }))
                    return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                     "Paste time is occupied");
                const auto id = draft.ids().allocateKeyframe();
                if (!id)
                    return exhaustedIds();
                curve.keyframes.push_back(Key{*id, paste.time, *value, paste.interpolation});
                return OperationResult::applied();
            },
            edits.at(curveId));
        if (result.status == OperationStatus::Rejected)
            return result;
    }
    return publishCurves(*composition, std::move(edits));
}

} // namespace bloom::commands
