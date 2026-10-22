#pragma once

#include <bloom/document/project.hpp>

namespace bloom::ui::detail {

// Unrelated document edits need not cancel a pending choice. Only a replaced project or a
// conflicting edit to this asset's input interpretation invalidates what the artist saw.
[[nodiscard]] inline bool
assetInputPickerTargetMatches(const document::ProjectId projectId, const document::AssetId assetId,
                              const document::AssetInterpretation& initial,
                              const document::Project& current) {
    if (current.id() != projectId)
        return false;
    const auto* asset = current.findAsset(assetId);
    return asset && asset->interpretation == initial;
}

} // namespace bloom::ui::detail
