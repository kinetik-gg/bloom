#pragma once

#include <bloom/ui/timeline_editor.hpp>

#include <set>
#include <utility>
#include <vector>

namespace bloom::ui {

// ADR 0028: the "Masks" group under an expanded layer row. Nothing is appended for a layer
// without masks. The group holds one row per mask (label = name and mode, keying its path) and,
// one level deeper, a Feather, Expansion and Opacity row per mask.
inline constexpr std::string_view kMaskPathRowRole = "mask-path";
inline constexpr std::string_view kMaskFeatherRowRole = "mask-feather";
inline constexpr std::string_view kMaskExpansionRowRole = "mask-expansion";
inline constexpr std::string_view kMaskOpacityRowRole = "mask-opacity";

// With `reveal`, only the listed mask parameters show (the group opens; no row, no group).
void appendMaskEntries(const CompositionSession& session, const TimelineLayerEntry& layer,
                       const std::set<std::pair<document::LayerId, QString>>& collapsedGroups,
                       std::vector<TimelineLayerEntry>& rows,
                       const std::set<document::ParameterId>* reveal = nullptr);

// The collapse key of the Masks group.
[[nodiscard]] QString maskGroupKey();

} // namespace bloom::ui
