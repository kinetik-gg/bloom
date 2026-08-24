#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

[[nodiscard]] bool isValidSource(const bloom::document::ParameterSource& source) noexcept {
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
                return true;
            } else if constexpr (std::is_same_v<Source, bloom::document::AnimationCurveSource>) {
                return valueSource.curveId.isValid();
            } else {
                return valueSource.driverId.isValid();
            }
        },
        source);
}

} // namespace

namespace bloom::document {

const ParameterRecord* ParameterStore::find(const ParameterId id) const noexcept {
    const auto iterator = std::find_if(records_.begin(), records_.end(),
                                       [id](const auto& record) { return record.id == id; });
    return iterator == records_.end() ? nullptr : &*iterator;
}

ParameterRecord* ParameterStore::find(const ParameterId id) noexcept {
    return const_cast<ParameterRecord*>(std::as_const(*this).find(id));
}

bool ParameterStore::insert(ParameterRecord record) {
    if (!record.id.isValid() || record.schemaKey.empty() || !isValidSource(record.source) ||
        find(record.id) != nullptr) {
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
    auto* record = find(id);
    if (record == nullptr || !isValidSource(source)) {
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
        if (!isValidSource(record.source)) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Parameter source is invalid");
        }
    }

    return result;
}

} // namespace bloom::document
