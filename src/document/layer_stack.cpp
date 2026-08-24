#include <bloom/document/layer_stack.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>

namespace bloom::document {

const LayerStackEntry* LayerStack::find(const LayerSlotId slotId) const noexcept {
    const auto iterator =
        std::find_if(entries_.begin(), entries_.end(),
                     [slotId](const auto& entry) { return entry.slotId == slotId; });
    return iterator == entries_.end() ? nullptr : &*iterator;
}

bool LayerStack::append(const LayerStackEntry entry) {
    if (!entry.slotId.isValid() || !entry.layerId.isValid() || find(entry.slotId) != nullptr) {
        return false;
    }

    const auto duplicateLayer =
        std::find_if(entries_.begin(), entries_.end(),
                     [entry](const auto& existing) { return existing.layerId == entry.layerId; });
    if (duplicateLayer != entries_.end()) {
        return false;
    }

    entries_.push_back(entry);
    return true;
}

bool LayerStack::erase(const LayerSlotId slotId) {
    const auto iterator =
        std::find_if(entries_.begin(), entries_.end(),
                     [slotId](const auto& entry) { return entry.slotId == slotId; });
    if (iterator == entries_.end()) {
        return false;
    }

    entries_.erase(iterator);
    return true;
}

bool LayerStack::moveBefore(const LayerSlotId slotId,
                            const std::optional<LayerSlotId> beforeSlotId) {
    const auto moving = std::find_if(entries_.begin(), entries_.end(), [slotId](const auto& entry) {
        return entry.slotId == slotId;
    });
    if (moving == entries_.end()) {
        return false;
    }
    if (beforeSlotId == slotId) {
        return true;
    }

    std::size_t destination = entries_.size();
    if (beforeSlotId.has_value()) {
        const auto before =
            std::find_if(entries_.begin(), entries_.end(), [beforeSlotId](const auto& entry) {
                return entry.slotId == *beforeSlotId;
            });
        if (before == entries_.end()) {
            return false;
        }
        destination = static_cast<std::size_t>(std::distance(entries_.begin(), before));
    }

    const auto movingIndex = static_cast<std::size_t>(std::distance(entries_.begin(), moving));
    const auto entry = *moving;
    entries_.erase(moving);
    if (destination > movingIndex) {
        --destination;
    }
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(destination), entry);
    return true;
}

ValidationResult LayerStack::validate() const {
    ValidationResult result;
    if (!nodeId_.isValid()) {
        result.add(ValidationCode::InvalidId, "nodeId", "Layer Stack node ID must not be zero");
    }

    std::unordered_set<LayerSlotId> slotIds;
    std::unordered_set<LayerId> layerIds;
    for (const auto& entry : entries_) {
        const auto path = "entries[" + std::to_string(entry.slotId.value()) + "]";
        if (!entry.slotId.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".slotId",
                       "Layer Stack slot ID must not be zero");
        } else if (!slotIds.insert(entry.slotId).second) {
            result.add(ValidationCode::DuplicateId, path + ".slotId",
                       "Layer Stack slot ID is duplicated");
        }
        if (!entry.layerId.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".layerId", "Layer ID must not be zero");
        } else if (!layerIds.insert(entry.layerId).second) {
            result.add(ValidationCode::DuplicateId, path + ".layerId",
                       "Layer may participate only once in a Layer Stack");
        }
    }

    return result;
}

} // namespace bloom::document
