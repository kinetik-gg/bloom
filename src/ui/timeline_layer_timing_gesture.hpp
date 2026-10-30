#pragma once

// The Timeline bar gesture math lives with the commands that also use it (a layer drop moves each
// new layer to its start the way a body drag does); the Timeline keeps its own names for it.
#include <bloom/commands/layer_timing_gesture.hpp>

namespace bloom::ui {
using commands::LayerTimingGestureInput;
using commands::LayerTimingProposal;
using commands::proposeLayerTiming;
} // namespace bloom::ui
