#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

[[nodiscard]] bool
isGenericallyValidSource(const bloom::document::ParameterSource& source) noexcept {
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
    if (schemaKey == kPositionParameterSchemaKey) {
        const auto* position = std::get_if<Vec2d>(&constant.value);
        return position != nullptr && std::isfinite(position->x) && std::isfinite(position->y);
    }
    if (schemaKey == kOpacityParameterSchemaKey) {
        const auto* opacity = std::get_if<double>(&constant.value);
        return opacity != nullptr && std::isfinite(*opacity) && *opacity >= 0.0 && *opacity <= 1.0;
    }
    if (schemaKey == kTextParameterSchemaKey) {
        return std::holds_alternative<std::string>(constant.value);
    }
    return true;
}

[[nodiscard]] bool isValidSourceForSchema(const std::string_view schemaKey,
                                          const bloom::document::ParameterSource& source) noexcept {
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
    if (!record.id.isValid() || record.schemaKey.empty() ||
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
        if (record.schemaKey.empty()) {
            result.add(ValidationCode::EmptyKey, path + ".schemaKey",
                       "Parameter schema key must not be empty");
        }
        if (!isValidSourceForSchema(record.schemaKey, record.source)) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Parameter source does not satisfy its schema");
        }
    }

    return result;
}

} // namespace bloom::document
