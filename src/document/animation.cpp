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

using bloom::document::AnimationComponent;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveRecord;
using bloom::document::Color4AnimationCurve;
using bloom::document::ComponentAnimationCurve;
using bloom::document::KeyframeId;
using bloom::document::KeyframeInterpolation;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2AnimationCurve;
using bloom::document::Vec3AnimationCurve;

[[nodiscard]] bool validInterpolation(const KeyframeInterpolation interpolation) noexcept {
    return interpolation == KeyframeInterpolation::Hold ||
           interpolation == KeyframeInterpolation::Linear ||
           interpolation == KeyframeInterpolation::EaseInOut;
}

// "Value is representable" per curve kind. A color key reuses core::Color4d::isValid() rather than
// a fourth hand-written finiteness test, so an animated authoring color is admitted on exactly the
// terms a constant one is.
[[nodiscard]] bool finiteValue(const ScalarKeyframe& keyframe) noexcept {
    return std::isfinite(keyframe.value);
}

[[nodiscard]] bool finiteValue(const bloom::document::Vec2Keyframe& keyframe) noexcept {
    return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y);
}

[[nodiscard]] bool finiteValue(const bloom::document::Vec3Keyframe& keyframe) noexcept {
    return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y) &&
           std::isfinite(keyframe.value.z);
}

[[nodiscard]] bool finiteValue(const bloom::document::Color4Keyframe& keyframe) noexcept {
    return keyframe.value.isValid();
}

// Ease handles exist only on the scalar/component key. For the whole-value projections the
// predicate is vacuously true, so every keyframe admission path can ask it unconditionally.
template <typename Keyframe> [[nodiscard]] bool validHandles(const Keyframe& keyframe) noexcept {
    if constexpr (requires { keyframe.outgoingHandle; }) {
        return bloom::document::isValidKeyframeHandle(keyframe.outgoingHandle) &&
               bloom::document::isValidKeyframeHandle(keyframe.incomingHandle);
    } else {
        return true;
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
            !validHandles(keyframe) || !validInterpolation(keyframe.outgoingInterpolation)) {
            return false;
        }
        if (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool componentCurveCanEnterStore(const ComponentAnimationCurve& curve) {
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        if (!keyframe.id.isValid() || !finiteValue(keyframe) || !validHandles(keyframe) ||
            !validInterpolation(keyframe.outgoingInterpolation) ||
            (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time))) {
            return false;
        }
    }
    return true;
}

template <std::size_t Count>
[[nodiscard]] bool
componentCurveRecordCanEnterStore(const AnimationCurveId id,
                                  const std::array<ComponentAnimationCurve, Count>& components) {
    if (!id.isValid()) {
        return false;
    }
    bool hasKey = false;
    for (const auto& component : components) {
        if (!component.keyframes.empty()) {
            hasKey = true;
        }
        if (!componentCurveCanEnterStore(component)) {
            return false;
        }
    }
    return hasKey;
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
        if (!validHandles(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".handle",
                       "Keyframe handle time must be within [0, 1] and its value offset finite");
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

void validateComponentCurve(const ComponentAnimationCurve& curve, const std::string& path,
                            ValidationResult& result) {
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        const auto keyframePath = path + ".keyframes[" + std::to_string(keyframe.id.value()) + "]";
        if (!keyframe.id.isValid()) {
            result.add(ValidationCode::InvalidId, keyframePath + ".id",
                       "Keyframe ID must not be zero");
        }
        if (!finiteValue(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".value",
                       "Keyframe value must be finite");
        }
        if (!validHandles(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".handle",
                       "Keyframe handle time must be within [0, 1] and its value offset finite");
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
    if (!curve.keyframes.empty() &&
        curve.keyframes.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
        const auto& finalKey = curve.keyframes.back();
        result.add(ValidationCode::InvalidInterpolation,
                   path + ".keyframes[" + std::to_string(finalKey.id.value()) +
                       "].outgoingInterpolation",
                   "The final keyframe interpolation must be canonical Linear");
    }
}

template <typename Curve, typename Keyframe>
[[nodiscard]] bool insertKeyframe(Curve& curve, Keyframe keyframe, const bool idAlreadyExists) {
    if (!keyframe.id.isValid() || idAlreadyExists || !finiteValue(keyframe) ||
        !validHandles(keyframe) || !validInterpolation(keyframe.outgoingInterpolation)) {
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
    if (!keyframe.id.isValid() || !finiteValue(keyframe) || !validHandles(keyframe) ||
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

[[nodiscard]] bool isVec2(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Vec2AnimationCurve>(record);
}

[[nodiscard]] bool isVec3(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Vec3AnimationCurve>(record);
}

[[nodiscard]] bool isColor4(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Color4AnimationCurve>(record);
}

} // namespace

namespace bloom::document {

bool isValidKeyframeHandle(const KeyframeHandle& handle) noexcept {
    return std::isfinite(handle.value) && handle.time >= 0.0 && handle.time <= 1.0;
}

namespace {

template <std::size_t Count>
[[nodiscard]] const ComponentAnimationCurve*
componentAt(const std::array<ComponentAnimationCurve, Count>& components,
            const AnimationComponent component, const std::array<AnimationComponent, Count> names) {
    for (std::size_t index = 0; index < Count; ++index) {
        if (names[index] == component) {
            return &components[index];
        }
    }
    return nullptr;
}

template <std::size_t Count>
[[nodiscard]] ComponentAnimationCurve*
componentAt(std::array<ComponentAnimationCurve, Count>& components,
            const AnimationComponent component, const std::array<AnimationComponent, Count> names) {
    for (std::size_t index = 0; index < Count; ++index) {
        if (names[index] == component) {
            return &components[index];
        }
    }
    return nullptr;
}

} // namespace

const ComponentAnimationCurve*
Vec2AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::X, AnimationComponent::Y});
}

ComponentAnimationCurve*
Vec2AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::X, AnimationComponent::Y});
}

const ComponentAnimationCurve*
Vec3AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(
        components, component,
        std::array{AnimationComponent::X, AnimationComponent::Y, AnimationComponent::Z});
}

ComponentAnimationCurve*
Vec3AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(
        components, component,
        std::array{AnimationComponent::X, AnimationComponent::Y, AnimationComponent::Z});
}

const ComponentAnimationCurve*
Color4AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::Red, AnimationComponent::Green,
                                  AnimationComponent::Blue, AnimationComponent::Alpha});
}

ComponentAnimationCurve*
Color4AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::Red, AnimationComponent::Green,
                                  AnimationComponent::Blue, AnimationComponent::Alpha});
}

AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept {
    if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
        return scalar->id;
    }
    if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
        return vector->id;
    }
    if (const auto* vector = std::get_if<Vec3AnimationCurve>(&record)) {
        return vector->id;
    }
    if (const auto* color = std::get_if<Color4AnimationCurve>(&record)) {
        return color->id;
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

const Vec3AnimationCurve* AnimationCurveStore::findVec3(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Vec3AnimationCurve>(record);
}

const Color4AnimationCurve*
AnimationCurveStore::findColor4(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Color4AnimationCurve>(record);
}

const ComponentAnimationCurve*
AnimationCurveStore::findComponent(const AnimationCurveId id,
                                   const AnimationComponent component) const noexcept {
    const auto* record = find(id);
    if (record == nullptr) {
        return nullptr;
    }
    if (const auto* vector = std::get_if<Vec2AnimationCurve>(record)) {
        return vector->component(component);
    }
    if (const auto* vector = std::get_if<Vec3AnimationCurve>(record)) {
        return vector->component(component);
    }
    if (const auto* color = std::get_if<Color4AnimationCurve>(record)) {
        return color->component(component);
    }
    return nullptr;
}

bool AnimationCurveStore::insert(AnimationCurveRecord record) {
    const bool valid = std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>) {
                return curveCanEnterStore(curve);
            } else {
                if (std::ranges::all_of(
                        curve.components,
                        [](const auto& component) { return component.keyframes.empty(); }) &&
                    !curve.keyframes.empty()) {
                    return curveCanEnterStore(curve);
                }
                return componentCurveRecordCanEnterStore(curve.id, curve.components);
            }
        },
        record);
    const auto id = animationCurveId(record);
    if (!valid || find(id) != nullptr) {
        return false;
    }
    const bool hasDuplicateKeyframe = std::visit(
        [&](const auto& curve) {
            if constexpr (std::is_same_v<std::decay_t<decltype(curve)>, ScalarAnimationCurve>) {
                return std::ranges::any_of(curve.keyframes, [&](const auto& keyframe) {
                    return containsKeyframe(keyframe.id);
                });
            } else {
                if (std::ranges::all_of(curve.components, [](const auto& component) {
                        return component.keyframes.empty();
                    })) {
                    return std::ranges::any_of(curve.keyframes, [&](const auto& keyframe) {
                        return containsKeyframe(keyframe.id);
                    });
                }
                return std::ranges::any_of(curve.components, [&](const auto& component) {
                    return std::ranges::any_of(component.keyframes, [&](const auto& keyframe) {
                        return containsKeyframe(keyframe.id);
                    });
                });
            }
        },
        record);

    if (hasDuplicateKeyframe) {
        return false;
    }
    std::visit(
        [](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>) {
                normalizeFinalInterpolation(curve);
            } else {
                if (std::ranges::all_of(curve.components, [](const auto& component) {
                        return component.keyframes.empty();
                    })) {
                    normalizeFinalInterpolation(curve);
                } else {
                    for (auto& component : curve.components) {
                        normalizeFinalInterpolation(component);
                    }
                }
            }
        },
        record);
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
        if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
            if (std::ranges::all_of(vector->components, [](const auto& component) {
                    return component.keyframes.empty();
                })) {
                return contains(*vector);
            }
            return std::ranges::any_of(vector->components, contains);
        }
        if (const auto* vector = std::get_if<Vec3AnimationCurve>(&record)) {
            if (std::ranges::all_of(vector->components, [](const auto& component) {
                    return component.keyframes.empty();
                })) {
                return contains(*vector);
            }
            return std::ranges::any_of(vector->components, contains);
        }
        const auto* color = std::get_if<Color4AnimationCurve>(&record);
        if (color == nullptr) {
            return false;
        }
        if (std::ranges::all_of(color->components, [](const auto& component) {
                return component.keyframes.empty();
            })) {
            return contains(*color);
        }
        return std::ranges::any_of(color->components, contains);
    });
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    return curve != nullptr && ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframeId));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, Vec2Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
    return curve != nullptr && curve->components[0].keyframes.empty() &&
           curve->components[1].keyframes.empty() &&
           ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframe.id));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, Vec3Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec3AnimationCurve>(record);
    return curve != nullptr &&
           std::ranges::all_of(curve->components,
                               [](const auto& component) { return component.keyframes.empty(); }) &&
           ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframe.id));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, Color4Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Color4AnimationCurve>(record);
    return curve != nullptr &&
           std::ranges::all_of(curve->components,
                               [](const auto& component) { return component.keyframes.empty(); }) &&
           ::insertKeyframe(*curve, keyframe, containsKeyframe(keyframe.id));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId,
                                         const AnimationComponent component,
                                         ScalarKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    if (record == nullptr || containsKeyframe(keyframeId)) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<Curve, ScalarAnimationCurve>) {
                curve = value.component(component);
            }
        },
        *record);
    return curve != nullptr && ::insertKeyframe(*curve, keyframe, false);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, Vec2Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
    return curve != nullptr &&
           std::ranges::all_of(curve->components,
                               [](const auto& component) { return component.keyframes.empty(); }) &&
           ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, Vec3Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Vec3AnimationCurve>(record);
    return curve != nullptr &&
           std::ranges::all_of(curve->components,
                               [](const auto& component) { return component.keyframes.empty(); }) &&
           ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, Color4Keyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<Color4AnimationCurve>(record);
    return curve != nullptr &&
           std::ranges::all_of(curve->components,
                               [](const auto& component) { return component.keyframes.empty(); }) &&
           ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId,
                                         const AnimationComponent component,
                                         ScalarKeyframe keyframe) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<Curve, ScalarAnimationCurve>) {
                curve = value.component(component);
            }
        },
        *record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::eraseKeyframe(const AnimationCurveId curveId,
                                        const KeyframeId keyframeId) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    if (auto* scalar = std::get_if<ScalarAnimationCurve>(record)) {
        return eraseCurveKeyframe(*scalar, keyframeId);
    }
    const bool legacy = std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>)
                return false;
            else
                return std::ranges::all_of(curve.components, [](const auto& component) {
                    return component.keyframes.empty();
                });
        },
        *record);
    if (legacy) {
        return std::visit(
            [&](auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (!std::is_same_v<Curve, ScalarAnimationCurve>)
                    return eraseCurveKeyframe(curve, keyframeId);
                return false;
            },
            *record);
    }
    bool erased = false;
    std::visit(
        [&](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (!std::is_same_v<Curve, ScalarAnimationCurve>) {
                for (auto& component : curve.components) {
                    if (std::ranges::find(component.keyframes, keyframeId, &ScalarKeyframe::id) !=
                        component.keyframes.end()) {
                        erased = eraseCurveKeyframe(component, keyframeId);
                        return;
                    }
                }
            }
        },
        *record);
    return erased;
}

bool AnimationCurveStore::synchronizeCompatibilityProjection(const AnimationCurveId curveId) {
    auto* record = findMutable(curveId);
    if (record == nullptr)
        return false;
    return std::visit(
        [](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>) {
                return false;
            } else {
                if (std::ranges::all_of(curve.components, [](const auto& component) {
                        return component.keyframes.empty();
                    }))
                    return false;
                std::vector<core::RationalTime> times;
                for (const auto& component : curve.components)
                    for (const auto& key : component.keyframes)
                        if (std::ranges::find(times, key.time) == times.end())
                            times.push_back(key.time);
                std::ranges::sort(times);
                if constexpr (std::is_same_v<Curve, Vec2AnimationCurve>) {
                    curve.keyframes.clear();
                    for (const auto time : times) {
                        const auto x = std::ranges::find(curve.components[0].keyframes, time,
                                                         &ScalarKeyframe::time);
                        const auto y = std::ranges::find(curve.components[1].keyframes, time,
                                                         &ScalarKeyframe::time);
                        if (x == curve.components[0].keyframes.end() &&
                            y == curve.components[1].keyframes.end())
                            continue;
                        const auto id = x != curve.components[0].keyframes.end() ? x->id : y->id;
                        const auto interpolation = x != curve.components[0].keyframes.end()
                                                       ? x->outgoingInterpolation
                                                       : y->outgoingInterpolation;
                        curve.keyframes.push_back(
                            {id,
                             time,
                             {x == curve.components[0].keyframes.end() ? 0.0 : x->value,
                              y == curve.components[1].keyframes.end() ? 0.0 : y->value},
                             interpolation});
                    }
                } else if constexpr (std::is_same_v<Curve, Vec3AnimationCurve>) {
                    curve.keyframes.clear();
                    for (const auto time : times) {
                        const auto x = std::ranges::find(curve.components[0].keyframes, time,
                                                         &ScalarKeyframe::time);
                        const auto y = std::ranges::find(curve.components[1].keyframes, time,
                                                         &ScalarKeyframe::time);
                        const auto z = std::ranges::find(curve.components[2].keyframes, time,
                                                         &ScalarKeyframe::time);
                        if (x == curve.components[0].keyframes.end() &&
                            y == curve.components[1].keyframes.end() &&
                            z == curve.components[2].keyframes.end())
                            continue;
                        const auto id = x != curve.components[0].keyframes.end()   ? x->id
                                        : y != curve.components[1].keyframes.end() ? y->id
                                                                                   : z->id;
                        const auto interpolation =
                            x != curve.components[0].keyframes.end()   ? x->outgoingInterpolation
                            : y != curve.components[1].keyframes.end() ? y->outgoingInterpolation
                                                                       : z->outgoingInterpolation;
                        curve.keyframes.push_back(
                            {id,
                             time,
                             {x == curve.components[0].keyframes.end() ? 0.0 : x->value,
                              y == curve.components[1].keyframes.end() ? 0.0 : y->value,
                              z == curve.components[2].keyframes.end() ? 0.0 : z->value},
                             interpolation});
                    }
                } else {
                    curve.keyframes.clear();
                    for (const auto time : times) {
                        const auto red = std::ranges::find(curve.components[0].keyframes, time,
                                                           &ScalarKeyframe::time);
                        const auto green = std::ranges::find(curve.components[1].keyframes, time,
                                                             &ScalarKeyframe::time);
                        const auto blue = std::ranges::find(curve.components[2].keyframes, time,
                                                            &ScalarKeyframe::time);
                        const auto alpha = std::ranges::find(curve.components[3].keyframes, time,
                                                             &ScalarKeyframe::time);
                        if (red == curve.components[0].keyframes.end() &&
                            green == curve.components[1].keyframes.end() &&
                            blue == curve.components[2].keyframes.end() &&
                            alpha == curve.components[3].keyframes.end())
                            continue;
                        const auto id = red != curve.components[0].keyframes.end()     ? red->id
                                        : green != curve.components[1].keyframes.end() ? green->id
                                        : blue != curve.components[2].keyframes.end()  ? blue->id
                                                                                       : alpha->id;
                        const auto interpolation = red != curve.components[0].keyframes.end()
                                                       ? red->outgoingInterpolation
                                                   : green != curve.components[1].keyframes.end()
                                                       ? green->outgoingInterpolation
                                                   : blue != curve.components[2].keyframes.end()
                                                       ? blue->outgoingInterpolation
                                                       : alpha->outgoingInterpolation;
                        curve.keyframes.push_back(
                            {id,
                             time,
                             {red == curve.components[0].keyframes.end() ? 0.0 : red->value,
                              green == curve.components[1].keyframes.end() ? 0.0 : green->value,
                              blue == curve.components[2].keyframes.end() ? 0.0 : blue->value,
                              alpha == curve.components[3].keyframes.end() ? 0.0 : alpha->value},
                             interpolation});
                    }
                }
                return true;
            }
        },
        *record);
}

bool AnimationCurveStore::eraseKeyframe(const AnimationCurveId curveId,
                                        const AnimationComponent component,
                                        const KeyframeId keyframeId) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<Curve, ScalarAnimationCurve>) {
                curve = value.component(component);
            }
        },
        *record);
    if (curve == nullptr || curve->keyframes.empty()) {
        return false;
    }
    const auto found = std::ranges::find(curve->keyframes, keyframeId, &ScalarKeyframe::id);
    if (found == curve->keyframes.end()) {
        return false;
    }
    curve->keyframes.erase(found);
    normalizeFinalInterpolation(*curve);
    // A component's final key may be removed when another component still owns the grouped curve.
    // The command layer erases the grouped record or converts the parameter to a constant when no
    // component remains; the store reports the successful local mutation here.
    return true;
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
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>) {
                    validateCurve(curve, result);
                    for (const auto& keyframe : curve.keyframes) {
                        if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                            result.add(ValidationCode::DuplicateId,
                                       "[" + std::to_string(curve.id.value()) + "].keyframes[" +
                                           std::to_string(keyframe.id.value()) + "].id",
                                       "Keyframe ID is duplicated within the composition");
                        }
                    }
                } else {
                    if (std::ranges::all_of(
                            curve.components,
                            [](const auto& component) { return component.keyframes.empty(); }) &&
                        !curve.keyframes.empty()) {
                        validateCurve(curve, result);
                        for (const auto& keyframe : curve.keyframes) {
                            if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                                result.add(ValidationCode::DuplicateId,
                                           "[" + std::to_string(curve.id.value()) + "].keyframes[" +
                                               std::to_string(keyframe.id.value()) + "].id",
                                           "Keyframe ID is duplicated within the composition");
                            }
                        }
                        return;
                    }
                    for (std::size_t componentIndex = 0; componentIndex < curve.components.size();
                         ++componentIndex) {
                        const auto path = "[" + std::to_string(curve.id.value()) + "].components[" +
                                          std::to_string(componentIndex) + "]";
                        validateComponentCurve(curve.components[componentIndex], path, result);
                        for (const auto& keyframe : curve.components[componentIndex].keyframes) {
                            if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                                result.add(ValidationCode::DuplicateId,
                                           path + ".keyframes[" +
                                               std::to_string(keyframe.id.value()) + "].id",
                                           "Keyframe ID is duplicated within the composition");
                            }
                        }
                    }
                    if (!std::ranges::any_of(curve.components, [](const auto& item) {
                            return !item.keyframes.empty();
                        })) {
                        result.add(ValidationCode::InvalidValue,
                                   "[" + std::to_string(curve.id.value()) + "].components",
                                   "Component animation curve must contain at least one keyframe");
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

        // Animatability and curve kind both come from the shared schema predicates in
        // bloom/document/parameter.hpp, so this validation and the animation commands cannot
        // disagree about which parameters may be animated.
        if (isVec2AnimatableSchemaKey(parameter.schemaKey)) {
            if (!isVec2(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "This transform parameter requires a Vec2 animation curve");
            }
        } else if (isVec3AnimatableSchemaKey(parameter.schemaKey)) {
            if (!isVec3(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "This value parameter requires a Vec3 animation curve");
            }
        } else if (isScalarAnimatableSchemaKey(parameter.schemaKey)) {
            if (!isScalar(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "This scalar parameter requires a scalar animation curve");
            }
        } else if (isColor4AnimatableSchemaKey(parameter.schemaKey)) {
            if (!isColor4(*curve)) {
                result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                           "This color parameter requires a Color4 animation curve");
            }
        } else {
            result.add(ValidationCode::InvalidValue, parameterPath + ".source",
                       "This parameter schema does not support animation");
        }

        // A scalar DOMAIN belongs to the schema, not to scalar curves in general: opacity is
        // confined to [0, 1] and text size to (0, kMaximumTextSizePixels], while a rotation curve
        // measures degrees and must be free to wind past a full turn in either direction. The one
        // gate is isScalarWithinSchemaDomain() so a key and a constant are admitted identically.
        if (const auto* scalar = std::get_if<ScalarAnimationCurve>(curve)) {
            for (const auto& keyframe : scalar->keyframes) {
                if (!isScalarWithinSchemaDomain(parameter.schemaKey, keyframe.value)) {
                    result.add(ValidationCode::InvalidValue,
                               "animationCurves[" + std::to_string(scalar->id.value()) +
                                   "].keyframes[" + std::to_string(keyframe.id.value()) + "].value",
                               "Keyframe value is outside the domain its schema declares");
                }
            }
        } else if (const auto* color = std::get_if<Color4AnimationCurve>(curve)) {
            if (const auto* alpha = color->component(AnimationComponent::Alpha)) {
                for (const auto& keyframe : alpha->keyframes) {
                    if (keyframe.value < 0.0 || keyframe.value > 1.0) {
                        result.add(ValidationCode::InvalidValue,
                                   "animationCurves[" + std::to_string(color->id.value()) +
                                       "].components[3].keyframes[" +
                                       std::to_string(keyframe.id.value()) + "].value",
                                   "Alpha keyframe value is outside the color domain");
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
