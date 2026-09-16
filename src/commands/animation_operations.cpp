#include <bloom/commands/animation_operations.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
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
    } else if constexpr (std::is_same_v<Value, document::Vec3d>) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
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
    } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
        return store.findVec3(curveId);
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
    } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
        return document::isVec3AnimatableSchemaKey(owner.schemaKey);
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

template <std::size_t Count>
OperationResult setComponentKeyframes(
    document::Draft& draft, document::Composition& composition,
    const document::AnimationCurveId curveId, const core::RationalTime time,
    const std::array<document::AnimationComponent, Count> components,
    const std::array<double, Count> values, const bool requireNew,
    const document::KeyframeInterpolation interpolation = document::KeyframeInterpolation::Linear) {
    const auto* record = composition.animationCurves().find(curveId);
    if (record == nullptr) {
        return invalidCurve(curveId);
    }
    std::array<const document::ComponentAnimationCurve*, Count> current{};
    for (std::size_t index = 0; index < Count; ++index) {
        current[index] = composition.animationCurves().findComponent(curveId, components[index]);
        if (current[index] == nullptr) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Component does not belong to the parameter curve");
        }
        if (requireNew && std::ranges::any_of(current[index]->keyframes, [time](const auto& key) {
                return key.time == time;
            })) {
            return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                             "A keyframe already exists at the exact time");
        }
        const auto* owner = curveOwner(composition, curveId);
        if (owner == nullptr || !std::isfinite(values[index])) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Component keyframe value is invalid");
        }
        if constexpr (Count == 4) {
            if (components[index] == document::AnimationComponent::Alpha &&
                (values[index] < 0.0 || values[index] > 1.0)) {
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "Alpha keyframe value is outside its schema domain");
            }
        }
    }

    std::vector<OperationOutput> outputs;
    bool changed = false;
    for (std::size_t index = 0; index < Count; ++index) {
        const auto* component =
            composition.animationCurves().findComponent(curveId, components[index]);
        const auto existing =
            std::ranges::find(component->keyframes, time, &document::ScalarKeyframe::time);
        if (existing != component->keyframes.end()) {
            outputs.push_back({std::string(kKeyframeOutput), DurableObjectId{existing->id}});
            if (existing->value == values[index]) {
                continue;
            }
            auto updated = *existing;
            updated.value = values[index];
            if (!composition.animationCurves().updateKeyframe(curveId, components[index],
                                                              updated)) {
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Component keyframe could not be updated");
            }
            changed = true;
            continue;
        }
        const auto id = draft.ids().allocateKeyframe();
        if (!id.has_value()) {
            return exhaustedIds();
        }
        if (!composition.animationCurves().insertKeyframe(
                curveId, components[index],
                document::ScalarKeyframe{*id, time, values[index], interpolation})) {
            return OperationResult::rejected(
                OperationIssueCode::InvalidOrder,
                "A component keyframe already exists at the exact time");
        }
        outputs.push_back({std::string(kKeyframeOutput), DurableObjectId{*id}});
        changed = true;
    }
    if (changed)
        static_cast<void>(
            composition.animationCurves().synchronizeCompatibilityProjection(curveId));
    return changed ? OperationResult::applied(std::move(outputs))
                   : OperationResult::noChange(std::move(outputs));
}

template <typename Value, std::size_t Count>
OperationResult
setGroupedValueAtTime(document::Draft& draft, document::Composition& composition,
                      const document::AnimationCurveId curveId, const core::RationalTime time,
                      const Value& value,
                      const std::array<document::AnimationComponent, Count> components,
                      const std::array<double, Count> values) {
    static_cast<void>(value);
    const auto* record = composition.animationCurves().find(curveId);
    if (record == nullptr) {
        return invalidCurve(curveId);
    }
    const bool grouped = std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return false;
            } else {
                return std::ranges::any_of(curve.components, [](const auto& component) {
                    return !component.keyframes.empty();
                });
            }
        },
        *record);
    if (!grouped) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation curve is not component-aware");
    }
    return setComponentKeyframes(draft, composition, curveId, time, components, values, false);
}

template <std::size_t Count>
OperationResult updateComponentKeyframes(
    document::Composition& composition, const document::AnimationCurveId curveId,
    const document::KeyframeId anchorId, const core::RationalTime time,
    const std::array<document::AnimationComponent, Count> components,
    const std::array<double, Count> values, const document::KeyframeInterpolation interpolation) {
    std::optional<core::RationalTime> oldTime;
    for (const auto component : components) {
        const auto* curve = composition.animationCurves().findComponent(curveId, component);
        if (curve == nullptr)
            return invalidKeyframe(anchorId);
        const auto key =
            std::ranges::find(curve->keyframes, anchorId, &document::ScalarKeyframe::id);
        if (key != curve->keyframes.end()) {
            oldTime = key->time;
            break;
        }
    }
    if (!oldTime.has_value())
        return invalidKeyframe(anchorId);
    std::array<document::KeyframeId, Count> ids{};
    std::size_t found = 0;
    for (std::size_t index = 0; index < Count; ++index) {
        const auto* curve = composition.animationCurves().findComponent(curveId, components[index]);
        if (curve == nullptr)
            continue;
        const auto key =
            std::ranges::find(curve->keyframes, *oldTime, &document::ScalarKeyframe::time);
        if (key == curve->keyframes.end())
            continue;
        const auto occupied =
            std::ranges::find(curve->keyframes, time, &document::ScalarKeyframe::time);
        if (occupied != curve->keyframes.end() && occupied->id != key->id)
            return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                             "Keyframe time is occupied");
        ids[index] = key->id;
        ++found;
    }
    if (found == 0)
        return invalidKeyframe(anchorId);
    bool changed = false;
    for (std::size_t index = 0; index < Count; ++index) {
        if (!ids[index].isValid())
            continue;
        const auto* curve = composition.animationCurves().findComponent(curveId, components[index]);
        const auto key =
            std::ranges::find(curve->keyframes, ids[index], &document::ScalarKeyframe::id);
        auto updated = *key;
        updated.time = time;
        updated.value = values[index];
        updated.outgoingInterpolation = interpolation;
        if (!(updated == *key)) {
            if (!composition.animationCurves().updateKeyframe(curveId, components[index], updated))
                return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                 "Component keyframe could not be updated");
            changed = true;
        }
    }
    if (changed)
        static_cast<void>(
            composition.animationCurves().synchronizeCompatibilityProjection(curveId));
    return changed ? OperationResult::applied(keyframeOutput(anchorId))
                   : OperationResult::noChange(keyframeOutput(anchorId));
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
    std::variant<double, document::Vec2d, document::Vec3d, core::Color4d> initialValue;
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
    } else if (document::isVec3AnimatableSchemaKey(parameter->schemaKey)) {
        const auto* value = std::get_if<document::Vec3d>(&constant->value);
        if (value == nullptr || !std::isfinite(value->x) || !std::isfinite(value->y) ||
            !std::isfinite(value->z)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Vec3 constant is invalid for its schema");
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
    if (!curveId.has_value()) {
        return exhaustedIds();
    }
    const std::size_t componentCount = std::holds_alternative<double>(initialValue)            ? 1
                                       : std::holds_alternative<document::Vec2d>(initialValue) ? 2
                                       : std::holds_alternative<document::Vec3d>(initialValue) ? 3
                                                                                               : 4;
    if (component_.has_value() && componentCount == 1) {
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Scalar parameters do not have components");
    }
    std::vector<document::KeyframeId> keyframeIds;
    keyframeIds.reserve(component_.has_value() ? 1 : componentCount);
    for (std::size_t index = 0; index < (component_.has_value() ? 1 : componentCount); ++index) {
        const auto keyframeId = draft.ids().allocateKeyframe();
        if (!keyframeId.has_value())
            return exhaustedIds();
        keyframeIds.push_back(*keyframeId);
    }

    bool inserted = false;
    if (const auto* value = std::get_if<double>(&initialValue)) {
        inserted = composition->animationCurves().insert(
            document::ScalarAnimationCurve{*curveId,
                                           {{keyframeIds.front(), initialTime_, *value,
                                             document::KeyframeInterpolation::Linear}}});
    } else if (const auto* vectorValue = std::get_if<document::Vec2d>(&initialValue)) {
        document::Vec2AnimationCurve curve{*curveId, {}, {}};
        const auto names =
            std::array{document::AnimationComponent::X, document::AnimationComponent::Y};
        const auto values = std::array{vectorValue->x, vectorValue->y};
        if (!component_.has_value())
            curve.keyframes.push_back({keyframeIds.front(), initialTime_, *vectorValue,
                                       document::KeyframeInterpolation::Linear});
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (component_.has_value() && *component_ != names[index])
                continue;
            const auto keyIndex = component_.has_value() ? 0 : index;
            curve.components[index].keyframes.push_back({keyframeIds[keyIndex], initialTime_,
                                                         values[index],
                                                         document::KeyframeInterpolation::Linear});
        }
        inserted = composition->animationCurves().insert(std::move(curve));
    } else if (const auto* vector3Value = std::get_if<document::Vec3d>(&initialValue)) {
        document::Vec3AnimationCurve curve{*curveId, {}, {}};
        if (!component_.has_value())
            curve.keyframes.push_back({keyframeIds.front(), initialTime_, *vector3Value,
                                       document::KeyframeInterpolation::Linear});
        const std::array values{vector3Value->x, vector3Value->y, vector3Value->z};
        const auto names =
            std::array{document::AnimationComponent::X, document::AnimationComponent::Y,
                       document::AnimationComponent::Z};
        for (std::size_t index = 0; index < values.size(); ++index)
            if (!component_.has_value() || *component_ == names[index])
                curve.components[index].keyframes.push_back(
                    {keyframeIds[component_.has_value() ? 0 : index], initialTime_, values[index],
                     document::KeyframeInterpolation::Linear});
        inserted = composition->animationCurves().insert(std::move(curve));
    } else {
        const auto colorValue = std::get<core::Color4d>(initialValue);
        document::Color4AnimationCurve curve{*curveId, {}, {}};
        if (!component_.has_value())
            curve.keyframes.push_back({keyframeIds.front(), initialTime_, colorValue,
                                       document::KeyframeInterpolation::Linear});
        const std::array values{colorValue.red, colorValue.green, colorValue.blue,
                                colorValue.alpha};
        const auto names =
            std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                       document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
        for (std::size_t index = 0; index < values.size(); ++index)
            if (!component_.has_value() || *component_ == names[index])
                curve.components[index].keyframes.push_back(
                    {keyframeIds[component_.has_value() ? 0 : index], initialTime_, values[index],
                     document::KeyframeInterpolation::Linear});
        inserted = composition->animationCurves().insert(std::move(curve));
    }

    const auto defaultValue = std::visit(
        [](const auto& value) -> document::ParameterValue { return value; }, initialValue);
    if (!inserted || !composition->parameters().setSource(
                         parameterId_, document::AnimationCurveSource{*curveId, defaultValue})) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Animation could not be attached to the parameter");
    }
    std::vector<OperationOutput> outputs{
        {std::string(kAnimationCurveOutput), DurableObjectId{*curveId}}};
    for (const auto keyframeId : keyframeIds)
        outputs.push_back({std::string(kKeyframeOutput), DurableObjectId{keyframeId}});
    return OperationResult::applied(std::move(outputs));
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
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findVec2(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        return setComponentKeyframes(
            draft, *composition, curveId_, time_,
            std::array{document::AnimationComponent::X, document::AnimationComponent::Y},
            std::array{value_.x, value_.y}, true);
    }
    return insertKeyframe<document::Vec2AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec2Keyframe{{}, time_, value_, outgoingInterpolation_});
}

std::string_view InsertVec3Keyframe::typeId() const noexcept {
    return "bloom.animation.insert-vec3-keyframe";
}

OperationResult InsertVec3Keyframe::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findVec3(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        return setComponentKeyframes(draft, *composition, curveId_, time_,
                                     std::array{document::AnimationComponent::X,
                                                document::AnimationComponent::Y,
                                                document::AnimationComponent::Z},
                                     std::array{value_.x, value_.y, value_.z}, true);
    }
    return insertKeyframe<document::Vec3AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec3Keyframe{{}, time_, value_, outgoingInterpolation_});
}

std::string_view InsertColor4Keyframe::typeId() const noexcept {
    return "bloom.animation.insert-color4-keyframe";
}

OperationResult InsertColor4Keyframe::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findColor4(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        return setComponentKeyframes(
            draft, *composition, curveId_, time_,
            std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                       document::AnimationComponent::Blue, document::AnimationComponent::Alpha},
            std::array{value_.red, value_.green, value_.blue, value_.alpha}, true);
    }
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
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findVec2(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        return updateComponentKeyframes(
            *composition, curveId_, keyframeId_, time_,
            std::array{document::AnimationComponent::X, document::AnimationComponent::Y},
            std::array{value_.x, value_.y}, outgoingInterpolation_);
    }
    return updateKeyframe<document::Vec2AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec2Keyframe{keyframeId_, time_, value_, outgoingInterpolation_});
}

std::string_view UpdateVec3Keyframe::typeId() const noexcept {
    return "bloom.animation.update-vec3-keyframe";
}

OperationResult UpdateVec3Keyframe::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findVec3(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        const auto names =
            std::array{document::AnimationComponent::X, document::AnimationComponent::Y,
                       document::AnimationComponent::Z};
        return updateComponentKeyframes(*composition, curveId_, keyframeId_, time_, names,
                                        std::array{value_.x, value_.y, value_.z},
                                        outgoingInterpolation_);
    }
    return updateKeyframe<document::Vec3AnimationCurve>(
        draft, compositionId_, curveId_,
        document::Vec3Keyframe{keyframeId_, time_, value_, outgoingInterpolation_});
}

std::string_view UpdateColor4Keyframe::typeId() const noexcept {
    return "bloom.animation.update-color4-keyframe";
}

OperationResult UpdateColor4Keyframe::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    if (const auto* curve = composition->animationCurves().findColor4(curveId_);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        const auto names =
            std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                       document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
        return updateComponentKeyframes(
            *composition, curveId_, keyframeId_, time_, names,
            std::array{value_.red, value_.green, value_.blue, value_.alpha},
            outgoingInterpolation_);
    }
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
    const auto updateComponent = [&](const document::AnimationComponent component,
                                     const core::RationalTime time) -> OperationResult {
        const auto* values = composition->animationCurves().findComponent(curveId_, component);
        if (values == nullptr)
            return invalidKeyframe(keyframeId_);
        const auto key =
            std::ranges::find(values->keyframes, keyframeId_, &document::ScalarKeyframe::id);
        if (key == values->keyframes.end()) {
            const auto atTime =
                std::ranges::find(values->keyframes, time, &document::ScalarKeyframe::time);
            if (atTime == values->keyframes.end())
                return invalidKeyframe(keyframeId_);
            if (atTime->outgoingInterpolation == interpolation_)
                return OperationResult::noChange(keyframeOutput(atTime->id));
            if (atTime + 1 == values->keyframes.end() &&
                interpolation_ != document::KeyframeInterpolation::Linear)
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "The final keyframe interpolation must stay canonical Linear");
            auto updated = *atTime;
            updated.outgoingInterpolation = interpolation_;
            if (!composition->animationCurves().updateKeyframe(curveId_, component, updated))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Keyframe interpolation could not be updated");
            return OperationResult::applied(keyframeOutput(atTime->id));
        }
        if (key->outgoingInterpolation == interpolation_)
            return OperationResult::noChange(keyframeOutput(keyframeId_));
        if (key + 1 == values->keyframes.end() &&
            interpolation_ != document::KeyframeInterpolation::Linear)
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "The final keyframe interpolation must stay canonical Linear");
        auto updated = *key;
        updated.outgoingInterpolation = interpolation_;
        if (!composition->animationCurves().updateKeyframe(curveId_, component, updated))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Keyframe interpolation could not be updated");
        return OperationResult::applied(keyframeOutput(keyframeId_));
    };

    if (component_.has_value()) {
        const auto* values = composition->animationCurves().findComponent(curveId_, *component_);
        if (values == nullptr)
            return invalidKeyframe(keyframeId_);
        const auto key =
            std::ranges::find(values->keyframes, keyframeId_, &document::ScalarKeyframe::id);
        if (key == values->keyframes.end())
            return invalidKeyframe(keyframeId_);
        return updateComponent(*component_, key->time);
    }

    return std::visit(
        [&](const auto& curve) -> OperationResult {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                const auto key =
                    std::ranges::find(curve.keyframes, keyframeId_, &document::ScalarKeyframe::id);
                if (key == curve.keyframes.end())
                    return invalidKeyframe(keyframeId_);
                if (key->outgoingInterpolation == interpolation_)
                    return OperationResult::noChange(keyframeOutput(keyframeId_));
                if (key + 1 == curve.keyframes.end() &&
                    interpolation_ != document::KeyframeInterpolation::Linear)
                    return OperationResult::rejected(
                        OperationIssueCode::InvalidValue,
                        "The final keyframe interpolation must stay canonical Linear");
                auto updated = *key;
                updated.outgoingInterpolation = interpolation_;
                if (!composition->animationCurves().updateKeyframe(curveId_, updated))
                    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                     "Keyframe interpolation could not be updated");
                return OperationResult::applied(keyframeOutput(keyframeId_));
            } else {
                const auto names = [&] {
                    if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>)
                        return std::array{document::AnimationComponent::X,
                                          document::AnimationComponent::Y};
                    else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>)
                        return std::array{document::AnimationComponent::X,
                                          document::AnimationComponent::Y,
                                          document::AnimationComponent::Z};
                    else
                        return std::array{document::AnimationComponent::Red,
                                          document::AnimationComponent::Green,
                                          document::AnimationComponent::Blue,
                                          document::AnimationComponent::Alpha};
                }();
                std::optional<core::RationalTime> time;
                for (const auto component : names) {
                    const auto* values =
                        composition->animationCurves().findComponent(curveId_, component);
                    if (values == nullptr)
                        continue;
                    const auto key = std::ranges::find(values->keyframes, keyframeId_,
                                                       &document::ScalarKeyframe::id);
                    if (key != values->keyframes.end()) {
                        time = key->time;
                        break;
                    }
                }
                if (!time.has_value()) {
                    for (const auto& key : curve.keyframes) {
                        if (key.id == keyframeId_) {
                            time = key.time;
                            break;
                        }
                    }
                }
                if (!time.has_value())
                    return invalidKeyframe(keyframeId_);
                bool changed = false;
                for (const auto component : names) {
                    const auto* values =
                        composition->animationCurves().findComponent(curveId_, component);
                    if (values == nullptr)
                        continue;
                    const auto key = std::ranges::find(values->keyframes, *time,
                                                       &document::ScalarKeyframe::time);
                    if (key == values->keyframes.end())
                        continue;
                    if (key + 1 == values->keyframes.end() &&
                        interpolation_ != document::KeyframeInterpolation::Linear)
                        return OperationResult::rejected(
                            OperationIssueCode::InvalidValue,
                            "The final keyframe interpolation must stay canonical Linear");
                    if (key->outgoingInterpolation == interpolation_)
                        continue;
                    auto updated = *key;
                    updated.outgoingInterpolation = interpolation_;
                    if (!composition->animationCurves().updateKeyframe(curveId_, component,
                                                                       updated))
                        return OperationResult::rejected(
                            OperationIssueCode::InvalidValue,
                            "Keyframe interpolation could not be updated");
                    changed = true;
                }
                return changed ? OperationResult::applied(keyframeOutput(keyframeId_))
                               : OperationResult::noChange(keyframeOutput(keyframeId_));
            }
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
        if (auto* composition = draft.project().findComposition(compositionId_);
            composition != nullptr) {
            if (const auto* curve = composition->animationCurves().findVec2(curveId_);
                curve != nullptr &&
                std::ranges::any_of(curve->components, [](const auto& component) {
                    return !component.keyframes.empty();
                })) {
                return setGroupedValueAtTime(
                    draft, *composition, curveId_, time_, *vector,
                    std::array{document::AnimationComponent::X, document::AnimationComponent::Y},
                    std::array{vector->x, vector->y});
            }
        }
        return setKeyframeAtTime<document::Vec2AnimationCurve, document::Vec2Keyframe>(
            draft, compositionId_, curveId_, time_, *vector);
    }
    if (const auto* vector = std::get_if<document::Vec3d>(&value_)) {
        if (auto* composition = draft.project().findComposition(compositionId_);
            composition != nullptr) {
            if (const auto* curve = composition->animationCurves().findVec3(curveId_);
                curve != nullptr &&
                std::ranges::any_of(curve->components, [](const auto& component) {
                    return !component.keyframes.empty();
                })) {
                return setGroupedValueAtTime(draft, *composition, curveId_, time_, *vector,
                                             std::array{document::AnimationComponent::X,
                                                        document::AnimationComponent::Y,
                                                        document::AnimationComponent::Z},
                                             std::array{vector->x, vector->y, vector->z});
            }
        }
        return setKeyframeAtTime<document::Vec3AnimationCurve, document::Vec3Keyframe>(
            draft, compositionId_, curveId_, time_, *vector);
    }
    if (auto* composition = draft.project().findComposition(compositionId_);
        composition != nullptr) {
        if (const auto* curve = composition->animationCurves().findColor4(curveId_);
            curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
                return !component.keyframes.empty();
            })) {
            const auto& color = std::get<core::Color4d>(value_);
            return setGroupedValueAtTime(
                draft, *composition, curveId_, time_, color,
                std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                           document::AnimationComponent::Blue, document::AnimationComponent::Alpha},
                std::array{color.red, color.green, color.blue, color.alpha});
        }
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
        if (const auto* curve = composition->animationCurves().findVec2(source->curveId);
            curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
                return !component.keyframes.empty();
            })) {
            return setGroupedValueAtTime(
                draft, *composition, source->curveId, time_, *vector,
                std::array{document::AnimationComponent::X, document::AnimationComponent::Y},
                std::array{vector->x, vector->y});
        }
        return setKeyframeAtTime<document::Vec2AnimationCurve, document::Vec2Keyframe>(
            draft, compositionId_, source->curveId, time_, *vector);
    }
    if (const auto* vector = std::get_if<document::Vec3d>(&value_)) {
        if (const auto* curve = composition->animationCurves().findVec3(source->curveId);
            curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
                return !component.keyframes.empty();
            })) {
            return setGroupedValueAtTime(draft, *composition, source->curveId, time_, *vector,
                                         std::array{document::AnimationComponent::X,
                                                    document::AnimationComponent::Y,
                                                    document::AnimationComponent::Z},
                                         std::array{vector->x, vector->y, vector->z});
        }
        return setKeyframeAtTime<document::Vec3AnimationCurve, document::Vec3Keyframe>(
            draft, compositionId_, source->curveId, time_, *vector);
    }
    if (const auto* curve = composition->animationCurves().findColor4(source->curveId);
        curve != nullptr && std::ranges::any_of(curve->components, [](const auto& component) {
            return !component.keyframes.empty();
        })) {
        const auto& color = std::get<core::Color4d>(value_);
        return setGroupedValueAtTime(
            draft, *composition, source->curveId, time_, color,
            std::array{document::AnimationComponent::Red, document::AnimationComponent::Green,
                       document::AnimationComponent::Blue, document::AnimationComponent::Alpha},
            std::array{color.red, color.green, color.blue, color.alpha});
    }
    return setKeyframeAtTime<document::Color4AnimationCurve, document::Color4Keyframe>(
        draft, compositionId_, source->curveId, time_, std::get<core::Color4d>(value_));
}

std::string_view SetKeyframeAtTimeForParameterComponent::typeId() const noexcept {
    return "bloom.animation.set-keyframe-at-time-for-parameter-component";
}

OperationResult SetKeyframeAtTimeForParameterComponent::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return invalidComposition(compositionId_);
    const auto* parameter = composition->parameters().find(parameterId_);
    if (parameter == nullptr)
        return invalidParameter(parameterId_);
    if (!std::isfinite(value_) || !document::isAnimatableSchemaKey(parameter->schemaKey) ||
        document::isScalarAnimatableSchemaKey(parameter->schemaKey)) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Component keyframe value or component is invalid");
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter does not have an animation source");
    const auto* curveRecord = composition->animationCurves().find(source->curveId);
    if (curveRecord == nullptr)
        return invalidCurve(source->curveId);
    const auto* componentCurve =
        composition->animationCurves().findComponent(source->curveId, component_);
    if (componentCurve == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Component does not belong to the parameter curve");
    const auto* owner = curveOwner(*composition, source->curveId);
    if (owner == nullptr)
        return invalidParameter(parameterId_);
    const bool domainValid = [&] {
        if (composition->animationCurves().findVec2(source->curveId) != nullptr ||
            composition->animationCurves().findVec3(source->curveId) != nullptr) {
            return true;
        }
        if (composition->animationCurves().findColor4(source->curveId) != nullptr) {
            return component_ != document::AnimationComponent::Alpha ||
                   (value_ >= 0.0 && value_ <= 1.0);
        }
        return false;
    }();
    if (!domainValid)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Component keyframe value is outside its schema domain");
    const auto existing =
        std::ranges::find(componentCurve->keyframes, time_, &document::ScalarKeyframe::time);
    if (existing != componentCurve->keyframes.end()) {
        if (existing->value == value_)
            return OperationResult::noChange(keyframeOutput(existing->id));
        auto updated = *existing;
        updated.value = value_;
        const auto id = existing->id;
        if (!composition->animationCurves().updateKeyframe(source->curveId, component_, updated))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Component keyframe could not be updated");
        return OperationResult::applied(keyframeOutput(id));
    }

    const auto id = draft.ids().allocateKeyframe();
    if (!id.has_value())
        return exhaustedIds();
    if (!composition->animationCurves().insertKeyframe(
            source->curveId, component_,
            document::ScalarKeyframe{*id, time_, value_,
                                     document::KeyframeInterpolation::Linear})) {
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "A component keyframe already exists at the exact time");
    }
    return OperationResult::applied(keyframeOutput(*id));
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
    const bool grouped = std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return false;
            } else {
                return std::ranges::any_of(curve.components, [](const auto& component) {
                    return !component.keyframes.empty();
                });
            }
        },
        *record);
    if (component_.has_value()) {
        const auto* componentCurve =
            composition->animationCurves().findComponent(curveId_, *component_);
        if (componentCurve == nullptr)
            return invalidKeyframe(keyframeId_);
        const auto found = std::ranges::find(componentCurve->keyframes, keyframeId_,
                                             &document::ScalarKeyframe::id);
        if (found == componentCurve->keyframes.end())
            return invalidKeyframe(keyframeId_);
        std::size_t totalKeys = 0;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    for (const auto& component : curve.components)
                        totalKeys += component.keyframes.size();
                }
            },
            *record);
        if (totalKeys == 1) {
            const auto* owner = curveOwner(*composition, curveId_);
            const auto* source = owner == nullptr
                                     ? nullptr
                                     : std::get_if<document::AnimationCurveSource>(&owner->source);
            if (owner == nullptr || source == nullptr || !source->defaultValue.has_value()) {
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "The final component keyframe cannot be deleted without a parameter default");
            }
            if (!composition->parameters().setSource(
                    owner->id, document::ConstantValueSource{*source->defaultValue}) ||
                !composition->animationCurves().erase(curveId_)) {
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Component keyframe could not be deleted");
            }
            return OperationResult::applied(keyframeOutput(keyframeId_));
        }
        if (!composition->animationCurves().eraseKeyframe(curveId_, *component_, keyframeId_))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Component keyframe could not be deleted");
        return OperationResult::applied(keyframeOutput(keyframeId_));
    }
    if (grouped) {
        std::optional<core::RationalTime> time;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                for (const auto& key : curve.keyframes) {
                    if (key.id == keyframeId_) {
                        time = key.time;
                        return;
                    }
                }
                if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    for (const auto& component : curve.components) {
                        const auto key = std::ranges::find(component.keyframes, keyframeId_,
                                                           &document::ScalarKeyframe::id);
                        if (key != component.keyframes.end()) {
                            time = key->time;
                            return;
                        }
                    }
                }
            },
            *record);
        if (!time.has_value())
            return invalidKeyframe(keyframeId_);
        std::size_t totalKeys = 0;
        std::size_t keysAtTime = 0;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    for (const auto& component : curve.components) {
                        totalKeys += component.keyframes.size();
                        keysAtTime += static_cast<std::size_t>(std::ranges::count(
                            component.keyframes, *time, &document::ScalarKeyframe::time));
                    }
                }
            },
            *record);
        if (keysAtTime == 0)
            return invalidKeyframe(keyframeId_);
        if (keysAtTime == totalKeys) {
            const auto* owner = curveOwner(*composition, curveId_);
            const auto* source = owner == nullptr
                                     ? nullptr
                                     : std::get_if<document::AnimationCurveSource>(&owner->source);
            if (owner == nullptr || source == nullptr || !source->defaultValue.has_value())
                return OperationResult::rejected(
                    OperationIssueCode::InvalidValue,
                    "The final component keyframe cannot be deleted without a parameter default");
            if (!composition->parameters().setSource(
                    owner->id, document::ConstantValueSource{*source->defaultValue}) ||
                !composition->animationCurves().erase(curveId_))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Component keyframe could not be deleted");
            return OperationResult::applied(keyframeOutput(keyframeId_));
        }
        bool changed = false;
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    const auto names = [&] {
                        if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>)
                            return std::array{document::AnimationComponent::X,
                                              document::AnimationComponent::Y};
                        else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>)
                            return std::array{document::AnimationComponent::X,
                                              document::AnimationComponent::Y,
                                              document::AnimationComponent::Z};
                        else
                            return std::array{document::AnimationComponent::Red,
                                              document::AnimationComponent::Green,
                                              document::AnimationComponent::Blue,
                                              document::AnimationComponent::Alpha};
                    }();
                    for (std::size_t index = 0; index < curve.components.size(); ++index) {
                        const auto key = std::ranges::find(curve.components[index].keyframes, *time,
                                                           &document::ScalarKeyframe::time);
                        if (key != curve.components[index].keyframes.end()) {
                            changed = composition->animationCurves().eraseKeyframe(
                                          curveId_, names[index], key->id) ||
                                      changed;
                        }
                    }
                }
            },
            *record);
        return changed ? OperationResult::applied(keyframeOutput(keyframeId_))
                       : OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                   "Component keyframe could not be deleted");
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
    } else if (const auto* vector3 = std::get_if<document::Vec3d>(&value_)) {
        if (!document::isVec3AnimatableSchemaKey(parameter->schemaKey) ||
            composition->animationCurves().findVec3(source->curveId) == nullptr ||
            !finiteValue(*vector3)) {
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Constant value does not match Vec3 animation");
        }
        constantValue = *vector3;
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
    std::set<std::tuple<document::AnimationCurveId, document::KeyframeId,
                        std::optional<document::AnimationComponent>>>
        seen;
    for (const auto& key : keys) {
        if (!seen.emplace(key.curveId, key.keyframeId, key.component).second)
            return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                             "Duplicate selected key");
        const auto* curve = composition.animationCurves().find(key.curveId);
        if (!curve)
            return invalidCurve(key.curveId);
        const bool found = std::visit(
            [&](const auto& record) {
                using Curve = std::decay_t<decltype(record)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    return key.component == std::nullopt &&
                           std::ranges::any_of(record.keyframes, [&](const auto& k) {
                               return k.id == key.keyframeId;
                           });
                } else if (key.component.has_value()) {
                    const auto* component = record.component(*key.component);
                    return component != nullptr &&
                           std::ranges::any_of(component->keyframes, [&](const auto& k) {
                               return k.id == key.keyframeId;
                           });
                } else {
                    return std::ranges::any_of(
                               record.keyframes,
                               [&](const auto& k) { return k.id == key.keyframeId; }) ||
                           std::ranges::any_of(record.components, [&](const auto& component) {
                               return std::ranges::any_of(component.keyframes, [&](const auto& k) {
                                   return k.id == key.keyframeId;
                               });
                           });
                }
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
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    using Key = typename std::decay_t<decltype(curve.keyframes)>::value_type;
                    std::ranges::sort(curve.keyframes, {}, &Key::time);
                    if (!curve.keyframes.empty())
                        curve.keyframes.back().outgoingInterpolation =
                            document::KeyframeInterpolation::Linear;
                } else if (std::ranges::all_of(curve.components, [](const auto& component) {
                               return component.keyframes.empty();
                           })) {
                    using Key = typename std::decay_t<decltype(curve.keyframes)>::value_type;
                    std::ranges::sort(curve.keyframes, {}, &Key::time);
                    if (!curve.keyframes.empty())
                        curve.keyframes.back().outgoingInterpolation =
                            document::KeyframeInterpolation::Linear;
                } else {
                    for (auto& component : curve.components) {
                        std::ranges::sort(component.keyframes, {}, &document::ScalarKeyframe::time);
                        if (!component.keyframes.empty())
                            component.keyframes.back().outgoingInterpolation =
                                document::KeyframeInterpolation::Linear;
                    }
                }
            },
            record);
        const auto* original = staged.find(id);
        if (original && *original == record)
            continue;
        changed = true;
        if (!staged.erase(id) || !staged.insert(record))
            return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                             "Key times collide or curve is invalid");
        if (std::visit(
                [](const auto& curve) {
                    using Curve = std::decay_t<decltype(curve)>;
                    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>)
                        return false;
                    else
                        return std::ranges::any_of(curve.components, [](const auto& component) {
                            return !component.keyframes.empty();
                        });
                },
                record))
            static_cast<void>(staged.synchronizeCompatibilityProjection(id));
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
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    for (auto& key : curve.keyframes)
                        if (key.id == move.key.keyframeId)
                            key.time = move.time;
                } else if (std::ranges::all_of(curve.components, [](const auto& component) {
                               return component.keyframes.empty();
                           })) {
                    for (auto& key : curve.keyframes)
                        if (key.id == move.key.keyframeId)
                            key.time = move.time;
                } else if (move.key.component.has_value()) {
                    auto* component = curve.component(*move.key.component);
                    if (component != nullptr) {
                        for (auto& key : component->keyframes)
                            if (key.id == move.key.keyframeId)
                                key.time = move.time;
                    }
                } else {
                    std::optional<core::RationalTime> oldTime;
                    for (const auto& component : curve.components) {
                        const auto key = std::ranges::find(component.keyframes, move.key.keyframeId,
                                                           &document::ScalarKeyframe::id);
                        if (key != component.keyframes.end()) {
                            oldTime = key->time;
                            break;
                        }
                    }
                    if (!oldTime.has_value()) {
                        for (const auto& key : curve.keyframes) {
                            if (key.id == move.key.keyframeId) {
                                oldTime = key.time;
                                break;
                            }
                        }
                    }
                    if (oldTime.has_value()) {
                        for (auto& component : curve.components) {
                            for (auto& key : component.keyframes)
                                if (key.time == *oldTime)
                                    key.time = move.time;
                        }
                    }
                }
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
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    const auto fallback =
                        curve.keyframes.empty() ? 0.0 : curve.keyframes.front().value;
                    std::erase_if(curve.keyframes, [&](const auto& key) {
                        return std::ranges::any_of(keys_, [&](const auto& address) {
                            return address.curveId == id && !address.component.has_value() &&
                                   address.keyframeId == key.id;
                        });
                    });
                    if (curve.keyframes.empty()) {
                        const auto* owner = curveOwner(*composition, id);
                        if (owner == nullptr ||
                            !parameters.setSource(owner->id,
                                                  document::ConstantValueSource{fallback}))
                            return false;
                        return curves.erase(id);
                    }
                    return curves.erase(id) && curves.insert(curve);
                } else {
                    const bool componentAware =
                        std::ranges::any_of(curve.components, [](const auto& component) {
                            return !component.keyframes.empty();
                        });
                    if (!componentAware) {
                        const auto fallback =
                            curve.keyframes.empty()
                                ? document::ParameterValue{}
                                : document::ParameterValue{curve.keyframes.front().value};
                        std::erase_if(curve.keyframes, [&](const auto& key) {
                            return std::ranges::any_of(keys_, [&](const auto& address) {
                                return address.curveId == id && !address.component.has_value() &&
                                       address.keyframeId == key.id;
                            });
                        });
                        if (curve.keyframes.empty()) {
                            const auto* owner = curveOwner(*composition, id);
                            if (owner == nullptr ||
                                !parameters.setSource(owner->id,
                                                      document::ConstantValueSource{fallback}))
                                return false;
                            return curves.erase(id);
                        }
                        return curves.erase(id) && curves.insert(curve);
                    }

                    std::vector<core::RationalTime> wholeTimes;
                    for (const auto& address : keys_) {
                        if (address.curveId != id || address.component.has_value())
                            continue;
                        for (const auto& component : curve.components) {
                            const auto key =
                                std::ranges::find(component.keyframes, address.keyframeId,
                                                  &document::ScalarKeyframe::id);
                            if (key != component.keyframes.end()) {
                                wholeTimes.push_back(key->time);
                                break;
                            }
                        }
                        for (const auto& key : curve.keyframes) {
                            if (key.id == address.keyframeId)
                                wholeTimes.push_back(key.time);
                        }
                    }
                    for (std::size_t index = 0; index < curve.components.size(); ++index) {
                        auto& component = curve.components[index];
                        std::erase_if(component.keyframes, [&](const auto& key) {
                            return std::ranges::any_of(keys_, [&](const auto& address) {
                                if (address.curveId != id)
                                    return false;
                                if (address.component.has_value())
                                    return false;
                                return std::ranges::find(wholeTimes, key.time) != wholeTimes.end();
                            });
                        });
                    }
                    // Rebuild from the original record for component-scoped deletions, keeping
                    // the whole-time pass above as the all-components convenience path.
                    const auto* original = composition->animationCurves().find(id);
                    if (original != nullptr) {
                        std::visit(
                            [&](const auto& sourceCurve) {
                                using SourceCurve = std::decay_t<decltype(sourceCurve)>;
                                if constexpr (!std::is_same_v<SourceCurve,
                                                              document::ScalarAnimationCurve>) {
                                    for (std::size_t index = 0; index < curve.components.size();
                                         ++index) {
                                        const auto names = [&] {
                                            if constexpr (std::is_same_v<
                                                              Curve, document::Vec2AnimationCurve>)
                                                return std::array{document::AnimationComponent::X,
                                                                  document::AnimationComponent::Y};
                                            else if constexpr (std::is_same_v<
                                                                   Curve,
                                                                   document::Vec3AnimationCurve>)
                                                return std::array{document::AnimationComponent::X,
                                                                  document::AnimationComponent::Y,
                                                                  document::AnimationComponent::Z};
                                            else
                                                return std::array{
                                                    document::AnimationComponent::Red,
                                                    document::AnimationComponent::Green,
                                                    document::AnimationComponent::Blue,
                                                    document::AnimationComponent::Alpha};
                                        }();
                                        std::erase_if(
                                            curve.components[index].keyframes,
                                            [&](const auto& key) {
                                                return std::ranges::any_of(
                                                    keys_, [&](const auto& address) {
                                                        return address.curveId == id &&
                                                               address.component.has_value() &&
                                                               *address.component == names[index] &&
                                                               address.keyframeId == key.id;
                                                    });
                                            });
                                    }
                                }
                            },
                            *original);
                    }
                    const bool hasKeys =
                        std::ranges::any_of(curve.components, [](const auto& component) {
                            return !component.keyframes.empty();
                        });
                    if (!hasKeys) {
                        const auto* owner = curveOwner(*composition, id);
                        const auto* source =
                            owner == nullptr
                                ? nullptr
                                : std::get_if<document::AnimationCurveSource>(&owner->source);
                        if (owner == nullptr || source == nullptr ||
                            !source->defaultValue.has_value())
                            return false;
                        if (!parameters.setSource(
                                owner->id, document::ConstantValueSource{*source->defaultValue}))
                            return false;
                        return curves.erase(id);
                    }
                    return curves.erase(id) && curves.insert(curve);
                }
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
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    for (auto& key : curve.keyframes)
                        if (key.id == address.keyframeId)
                            key.outgoingInterpolation = interpolation_;
                } else {
                    const bool componentAware =
                        std::ranges::any_of(curve.components, [](const auto& component) {
                            return !component.keyframes.empty();
                        });
                    if (!componentAware) {
                        for (auto& key : curve.keyframes)
                            if (key.id == address.keyframeId)
                                key.outgoingInterpolation = interpolation_;
                    } else if (address.component.has_value()) {
                        auto* component = curve.component(*address.component);
                        if (component != nullptr) {
                            for (auto& key : component->keyframes)
                                if (key.id == address.keyframeId)
                                    key.outgoingInterpolation = interpolation_;
                        }
                    } else {
                        std::optional<core::RationalTime> time;
                        for (const auto& component : curve.components) {
                            const auto key =
                                std::ranges::find(component.keyframes, address.keyframeId,
                                                  &document::ScalarKeyframe::id);
                            if (key != component.keyframes.end()) {
                                time = key->time;
                                break;
                            }
                        }
                        if (!time.has_value()) {
                            for (const auto& key : curve.keyframes)
                                if (key.id == address.keyframeId)
                                    time = key.time;
                        }
                        if (time.has_value()) {
                            for (auto& component : curve.components) {
                                const auto key = std::ranges::find(component.keyframes, *time,
                                                                   &document::ScalarKeyframe::time);
                                if (key != component.keyframes.end())
                                    key->outgoingInterpolation = interpolation_;
                            }
                        }
                    }
                }
            },
            edits.at(address.curveId));
    return publishCurves(*composition, std::move(edits));
}

namespace {
// The scalar key sequence an address names, or null when the address does not name one. A scalar
// curve is addressed without a component; a vector or colour curve only through one, because its
// legacy whole-value projection carries neither handles nor an independently addressable value.
template <typename Curve>
[[nodiscard]] std::vector<document::ScalarKeyframe>*
addressedKeyframes(Curve& curve, const std::optional<document::AnimationComponent> component) {
    if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
        return component.has_value() ? nullptr : &curve.keyframes;
    } else {
        if (!component.has_value())
            return nullptr;
        auto* selected = curve.component(*component);
        return selected == nullptr ? nullptr : &selected->keyframes;
    }
}

[[nodiscard]] OperationResult wholeValueAddressRejected() {
    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                     "A whole-value keyframe address names no scalar component");
}
} // namespace

std::string_view SetKeyframeHandles::typeId() const noexcept {
    return "bloom.animation.set-keyframe-handles";
}
OperationResult SetKeyframeHandles::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    std::vector<KeyframeAddress> addresses;
    addresses.reserve(edits_.size());
    for (const auto& edit : edits_) {
        if ((edit.outgoing && !document::isValidKeyframeHandle(*edit.outgoing)) ||
            (edit.incoming && !document::isValidKeyframeHandle(*edit.incoming)))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Ease handle time must be within [0, 1] and its offset finite");
        addresses.push_back(edit.key);
    }
    CurveEdits edits;
    auto staged = stageKeys(*composition, addresses, edits);
    if (staged.status == OperationStatus::Rejected)
        return staged;
    for (const auto& edit : edits_) {
        auto outcome = std::visit(
            [&](auto& curve) {
                auto* keyframes = addressedKeyframes(curve, edit.key.component);
                if (keyframes == nullptr)
                    return wholeValueAddressRejected();
                const auto at = std::ranges::find(*keyframes, edit.key.keyframeId,
                                                  &document::ScalarKeyframe::id);
                if (at == keyframes->end())
                    return invalidKeyframe(edit.key.keyframeId);
                const auto index = static_cast<std::size_t>(at - keyframes->begin());
                if (edit.outgoing.has_value()) {
                    if (index + 1 == keyframes->size())
                        return OperationResult::rejected(
                            OperationIssueCode::InvalidValue,
                            "The final keyframe has no outgoing segment to shape");
                    at->outgoingHandle = *edit.outgoing;
                    at->outgoingInterpolation = document::KeyframeInterpolation::EaseInOut;
                }
                if (edit.incoming.has_value()) {
                    if (index == 0)
                        return OperationResult::rejected(
                            OperationIssueCode::InvalidValue,
                            "The first keyframe has no incoming segment to shape");
                    at->incomingHandle = *edit.incoming;
                    (*keyframes)[index - 1].outgoingInterpolation =
                        document::KeyframeInterpolation::EaseInOut;
                }
                return OperationResult::applied();
            },
            edits.at(edit.key.curveId));
        if (outcome.status == OperationStatus::Rejected)
            return outcome;
    }
    return publishCurves(*composition, std::move(edits));
}

std::string_view SetKeyframeValues::typeId() const noexcept {
    return "bloom.animation.set-keyframe-values";
}
OperationResult SetKeyframeValues::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return invalidComposition(composition_);
    std::vector<KeyframeAddress> addresses;
    addresses.reserve(values_.size());
    for (const auto& edit : values_)
        addresses.push_back(edit.key);
    CurveEdits edits;
    auto staged = stageKeys(*composition, addresses, edits);
    if (staged.status == OperationStatus::Rejected)
        return staged;
    // Admit the WHOLE batch before writing any of it: a value gesture over several curves either
    // lands completely or leaves the document untouched, so a refusal cannot publish half a drag.
    for (const auto& edit : values_) {
        const bool admitted = std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    return !edit.key.component.has_value() &&
                           validValueForCurve<Curve>(*composition, edit.key.curveId, edit.value);
                } else {
                    // A component's domain is the schema's own: only a colour alpha narrows, to
                    // [0, 1], exactly as a pasted component key already is.
                    return edit.key.component.has_value() && std::isfinite(edit.value) &&
                           (*edit.key.component != document::AnimationComponent::Alpha ||
                            (edit.value >= 0.0 && edit.value <= 1.0));
                }
            },
            edits.at(edit.key.curveId));
        if (!admitted)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Keyframe value is outside its parameter's domain");
    }
    for (const auto& edit : values_) {
        auto outcome = std::visit(
            [&](auto& curve) {
                auto* keyframes = addressedKeyframes(curve, edit.key.component);
                if (keyframes == nullptr)
                    return wholeValueAddressRejected();
                const auto at = std::ranges::find(*keyframes, edit.key.keyframeId,
                                                  &document::ScalarKeyframe::id);
                if (at == keyframes->end())
                    return invalidKeyframe(edit.key.keyframeId);
                at->value = edit.value;
                return OperationResult::applied();
            },
            edits.at(edit.key.curveId));
        if (outcome.status == OperationStatus::Rejected)
            return outcome;
    }
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
            paste.time >= composition->duration() || !validInterpolation(paste.interpolation) ||
            !document::isValidKeyframeHandle(paste.outgoingHandle) ||
            !document::isValidKeyframeHandle(paste.incomingHandle))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid pasted key time, interpolation or handle");
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
                std::visit(
                    [](auto& curve) {
                        curve.keyframes.clear();
                        using Curve = std::decay_t<decltype(curve)>;
                        if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                            for (auto& component : curve.components)
                                component.keyframes.clear();
                        }
                    },
                    edits.at(curveId));
        }
        auto result = std::visit(
            [&](auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    const auto* value = std::get_if<double>(&paste.value);
                    if (!value || !validValueForCurve<Curve>(*composition, curveId, *value))
                        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                         "Pasted value does not match parameter");
                    if (std::ranges::any_of(curve.keyframes, [&](const auto& key) {
                            return key.time == paste.time;
                        }))
                        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                         "Paste time is occupied");
                    const auto id = draft.ids().allocateKeyframe();
                    if (!id)
                        return exhaustedIds();
                    curve.keyframes.push_back({*id, paste.time, *value, paste.interpolation,
                                               paste.outgoingHandle, paste.incomingHandle});
                    return OperationResult::applied();
                } else if (paste.component.has_value()) {
                    auto* component = curve.component(*paste.component);
                    const auto* value = std::get_if<double>(&paste.value);
                    if (component == nullptr || value == nullptr || !std::isfinite(*value) ||
                        (*paste.component == document::AnimationComponent::Alpha &&
                         (*value < 0.0 || *value > 1.0)))
                        return OperationResult::rejected(
                            OperationIssueCode::InvalidValue,
                            "Pasted component value does not match parameter");
                    if (std::ranges::any_of(component->keyframes, [&](const auto& key) {
                            return key.time == paste.time;
                        }))
                        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                         "Paste time is occupied");
                    const auto id = draft.ids().allocateKeyframe();
                    if (!id)
                        return exhaustedIds();
                    component->keyframes.push_back({*id, paste.time, *value, paste.interpolation,
                                                    paste.outgoingHandle, paste.incomingHandle});
                    return OperationResult::applied();
                } else if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>) {
                    const auto* value = std::get_if<document::Vec2d>(&paste.value);
                    if (value == nullptr ||
                        !validValueForCurve<Curve>(*composition, curveId, *value))
                        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                         "Pasted value does not match parameter");
                    if (std::ranges::any_of(curve.components, [&](const auto& component) {
                            return std::ranges::any_of(component.keyframes, [&](const auto& key) {
                                return key.time == paste.time;
                            });
                        }))
                        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                         "Paste time is occupied");
                    const auto ids =
                        std::array{draft.ids().allocateKeyframe(), draft.ids().allocateKeyframe()};
                    if (!ids[0] || !ids[1])
                        return exhaustedIds();
                    curve.components[0].keyframes.push_back(
                        {*ids[0], paste.time, value->x, paste.interpolation});
                    curve.components[1].keyframes.push_back(
                        {*ids[1], paste.time, value->y, paste.interpolation});
                    return OperationResult::applied();
                } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
                    const auto* value = std::get_if<document::Vec3d>(&paste.value);
                    if (value == nullptr ||
                        !validValueForCurve<Curve>(*composition, curveId, *value))
                        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                         "Pasted value does not match parameter");
                    if (std::ranges::any_of(curve.components, [&](const auto& component) {
                            return std::ranges::any_of(component.keyframes, [&](const auto& key) {
                                return key.time == paste.time;
                            });
                        }))
                        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                         "Paste time is occupied");
                    const auto ids =
                        std::array{draft.ids().allocateKeyframe(), draft.ids().allocateKeyframe(),
                                   draft.ids().allocateKeyframe()};
                    if (!ids[0] || !ids[1] || !ids[2])
                        return exhaustedIds();
                    const auto values = std::array{value->x, value->y, value->z};
                    for (std::size_t index = 0; index < values.size(); ++index)
                        curve.components[index].keyframes.push_back(
                            {*ids[index], paste.time, values[index], paste.interpolation});
                    return OperationResult::applied();
                } else {
                    const auto* value = std::get_if<core::Color4d>(&paste.value);
                    if (value == nullptr ||
                        !validValueForCurve<Curve>(*composition, curveId, *value))
                        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                         "Pasted value does not match parameter");
                    if (std::ranges::any_of(curve.components, [&](const auto& component) {
                            return std::ranges::any_of(component.keyframes, [&](const auto& key) {
                                return key.time == paste.time;
                            });
                        }))
                        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                                         "Paste time is occupied");
                    const auto ids =
                        std::array{draft.ids().allocateKeyframe(), draft.ids().allocateKeyframe(),
                                   draft.ids().allocateKeyframe(), draft.ids().allocateKeyframe()};
                    if (!ids[0] || !ids[1] || !ids[2] || !ids[3])
                        return exhaustedIds();
                    const auto values =
                        std::array{value->red, value->green, value->blue, value->alpha};
                    for (std::size_t index = 0; index < values.size(); ++index)
                        curve.components[index].keyframes.push_back(
                            {*ids[index], paste.time, values[index], paste.interpolation});
                    return OperationResult::applied();
                }
            },
            edits.at(curveId));
        if (result.status == OperationStatus::Rejected)
            return result;
    }
    return publishCurves(*composition, std::move(edits));
}

} // namespace bloom::commands
