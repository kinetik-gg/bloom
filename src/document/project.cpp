#include <bloom/document/project.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace bloom::document {

bool Composition::setDuration(const core::RationalTime duration) noexcept {
    if (duration <= core::RationalTime{}) {
        return false;
    }
    duration_ = duration;
    return true;
}

ValidationResult Composition::validate() const {
    ValidationResult result;
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Composition ID must not be zero");
    }
    if (duration_ <= core::RationalTime{}) {
        result.add(ValidationCode::InvalidValue, "duration",
                   "Composition duration must be greater than zero");
    }

    result.append("parameters", parameters_.validate());
    result.append("graph", graph_.validate(parameters_));
    return result;
}

const Composition* Project::findComposition(const CompositionId id) const noexcept {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    return iterator == compositions_.end() ? nullptr : &*iterator;
}

Composition* Project::findComposition(const CompositionId id) noexcept {
    return const_cast<Composition*>(std::as_const(*this).findComposition(id));
}

bool Project::addComposition(Composition composition) {
    if (!composition.id().isValid() || findComposition(composition.id()) != nullptr) {
        return false;
    }
    compositions_.push_back(std::move(composition));
    return true;
}

bool Project::removeComposition(const CompositionId id) {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    if (iterator == compositions_.end()) {
        return false;
    }
    compositions_.erase(iterator);
    return true;
}

ValidationResult Project::validate() const {
    ValidationResult result;
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Project ID must not be zero");
    }

    std::unordered_set<CompositionId> ids;
    for (const auto& composition : compositions_) {
        const auto path = "compositions[" + std::to_string(composition.id().value()) + "]";
        if (!composition.id().isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Composition ID must not be zero");
        } else if (!ids.insert(composition.id()).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Composition ID is duplicated");
        }
        result.append(path, composition.validate());
    }
    return result;
}

} // namespace bloom::document
