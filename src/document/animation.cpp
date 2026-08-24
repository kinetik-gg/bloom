#include <bloom/document/animation.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveRecord;
using bloom::document::KeyframeId;
using bloom::document::KeyframeInterpolation;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2AnimationCurve;
using bloom::document::Vec2Keyframe;

[[nodiscard]] bool validInterpolation(const KeyframeInterpolation interpolation) noexcept {
    return interpolation == KeyframeInterpolation::Hold ||
           interpolation == KeyframeInterpolation::Linear;
}

template <typename Keyframe> [[nodiscard]] bool finiteValue(const Keyframe& keyframe) noexcept {
    if constexpr (std::is_same_v<Keyframe, ScalarKeyframe>) {
        return std::isfinite(keyframe.value);
    } else {
        return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y);
    }
}

template <typename Curve> void normalizeFinalInterpolation(Curve& curve) noexcept {
    if (!curve.keyframes.empty()) {
        curve.keyframes.back().outgoingInterpolation = KeyframeInterpolation::Linear;
    }
}

template <typename Curve> [[nodiscard]] bool curveCanEnterStore(const Curve& curve) {
    if (!curve.id.isValid() || curve.keyframes.empty()) {
        return false;
    }

    std::unordered_set<KeyframeId> ids;
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        if (!keyframe.id.isValid() || !ids.insert(keyframe.id).second || !finiteValue(keyframe) ||
            !validInterpolation(keyframe.outgoingInterpolation)) {
            return false;
        }
        if (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time)) {
            return false;
        }
    }
    return true;
}

template <typename Curve> void validateCurve(const Curve& curve, ValidationResult& result) {
    const auto curvePath = "[" + std::to_string(curve.id.value()) + "]";
    if (!curve.id.isValid()) {
        result.add(ValidationCode::InvalidId, curvePath + ".id",
                   "Animation curve ID must not be zero");
    }
    if (curve.keyframes.empty()) {
        result.add(ValidationCode::InvalidValue, curvePath + ".keyframes",
                   "Animation curve must contain at least one keyframe");
        return;
    }

    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        const auto keyframePath =
            curvePath + ".keyframes[" + std::to_string(keyframe.id.value()) + "]";
        if (!keyframe.id.isValid()) {
            result.add(ValidationCode::InvalidId, keyframePath + ".id",
                       "Keyframe ID must not be zero");
        }
        if (!finiteValue(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".value",
                       "Keyframe value must be finite");
        }
        if (!validInterpolation(keyframe.outgoingInterpolation)) {
            result.add(ValidationCode::InvalidInterpolation,
                       keyframePath + ".outgoingInterpolation",
                       "Keyframe interpolation mode is unsupported");
        }
        if (index > 0) {
            const auto& previous = curve.keyframes[index - 1];
            if (previous.time == keyframe.time) {
                result.add(ValidationCode::DuplicateTime, keyframePath + ".time",
                           "Animation curve contains duplicate exact keyframe times");
            } else if (keyframe.time < previous.time) {
                result.add(ValidationCode::InvalidOrder, keyframePath + ".time",
                           "Animation keyframes must be ordered by increasing exact time");
            }
        }
    }
    if (curve.keyframes.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
        const auto& finalKey = curve.keyframes.back();
        result.add(ValidationCode::InvalidInterpolation,
                   curvePath + ".keyframes[" + std::to_string(finalKey.id.value()) +
                       "].outgoingInterpolation",
                   "The final keyframe interpolation must be canonical Linear");
    }
}

template <typename Curve, typename Keyframe>
[[nodiscard]] bool insertKeyframe(Curve& curve, Keyframe keyframe, const bool idAlreadyExists) {
    if (!keyframe.id.isValid() || idAlreadyExists || !finiteValue(keyframe) ||
        !validInterpolation(keyframe.outgoingInterpolation)) {
        return false;
    }
    const auto position = std::lower_bound(
        curve.keyframes.begin(), curve.keyframes.end(), keyframe.time,
        [](const auto& candidate, const auto time) { return candidate.time < time; });
    if (position != curve.keyframes.end() && position->time == keyframe.time) {
        return false;
    }
    curve.keyframes.insert(position, keyframe);
    normalizeFinalInterpolation(curve);
    return true;
}

template <typename Curve, typename Keyframe>
[[nodiscard]] bool updateKeyframe(Curve& curve, Keyframe keyframe) {
    if (!keyframe.id.isValid() || !finiteValue(keyframe) ||
        !validInterpolation(keyframe.outgoingInterpolation)) {
        return false;
    }
    const auto current = std::ranges::find(curve.keyframes, keyframe.id, &Keyframe::id);
    if (current == curve.keyframes.end()) {
        return false;
    }
    const auto occupied = std::ranges::find(curve.keyframes, keyframe.time, &Keyframe::time);
    if (occupied != curve.keyframes.end() && occupied->id != keyframe.id) {
        return false;
    }

    *current = keyframe;
    std::ranges::sort(curve.keyframes, {}, &Keyframe::time);
    normalizeFinalInterpolation(curve);
    return true;
}

template <typename Curve>
[[nodiscard]] bool eraseCurveKeyframe(Curve& curve, const KeyframeId keyframeId) {
    if (curve.keyframes.size() <= 1) {
        return false;
    }
    const auto keyframe =
        std::ranges::find_if(curve.keyframes, [keyframeId](const auto& candidate) {
            return candidate.id == keyframeId;
        });
    if (keyframe == curve.keyframes.end()) {
        return false;
    }
    curve.keyframes.erase(keyframe);
    normalizeFinalInterpolation(curve);
    return true;
}

[[nodiscard]] bool isScalar(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<ScalarAnimationCurve>(record);
}

} // namespace

namespace bloom::document {

AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept {
    if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
        return scalar->id;
    }
    if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
        return vector->id;
    }
    return {};
}

const AnimationCurveRecord* AnimationCurveStore::find(const AnimationCurveId id) const noexcept {
    const auto record = std::lower_bound(records_.begin(), records_.end(), id,
                                         [](const auto& candidate, const auto curveId) {
                                             return animationCurveId(candidate) < curveId;
                                         });
    return record == records_.end() || animationCurveId(*record) != id ? nullptr : &*record;
}

AnimationCurveRecord* AnimationCurveStore::findMutable(const AnimationCurveId id) noexcept {
    const auto record = std::lower_bound(records_.begin(), records_.end(), id,
                                         [](const auto& candidate, const auto curveId) {
                                             return animationCurveId(candidate) < curveId;
                                         });
    return record == records_.end() || animationCurveId(*record) != id ? nullptr : &*record;
}

const ScalarAnimationCurve*
AnimationCurveStore::findScalar(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
}

const Vec2AnimationCurve* AnimationCurveStore::findVec2(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
}

bool AnimationCurveStore::insert(AnimationCurveRecord record) {
    const bool valid =
        std::visit([](const auto& curve) { return curveCanEnterStore(curve); }, record);
    const auto id = animationCurveId(record);
    if (!valid || find(id) != nullptr) {
        return false;
    }
    const bool hasDuplicateKeyframe = std::visit(
        [&](const auto& curve) {
            return std::ranges::any_of(curve.keyframes, [&](const auto& keyframe) {
                return containsKeyframe(keyframe.id);
            });
        },
        record);
    if (hasDuplicateKeyframe) {
        return false;
    }
    std::visit([](auto& curve) { normalizeFinalInterpolation(curve); }, record);
    const auto position = std::lower_bound(records_.begin(), records_.end(), id,
                                           [](const auto& candidate, const auto curveId) {
                                               return animationCurveId(candidate) < curveId;
                                           });
    records_.insert(position, std::move(record));
    return true;
}

bool AnimationCurveStore::erase(const AnimationCurveId id) {
    const auto record = std::lower_bound(records_.begin(), records_.end(), id,
                                         [](const auto& candidate, const auto curveId) {
                                             return animationCurveId(candidate) < curveId;
                                         });
    if (record == records_.end() || animationCurveId(*record) != id) {
        return false;
    }
    records_.erase(record);
    return true;
}

bool AnimationCurveStore::containsKeyframe(const KeyframeId id) const noexcept {
    return std::ranges::any_of(records_, [id](const auto& record) {
        const auto contains = [id](const auto& curve) {
            return std::ranges::any_of(curve.keyframes,
                                       [id](const auto& keyframe) { return keyframe.id == id; });
        };
        if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
            return contains(*scalar);
        }
        const auto* vector = std::get_if<Vec2AnimationCurve>(&record);
        return vector != nullptr && contains(*vector);
    });
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    return curve != nullptr && ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframeId));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, Vec2Keyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
    return curve != nullptr && ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframeId));
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, Vec2Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::eraseKeyframe(const AnimationCurveId curveId,
                                        const KeyframeId keyframeId) {
    auto* record = findMutable(curveId);
    return record != nullptr &&
           std::visit([keyframeId](auto& curve) { return eraseCurveKeyframe(curve, keyframeId); },
                      *record);
}

ValidationResult AnimationCurveStore::validate() const {
    ValidationResult result;
    std::unordered_set<AnimationCurveId> curveIds;
    std::unordered_set<KeyframeId> keyframeIds;
    AnimationCurveId previousId;
    for (const auto& record : records_) {
        const auto id = animationCurveId(record);
        if (!curveIds.insert(id).second) {
            result.add(ValidationCode::DuplicateId, "[" + std::to_string(id.value()) + "].id",
                       "Animation curve ID is duplicated");
        }
        if (previousId.isValid() && id < previousId) {
            result.add(ValidationCode::InvalidOrder, "[" + std::to_string(id.value()) + "].id",
                       "Animation curves must be ordered by ID");
        }
        previousId = id;
        std::visit(
            [&](const auto& curve) {
                validateCurve(curve, result);
                for (const auto& keyframe : curve.keyframes) {
                    if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                        result.add(ValidationCode::DuplicateId,
                                   "[" + std::to_string(curve.id.value()) + "].keyframes[" +
                                       std::to_string(keyframe.id.value()) + "].id",
                                   "Keyframe ID is duplicated within the composition");
                    }
                }
            },
            record);
    }
    return result;
}

ValidationResult validateAnimationCurveReferences(const ParameterStore& parameters,
                                                  const AnimationCurveStore& animationCurves) {
    ValidationResult result;
    std::unordered_map<AnimationCurveId, ParameterId> owners;

    for (const auto& parameter : parameters.records()) {
        const auto* source = std::get_if<AnimationCurveSource>(&parameter.source);
        if (source == nullptr) {
            continue;
        }
        const auto parameterPath = "parameters[" + std::to_string(parameter.id.value()) + "]";
        const auto* curve = animationCurves.find(source->curveId);
        if (curve == nullptr) {
            result.add(ValidationCode::MissingReference, parameterPath + ".source.curveId",
                       "Animation source does not resolve to a curve in this composition");
            continue;
        }

        const auto [owner, inserted] = owners.emplace(source->curveId, parameter.id);
        if (!inserted && owner->second != parameter.id) {
            result.add(ValidationCode::SharedReference, parameterPath + ".source.curveId",
                       "Animation curve may be owned by only one parameter");
        }

        if (parameter.schemaKey == kPositionParameterSchemaKey) {
            if (isScalar(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "Position parameter requires a Vec2 animation curve");
            }
        } else if (parameter.schemaKey == kOpacityParameterSchemaKey) {
            if (!isScalar(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "Opacity parameter requires a scalar animation curve");
            }
        } else {
            result.add(ValidationCode::InvalidValue, parameterPath + ".source",
                       "This parameter schema does not support animation in schema version 1");
        }

        if (parameter.schemaKey == kOpacityParameterSchemaKey) {
            if (const auto* scalar = std::get_if<ScalarAnimationCurve>(curve)) {
                for (const auto& keyframe : scalar->keyframes) {
                    if (keyframe.value < 0.0 || keyframe.value > 1.0) {
                        result.add(ValidationCode::InvalidValue,
                                   "animationCurves[" + std::to_string(scalar->id.value()) +
                                       "].keyframes[" + std::to_string(keyframe.id.value()) +
                                       "].value",
                                   "Opacity keyframe value must be between zero and one");
                    }
                }
            }
        }
    }

    for (const auto& record : animationCurves.records()) {
        const auto id = animationCurveId(record);
        if (!owners.contains(id)) {
            result.add(ValidationCode::OrphanObject,
                       "animationCurves[" + std::to_string(id.value()) + "]",
                       "Animation curve must be owned by exactly one parameter");
        }
    }
    return result;
}

} // namespace bloom::document
