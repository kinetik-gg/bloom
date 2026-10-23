#pragma once

#include <QVBoxLayout>
#include <QWidget>

namespace bloom::ui {
class CompositionSession;

namespace properties {

// ADR 0024 layer-owned time: the Properties "Time" section of the contextual layer. Start moves
// the layer (its content and keys with it); In, Out and Frames trim its range. Every edit is one
// SetLayerTiming transaction with exact undo. Before/After pick the out-of-extent modes (one
// SetLayerTimeModes each) and Time Remap binds or unbinds the layer's remap curve. The section refreshes itself from the session's
// snapshot and selection, and hides itself when no layer is in context.
void addLayerTimeSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent);

} // namespace properties
} // namespace bloom::ui
