#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QRectF>

#include <cstdint>

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

// The outline a filled canvas glyph (a keyframe) strokes with: its fill taken two steps down the
// same pressed relation, so the edge reads on the fill without a hand-picked darker shade.
[[nodiscard]] QColor edgeInkFor(const QColor& fill);

// The surface a control rests on in a given state, walking the token surface ladder: hover is one
// step up, pressed is one step down, disabled does not move at all (a disabled control has no
// hover response).
[[nodiscard]] Color surfaceForState(Color resting, State state);

// The border a control shows in a given state.
[[nodiscard]] Color borderForState(State state);

// The kit-wide focus/hover border rule (task F1, item F2). A kit control shows exactly ONE 1px
// border and never a second outline: `Border` at rest, `BorderHover` under the pointer, and
// `Accent` while it is active -- focused, being edited, or holding an open popup.
//
// FOCUS WINS. A control that is both active and hovered stays `Accent`, so moving the pointer over
// the thing you are editing never takes the focus indication away; hover is the weaker signal and
// yields. A disabled control rests at `Border` and has no hover response at all, matching the
// disabled rule the rest of this header already applies.
//
// A control that is borderless at rest -- a KValueField cell -- paints this function's `Border`
// result transparent instead. The token it gets back is the same; only that one control's resting
// ink differs, because its cell already reads as a field without an outline.
//
// This replaces the outer focus ring for every control it applies to: drawFocusRing() below is no
// longer part of the focus recipe for KButton, KDropdown, KPanelSwitcher, KSlider, KSwitch, or
// KValueField.
[[nodiscard]] Color borderForInteraction(bool enabled, bool active, bool hovered);

// The ink a control's text takes in a given state, already faded when disabled.
[[nodiscard]] QColor inkForState(Color resting, State state);

// The kit's ONE button state recipe. KButton's four variants, the recessed KIconButton, and the
// menu-strip KMenuButton (whose rules the generated stylesheet emits from this same function) all
// resolve their fill, border and ink here, so "checked", "danger", "hover" and "focused" read the
// same on every kind of button.
//
//   Primary       Accent fill; the affirmative action.
//   Secondary     A raised plate with a hairline; the ordinary action.
//   Ghost         Chrome weight: no plate at rest, a plate under the pointer.
//   Danger        Outlined in Error at rest, Error fill once the pointer commits.
//   GhostDanger   Ghost at rest, the Danger fill under the pointer (close buttons).
//   Recessed      The ControlSurface well of a header icon button.
//   MenuItem      A menu-strip title: transparent at rest, the Accent hover bar under the pointer.
//
// Selected (a checked toggle) is Accent with OnAccent ink on every kind. Disabled keeps the plate
// still and fades the ink by the one disabled recipe. Keyboard focus turns the single border
// Accent -- or Foreground on an Accent plate, where an Accent border would be invisible -- and
// focus wins over hover.
enum class ButtonKind : std::uint8_t {
    Primary,
    Secondary,
    Ghost,
    Danger,
    GhostDanger,
    Recessed,
    MenuItem,
};

struct ButtonColors {
    // An invalid colour means "paint nothing" for that layer.
    QColor fill;
    QColor border;
    QColor ink;
};

[[nodiscard]] ButtonColors buttonColors(ButtonKind kind, State state, bool keyboardFocus);

// A pen exactly one physical pixel wide at the painter's device pixel ratio -- no blur at 125% or
// 150% -- with the painter offset conventions the rounded-rect helpers below expect.
void applyHairlinePen(QPainter& painter, const QColor& color);

// Fills a rounded rectangle and strokes its hairline border inside `bounds`. An invalid border
// color skips the stroke. The rectangle is first snapped to whole device pixels under the
// painter's own transform (device pixel ratio included), so the hairline lands on one physical
// pixel row at 1x, 1.25x, 1.5x and 2x instead of straddling two.
void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, Radius radius);

// The same with an explicit corner radius in design pixels -- for a shape nested inside another
// rounded shape, whose corner must be the outer radius minus the inset to stay concentric.
void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, qreal cornerPx);

// `bounds` with its edges rounded to whole device pixels under `painter`'s transform. Only exact
// for an unrotated transform; a rotated or sheared one returns `bounds` unchanged.
[[nodiscard]] QRectF snappedToDevicePixels(const QPainter& painter, const QRectF& bounds);

// The box a control paints inside its own widget rectangle: Spacing::ControlInset in on every
// side. Every button, dropdown and value field draws its plate here, so they share one height.
[[nodiscard]] QRectF controlBounds(const QWidget& widget);

// A selected list row (timeline layer and lane rows, KRow, Assets and Jobs rows): the opaque
// selectionWash() over the row's own surface, with a Size::SelectionEdge Accent edge at its
// leading side when `leadingEdge` is set. The edge marks the row even where the wash is covered
// by content; the wash keeps that content -- clip bars in their own colours -- readable.
void paintSelectedRow(QPainter& painter, const QRectF& row, Color surface, bool leadingEdge);

// The hairline that closes a list row, along `row`'s bottom edge inside it, snapped to one
// physical pixel. The ONE row separator: KListSurface, KRow and the timeline grid all draw it.
void paintRowSeparator(QPainter& painter, const QRectF& row);

// Draws an accent ring OUTSIDE `bounds`, in the kFocusRingWidth of margin widgets reserve for it in
// their size hints, so drawing it never changes a control's size or moves anything next to it.
//
// No longer the kit's focus affordance (task F1, item F2: borderForInteraction() above is). It
// survives for the color widgets, whose focusable target is a color field or a swatch -- surfaces
// whose own border color is the artist's data rather than a state channel, so a border-color change
// there could not carry focus at all.
void drawFocusRing(QPainter& painter, const QRectF& bounds, Radius radius);

// Attaches (or clears, for Elevation::Flat) the token drop shadow for an elevated surface.
void applyElevation(QWidget& widget, Elevation elevation);

// Paints an elevation token's soft shadow for a rounded surface whose shadow-casting shape is
// `caster`, offset by the token's own offset, from a cached nine-patch (one blur per token,
// radius and device pixel ratio -- never a per-paint blur). The caller reserves transparent room
// around the surface for it and paints the surface itself on top. Flat paints nothing.
void drawElevationShadow(QPainter& painter, const QRectF& caster, Radius radius,
                         Elevation elevation);

// The two-tone grid every translucent-color surface paints behind itself -- a color chip, the
// picker's alpha bar, a swatch, and the Viewer's Checkerboard background -- so partial alpha reads
// against a neutral ground. The tones are the CheckerLight/CheckerDark mid-greys, never interface
// surfaces: a dark surface pair vanishes under any dark image. `cellPx` is one square's edge in
// device-independent pixels; `radius` clips the pattern to `bounds`' rounded corners so it never
// bleeds past a chip's own outline. The square overload clips to `bounds` alone.
void drawAlphaCheckerboard(QPainter& painter, const QRectF& bounds, int cellPx, Radius radius);
void drawAlphaCheckerboard(QPainter& painter, const QRectF& bounds, int cellPx);

} // namespace bloom::ui::kit
