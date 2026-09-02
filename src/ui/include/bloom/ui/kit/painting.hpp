#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QRectF>

class QPainter;
class QWidget;

namespace bloom::ui::kit {

// The state color recipes and the painting primitives every kit widget draws with. Kept out of
// each widget so "hover is one surface step up plus BorderHover" is written once and can be
// asserted once, rather than re-derived in seven paint methods.

// How much a filled control lightens on hover, and how much it darkens on press.
//
// Both are derived from the Accent triple rather than invented: the tokens already state what a
// filled Bloom control does between rest, hover, and press (Accent -> AccentHover ->
// AccentPressed), so every other filled variant -- Danger above all -- reproduces exactly that
// relation instead of needing its own pair of hand-picked colors. Hover blends toward Foreground;
// press scales the channels down. kit_painting_tests.cpp asserts both round-trip the Accent triple.
[[nodiscard]] qreal filledHoverBlend();
[[nodiscard]] qreal filledPressedScale();

[[nodiscard]] QColor hoverFillFor(const QColor& resting);
[[nodiscard]] QColor pressedFillFor(const QColor& resting);

// The surface a control rests on in a given state, walking the token surface ladder: hover is one
// step up, pressed is one step down, disabled does not move at all (a disabled control has no
// hover response).
[[nodiscard]] Color surfaceForState(Color resting, State state);

// The border a control shows in a given state.
[[nodiscard]] Color borderForState(State state);

// The ink a control's text takes in a given state, already faded when disabled.
[[nodiscard]] QColor inkForState(Color resting, State state);

// A pen exactly one physical pixel wide at the painter's device pixel ratio -- no blur at 125% or
// 150% -- with the painter offset conventions the rounded-rect helpers below expect.
void applyHairlinePen(QPainter& painter, const QColor& color);

// Fills a rounded rectangle and strokes its hairline border inside `bounds`. An invalid border
// color skips the stroke.
void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, Radius radius);

// Draws the focus ring OUTSIDE `bounds`, so gaining focus never changes a control's size or moves
// anything next to it. Widgets reserve kFocusRingWidth of margin for it in their size hints.
void drawFocusRing(QPainter& painter, const QRectF& bounds, Radius radius);

// Attaches (or clears, for Elevation::Flat) the token drop shadow for an elevated surface.
void applyElevation(QWidget& widget, Elevation elevation);

// Task U6 (issue #121): the two-tone grid every translucent-color surface paints behind itself --
// a color chip, the alpha bar in KColorPicker's Square HSV form -- so partial alpha reads against
// a neutral ground instead of silently blending into whatever the widget happens to sit on. Added
// here rather than duplicated in each color widget because it is exactly what this header already
// promises ("the painting primitives every kit widget draws with"), and it composes only from the
// existing surface-ladder tokens: the two checker tones are Color::Surface and its neighbor one
// step up the ladder, never a literal gray of their own. `cellPx` is the edge length of one square
// in the checker in device-independent pixels; `radius` clips the pattern to `bounds`' rounded
// corners so it never bleeds past a chip's own outline.
void drawAlphaCheckerboard(QPainter& painter, const QRectF& bounds, int cellPx, Radius radius);

} // namespace bloom::ui::kit
