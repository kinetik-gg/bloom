#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <optional>
#include <span>
#include <vector>

namespace bloom::document {

struct LayerStackEntry {
    LayerSlotId slotId;
    LayerId layerId;

    friend bool operator==(const LayerStackEntry&, const LayerStackEntry&) = default;
};

class LayerStack final {
  public:
    explicit LayerStack(NodeId nodeId) noexcept : nodeId_(nodeId) {}

    [[nodiscard]] NodeId nodeId() const noexcept { return nodeId_; }
    [[nodiscard]] std::span<const LayerStackEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] const LayerStackEntry* find(LayerSlotId slotId) const noexcept;

    [[nodiscard]] bool append(LayerStackEntry entry);
    [[nodiscard]] bool erase(LayerSlotId slotId);
    [[nodiscard]] bool moveBefore(LayerSlotId slotId, std::optional<LayerSlotId> beforeSlotId);

    [[nodiscard]] ValidationResult validate() const;

  private:
    NodeId nodeId_;
    std::vector<LayerStackEntry> entries_;
};

} // namespace bloom::document
