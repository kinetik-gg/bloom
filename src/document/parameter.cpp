#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>

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
                if (const auto* path = std::get_if<bloom::document::PathValue>(&valueSource.value))
                    return path->isValid();
                if (const auto* value = std::get_if<double>(&valueSource.value)) {
                    return std::isfinite(*value);
                }
                if (const auto* value = std::get_if<bloom::document::Vec2d>(&valueSource.value)) {
                    return std::isfinite(value->x) && std::isfinite(value->y);
                }
                if (const auto* value = std::get_if<bloom::document::Vec3d>(&valueSource.value)) {
                    return std::isfinite(value->x) && std::isfinite(value->y) &&
                           std::isfinite(value->z);
                }
                if (const auto* value = std::get_if<bloom::core::Color4d>(&valueSource.value)) {
                    return value->isValid();
                }
                if (const auto* value = std::get_if<std::string>(&valueSource.value)) {
                    return bloom::core::isValidUtf8(*value);
                }
                return true;
            } else if constexpr (std::is_same_v<Source, bloom::document::AnimationCurveSource>) {
                if (!valueSource.curveId.isValid()) {
                    return false;
                }
                if (!valueSource.defaultValue.has_value()) {
                    return true;
                }
                const bloom::document::ConstantValueSource fallback{*valueSource.defaultValue};
                return isGenericallyValidSource(bloom::document::ParameterSource{fallback});
            } else {
                // A driver names a node and one of its output ports. Whether that node EXISTS and
                // whether its kind fits belongs to CanonicalGraph::validate(), which can see the
                // graph; a ParameterStore can only check that the reference is well formed.
                return valueSource.sourceNodeId.isValid() &&
                       bloom::document::isValidStructuralText(valueSource.outputPort);
            }
        },
        source);
}

// The value-graph schemas (task S7). Split out of constantMatchesSchema() rather than appended to
// it because the two halves answer different questions: the layer schemas above each bound a value
// to the picture it draws (an opacity is in [0, 1], a text size fits the rasterizer), while these
// bound a value to the TYPE its socket carries. Every numeric one here is
// finite-and-otherwise-free, because a value graph's whole purpose is arithmetic the artist
// controls; the only real domains are the closed selector mappings and a non-negative tolerance.
[[nodiscard]] bool
valueGraphConstantMatchesSchema(const std::string_view schemaKey,
                                const bloom::document::ConstantValueSource& constant) noexcept {
    using namespace bloom::document;
    const auto finiteScalar = [&] {
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value);
    };
    const auto integer = [&] { return std::holds_alternative<std::int64_t>(constant.value); };
    if (schemaKey == kScalarValueParameterSchemaKey ||
        schemaKey == kScalarOperandParameterSchemaKey) {
        return finiteScalar();
    }
    if (schemaKey == kIntegerValueParameterSchemaKey ||
        schemaKey == kIntegerOperandParameterSchemaKey) {
        return integer();
    }
    if (schemaKey == kDataBlockParameterSchemaKey) {
        return integer();
    }
    if (schemaKey == kBooleanValueParameterSchemaKey ||
        schemaKey == kBooleanOperandParameterSchemaKey ||
        schemaKey == kClampResultParameterSchemaKey) {
        return std::holds_alternative<bool>(constant.value);
    }
    if (schemaKey == kVector2ValueParameterSchemaKey ||
        schemaKey == kVector2OperandParameterSchemaKey) {
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y);
    }
    if (schemaKey == kVector3ValueParameterSchemaKey ||
        schemaKey == kVector3OperandParameterSchemaKey) {
        const auto* value = std::get_if<Vec3d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y) &&
               std::isfinite(value->z);
    }
    if (schemaKey == kColorValueParameterSchemaKey ||
        schemaKey == kColorOperandParameterSchemaKey) {
        const auto* color = std::get_if<bloom::core::Color4d>(&constant.value);
        return color != nullptr && color->isValid();
    }
    if (schemaKey == kStringValueParameterSchemaKey ||
        schemaKey == kStringOperandParameterSchemaKey) {
        return std::holds_alternative<std::string>(constant.value);
    }
    // The selectors. An integer naming no implemented operation is refused rather than folded to
    // the default, exactly as an unknown blend mode is: computing a different operation than the
    // document asked for would be a silent miscomputation.
    if (schemaKey == kScalarOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && scalarOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kVectorOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && vectorOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kVectorReductionParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && vectorReductionFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kRangeInterpolationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && rangeInterpolationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kCompareOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && compareOperationFromStoredValue(*stored).has_value();
    }
    if (schemaKey == kRoundingModeParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kRoundingModes, *stored).has_value();
    }
    if (schemaKey == kIntegerOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr &&
               selectorFromStoredValue(kIntegerOperations, *stored).has_value();
    }
    if (schemaKey == kBooleanOperationParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr &&
               selectorFromStoredValue(kBooleanOperations, *stored).has_value();
    }
    if (schemaKey == kStringCaseParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStringCaseModes, *stored).has_value();
    }
    if (schemaKey == kStringPadSideParameterSchemaKey) {
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && selectorFromStoredValue(kStringPadSides, *stored).has_value();
    }
    if (schemaKey == kNumberRadixParameterSchemaKey) {
        // The radix itself, not an index: anything core::parseInteger() can actually read, so a
        // document may carry base 36 even though the card offers four bases.
        const auto* stored = std::get_if<std::int64_t>(&constant.value);
        return stored != nullptr && isValidStoredRadix(*stored);
    }
    if (schemaKey == kCompareEpsilonParameterSchemaKey) {
        // A negative tolerance is not a tolerance, and an infinite one makes every comparison true.
        const auto* value = std::get_if<double>(&constant.value);
        return value != nullptr && std::isfinite(*value) && *value >= 0.0;
    }
    // An unregistered schema key carries whatever it carries: this function is the gate for the
    // schemas Bloom OWNS, and an extension's key is not one of them.
    return true;
}

[[nodiscard]] bool
constantMatchesSchema(const std::string_view schemaKey,
                      const bloom::document::ConstantValueSource& constant) noexcept {
    using namespace bloom::document;
    if (schemaKey.starts_with("bloom.shape."))
        return shapeConstantMatchesSchema(schemaKey, constant.value);
    if (schemaKey == kSolidWidthParameterSchemaKey || schemaKey == kSolidHeightParameterSchemaKey) {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && isScalarWithinSchemaDomain(schemaKey, *value);
    }
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
    if (schemaKey == kAudioLevelParameterSchemaKey) {
        const auto* level = std::get_if<double>(&constant.value);
        return level != nullptr && std::isfinite(*level) &&
               isScalarWithinSchemaDomain(schemaKey, *level);
    }
    if (schemaKey == "bloom.composition-source.composition" ||
        schemaKey == "bloom.composition-source.loop-mode") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 &&
               (schemaKey == "bloom.composition-source.composition" || *value <= 2);
    }
    if (schemaKey == "bloom.composition-source.time-offset" ||
        schemaKey == "bloom.composition-source.time-scale") {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value);
    }
    if (schemaKey == "bloom.image.asset")
        return std::holds_alternative<std::string>(constant.value);
    if (schemaKey == "bloom.image.premultiply")
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == "bloom.image.start-frame" || schemaKey == "bloom.image.loop-mode" ||
        schemaKey == "bloom.image.color-space") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value &&
               (schemaKey == "bloom.image.start-frame"
                    ? (*value >= -1000000000 && *value <= 1000000000)
                    : (*value >= 0 && *value <= (schemaKey == "bloom.image.loop-mode" ? 2 : 3)));
    }
    if (schemaKey == "bloom.video.asset")
        return std::holds_alternative<std::string>(constant.value);
    if (schemaKey == "bloom.video.start-frame" || schemaKey == "bloom.video.loop-mode" ||
        schemaKey == "bloom.video.color-space") {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value &&
               (schemaKey == "bloom.video.start-frame"
                    ? (*value >= -1000000000 && *value <= 1000000000)
                    : (*value >= 0 && *value <= (schemaKey == "bloom.video.loop-mode" ? 2 : 3)));
    }
    if (schemaKey == kTextAlignmentParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value <= 2;
    }
    if (schemaKey == kTextFontParameterSchemaKey) {
        if (const auto* reference = std::get_if<std::string>(&constant.value))
            return bloom::core::isValidUtf8(*reference) && reference->size() <= 4096;
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value && *value >= 0 && *value < kTextFontChoiceCount;
    }
    if (schemaKey == kTextBoxParameterSchemaKey) {
        const auto* value = std::get_if<Vec2d>(&constant.value);
        return value != nullptr && std::isfinite(value->x) && std::isfinite(value->y) &&
               value->x >= 0.0 && value->y >= 0.0 && value->x <= 1'000'000.0 &&
               value->y <= 1'000'000.0 &&
               ((value->x == 0.0 && value->y == 0.0) || (value->x > 0.0 && value->y > 0.0));
    }
    if (schemaKey == kTextWrapParameterSchemaKey)
        return std::holds_alternative<bool>(constant.value);
    if (schemaKey == kTextVerticalAlignmentParameterSchemaKey ||
        schemaKey == kTextAnchorModeParameterSchemaKey ||
        schemaKey == kTextOverflowParameterSchemaKey) {
        const auto* value = std::get_if<std::int64_t>(&constant.value);
        return value != nullptr && *value >= 0 && *value <= 2;
    }
    if (schemaKey == kTextLineHeightParameterSchemaKey ||
        schemaKey == kTextLetterSpacingParameterSchemaKey) {
        const auto* value = std::get_if<double>(&constant.value);
        return value && std::isfinite(*value) && isScalarWithinSchemaDomain(schemaKey, *value);
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
    return valueGraphConstantMatchesSchema(schemaKey, constant);
}

[[nodiscard]] bool isValidSourceForSchema(const std::string_view schemaKey,
                                          const bloom::document::ParameterSource& source) {
    if (schemaKey == "bloom.shape.path" &&
        !std::holds_alternative<bloom::document::ConstantValueSource>(source))
        return false;
    if (!isGenericallyValidSource(source)) {
        return false;
    }
    if (const auto* constant = std::get_if<bloom::document::ConstantValueSource>(&source)) {
        return constantMatchesSchema(schemaKey, *constant);
    }

    if (const auto* animation = std::get_if<bloom::document::AnimationCurveSource>(&source)) {
        if (!animation->defaultValue.has_value()) {
            return true;
        }
        return constantMatchesSchema(
            schemaKey, bloom::document::ConstantValueSource{*animation->defaultValue});
    }

    // Composition validation owns typed curve resolution because a ParameterStore cannot inspect
    // its composition's curve store. Driver evaluation remains deferred.
    return true;
}

} // namespace

namespace bloom::document {

bool PathValue::isValid() const noexcept {
    const auto finite = [](const Vec2d point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    return anchors.size() <= kMaximumPathAnchors &&
           std::ranges::all_of(anchors, [&](const PathAnchor& anchor) {
               return finite(anchor.point) && (!anchor.inHandle || finite(*anchor.inHandle)) &&
                      (!anchor.outHandle || finite(*anchor.outHandle));
           });
}

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
