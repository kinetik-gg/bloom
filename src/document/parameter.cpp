#include <bloom/document/parameter.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

[[nodiscard]] bool isGenericallyValidSource(const bloom::document::ParameterSource& source) {
    return std::visit(
        [](const auto& valueSource) {
            using Source = std::decay_t<decltype(valueSource)>;
            if constexpr (std::is_same_v<Source, bloom::document::ConstantValueSource>) {
                if (const auto* value = std::get_if<double>(&valueSource.value)) {
                    return std::isfinite(*value);
                }
                if (const auto* value = std::get_if<bloom::document::Vec2d>(&valueSource.value)) {
                    return std::isfinite(value->x) && std::isfinite(value->y);
                }
                if (const auto* value = std::get_if<bloom::core::Color4d>(&valueSource.value)) {
                    return value->isValid();
                }
                if (const auto* value = std::get_if<std::string>(&valueSource.value)) {
                    return bloom::core::isValidUtf8(*value);
                }
                return true;
            } else if constexpr (std::is_same_v<Source, bloom::document::AnimationCurveSource>) {
                return valueSource.curveId.isValid();
            } else {
                return valueSource.driverId.isValid();
            }
        },
        source);
}

[[nodiscard]] bool
constantMatchesSchema(const std::string_view schemaKey,
                      const bloom::document::ConstantValueSource& constant) noexcept {
    using namespace bloom::document;
    if (schemaKey == kSolidColorParameterSchemaKey) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    if (schemaKey == kPositionParameterSchemaKey || schemaKey == kAnchorParameterSchemaKey ||
        schemaKey == kScaleParameterSchemaKey) {
        // One rule for all three Vec2d transform values: finite, otherwise unbounded. Scale is
        // deliberately NOT confined to positive numbers -- a negative factor mirrors the axis and
        // zero collapses the layer, both of which evaluation renders rather than refuses -- and
        // anchor is deliberately not confined to the layer's own box, so an off-layer pivot stays
        // authorable.
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y);
    }
    if (schemaKey == kRotationParameterSchemaKey) {
        // Degrees, finite and unbounded: an animated rotation must be able to wind past 360 and
        // below zero, so only a non-finite value is rejected.
        const auto* rotation = std::get_if<double>(&constant.value);
        return rotation != nullptr && std::isfinite(*rotation);
    }
    if (schemaKey == kOpacityParameterSchemaKey) {
        const auto* opacity = std::get_if<double>(&constant.value);
        return opacity != nullptr && std::isfinite(*opacity) && *opacity >= 0.0 && *opacity <= 1.0;
    }
    if (schemaKey == kBlendModeParameterSchemaKey) {
        // A blend mode is a stored integer under core::BlendMode's closed mapping. An integer that
        // names no implemented mode is refused rather than folded to Normal: drawing a different
        // mode than the document asked for would be a silent misrender, and a document from a newer
        // build belongs in the "cannot be interpreted" path, not in a guess.
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && bloom::core::blendModeFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kTextParameterSchemaKey) {
        return std::holds_alternative<std::string>(constant.value);
    }
    if (schemaKey == kTextSizeParameterSchemaKey) {
        const auto* size = std::get_if<double>(&constant.value);
        return size != nullptr && std::isfinite(*size) && *size > 0.0 &&
               *size <= kMaximumTextSizePixels;
    }
    if (schemaKey == kTextColorParameterSchemaKey) {
        // Exactly the solid color rule: a finite straight RGBA authoring value with alpha in [0,
        // 1], unbounded in RGB so an HDR or negative channel survives a round trip.
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    return true;
}

[[nodiscard]] bool isValidSourceForSchema(const std::string_view schemaKey,
                                          const bloom::document::ParameterSource& source) {
    if (!isGenericallyValidSource(source)) {
        return false;
    }
    if (const auto* constant = std::get_if<bloom::document::ConstantValueSource>(&source)) {
        return constantMatchesSchema(schemaKey, *constant);
    }

    // Composition validation owns typed curve resolution because a ParameterStore cannot inspect
    // its composition's curve store. Driver evaluation remains deferred.
    return true;
}

} // namespace

namespace bloom::document {

const ParameterRecord* ParameterStore::find(const ParameterId id) const noexcept {
    const auto iterator = std::find_if(records_.begin(), records_.end(),
                                       [id](const auto& record) { return record.id == id; });
    return iterator == records_.end() ? nullptr : &*iterator;
}

bool ParameterStore::insert(ParameterRecord record) {
    if (!record.id.isValid() || !isValidStructuralText(record.schemaKey) ||
        !isValidSourceForSchema(record.schemaKey, record.source) || find(record.id) != nullptr) {
        return false;
    }

    records_.push_back(std::move(record));
    return true;
}

bool ParameterStore::erase(const ParameterId id) {
    const auto iterator = std::find_if(records_.begin(), records_.end(),
                                       [id](const auto& record) { return record.id == id; });
    if (iterator == records_.end()) {
        return false;
    }

    records_.erase(iterator);
    return true;
}

bool ParameterStore::setSource(const ParameterId id, ParameterSource source) {
    const auto record = std::find_if(records_.begin(), records_.end(),
                                     [id](const auto& item) { return item.id == id; });
    if (record == records_.end() || !isValidSourceForSchema(record->schemaKey, source)) {
        return false;
    }

    record->source = std::move(source);
    return true;
}

ValidationResult ParameterStore::validate() const {
    ValidationResult result;
    std::unordered_set<ParameterId> ids;

    for (const auto& record : records_) {
        const auto path = "parameters[" + std::to_string(record.id.value()) + "]";
        if (!record.id.isValid()) {
            result.add(ValidationCode::InvalidId, path, "Parameter ID must not be zero");
        } else if (!ids.insert(record.id).second) {
            result.add(ValidationCode::DuplicateId, path, "Parameter ID is duplicated");
        }
        validateStructuralText(record.schemaKey, path + ".schemaKey", "Parameter schema key",
                               result);
        if (!isValidSourceForSchema(record.schemaKey, record.source)) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Parameter source does not satisfy its schema");
        }
    }

    return result;
}

} // namespace bloom::document
