#pragma once

// The layer table's labels (After Effects' Label column): the label menu a label square and the
// row context menu open, Select Label Group, and the header's label filter.
//
// A label is the layer's stored label colour, or none (the kind default the clip bar falls back
// to). Two layers are in the same label group when they store the same label. The filter groups
// labels by their menu entry: Default, Label 1 to Label 8, and Custom for any other colour.

#include <bloom/document/ids.hpp>

#include <QColor>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

class QMenu;

namespace bloom::ui {
class CompositionSession;

namespace layer_labels {

using Label = std::optional<std::array<std::uint8_t, 3>>;

inline constexpr int kKindDefault = 0;
inline constexpr int kPresetCount = 8;
inline constexpr int kCustom = kPresetCount + 1;

[[nodiscard]] Label labelOf(const CompositionSession& session, document::LayerId layer);
// The filter entry a label falls under: kKindDefault, 1..kPresetCount, or kCustom.
[[nodiscard]] int filterKey(const Label& label);
[[nodiscard]] QColor presetColor(int preset);
[[nodiscard]] QString filterKeyName(int key);

// Every layer of the composition storing `layer`'s label, selected; `layer` stays primary.
void selectLabelGroup(CompositionSession& session, document::LayerId layer);

// The label menu for `layers` (one undoable step for all of them), with `anchor`'s label checked:
// Default, Label 1 to Label 8, Custom… (which calls `custom`), then Select Label Group for
// `anchor`'s label.
void populateLabelMenu(QMenu& menu, CompositionSession& session,
                       const std::vector<document::LayerId>& layers, document::LayerId anchor,
                       std::function<void()> custom);

// The header's label filter menu: All Labels, then one checkable entry per filter key. Choosing an
// entry hands `apply` the new set (empty: every label shows).
void populateLabelFilterMenu(QMenu& menu, const std::set<int>& active,
                             std::function<void(std::set<int>)> apply);

} // namespace layer_labels
} // namespace bloom::ui
