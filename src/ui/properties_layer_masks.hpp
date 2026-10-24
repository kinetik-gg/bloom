#pragma once

#include <QVBoxLayout>
#include <QWidget>

namespace bloom::ui {
class CompositionSession;

namespace properties {

// ADR 0028 Properties sections of the contextual layer.
//
// Masks: one group per mask -- name, closed-enum Mode, Inverted, the path (with its keyframe
// diamond) and Feather / Expansion / Opacity rows editing the mask's own parameters through the
// ordinary parameter edit and keyframe paths -- plus move up / down / remove. Every edit is one
// typed command with exact undo. Clicking a group selects the mask for the Viewer.
void addLayerMasksSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent);

// Track Matte: the source layer (None or another layer of the same Merge), the channel and the
// Inverted bit, mirroring the Timeline matte cell.
void addTrackMatteSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent);

} // namespace properties
} // namespace bloom::ui
