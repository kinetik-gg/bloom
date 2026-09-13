#pragma once

#include <QString>

namespace bloom::core {
struct Color4d;
} // namespace bloom::core

namespace bloom::document {
struct ParameterRecord;
} // namespace bloom::document

namespace bloom::ui {

class CompositionSession;

// --- Shared authoring/presentation truth (task U4, issue #123) ---------------------------------
//
// Three decisions the Properties panel and the Timeline's Add menu already owned, now CALLED by the
// Nodes canvas rather than copied into it. Task U4's decision 4 requires the node editor's Add menu
// to offer "exactly the layer kinds the command layer offers today (mirror the timeline's Add menu
// contents)", and its decision 5 requires in-node fields to read and commit through "the EXACT same
// session/command paths Properties uses". Copying the default-name rule, the built-in proof
// palette, or the source wording into node_editor.cpp would have duplicated four raw color literals
// and three user-visible strings, and would let the two surfaces silently drift. These declarations
// exist so there is exactly one definition of each; every one of them is defined in
// composition_authoring.cpp, beside the anonymous-namespace helpers it already used.

// "Constant" / "Animated" / "Driven by graph" -- the one wording for a parameter's source, shown by
// the Properties rows' tooltips and by the node card's own field tooltips.
[[nodiscard]] QString parameterSourceDescription(const document::ParameterRecord& parameter);

// The exact, never-rounded and never-clipped "R r  G g  B b  A a" rendering of an authoring color
// (negative and HDR channels included -- docs/architecture/evaluation-primitives.md's straight
// Color4d authoring values). The Properties Solid Source row shows it as its value text; the node
// card's read-only color chip carries it in its tooltip, because a quantized 8-bit swatch cannot
// show an out-of-range channel honestly on its own.
[[nodiscard]] QString exactColorText(core::Color4d color);

// One "Add Solid"/"Add Text" gesture, two entry points (the Timeline's Add menu, the Nodes canvas
// context menu): same default numbered name derived from the composition's own existing layers,
// same next built-in reference-linear-sRGB proof color, same single command transaction. Returns
// what CompositionSession::addSolidLayer()/addTextLayer() returned.
[[nodiscard]] bool addDefaultSolidLayer(CompositionSession& session);
[[nodiscard]] bool addDefaultTextLayer(CompositionSession& session);

} // namespace bloom::ui
