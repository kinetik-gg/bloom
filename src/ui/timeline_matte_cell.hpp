#pragma once

#include <bloom/document/ids.hpp>

namespace bloom::ui {
class CompositionSession;
namespace kit {
class KDropdown;
}

// ADR 0028: the Timeline's Track Matte column. One compact dropdown per layer row listing
// None, the other layers of the same Merge, then Alpha / Luma / Inverted for the current matte.
// Items carry a signed code: >= 0 is a source layer id (0 is None); the negatives are the
// channel and invert actions, which edit the existing matte and are disabled without one.
namespace matte_cell {

inline constexpr long long kActionAlpha = -1;
inline constexpr long long kActionLuma = -2;
inline constexpr long long kActionInverted = -3;

// Rebuilds the items from the document and selects the current source. Never authors anything.
void populate(kit::KDropdown& dropdown, const CompositionSession& session, document::LayerId layer);

// Applies the picked item as one SetTrackMatte. Returns whether the document changed.
bool activate(CompositionSession& session, document::LayerId layer, long long code);

} // namespace matte_cell
} // namespace bloom::ui
