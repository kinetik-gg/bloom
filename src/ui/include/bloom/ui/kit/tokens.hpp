#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QString>

#include <cstdint>

// Kinetik design tokens -- the single C++ truth for every Bloom interface color, radius, border,
// spacing, size, elevation, type role, state recipe, and motion duration. Product code (widgets,
// painters, and the generated application stylesheet) reads these; no Bloom surface may spell a
// raw hex literal, a raw pixel gap, or a raw duration of its own.
//
// Namespace choice: the whole kit -- tokens, icons, fonts, theme installer, and widgets -- lives in
// the single namespace `bloom::ui::kit`, which is exactly the CMake target name (`bloom_ui_kit`),
// the source directory (`src/ui/kit`), and the public include root (`bloom/ui/kit`). One name for
// one boundary; a separate `bloom::ui::tokens` would have split the token vocabulary away from the
// widgets that are the only reason it exists.
//
// Unit rule: every numeric token below is expressed in 1x design pixels, and 1 design pixel is
// exactly 1 Qt logical pixel. Physical pixels are derived from the device pixel ratio only inside
// painting code (see snappedHairlineWidth()) -- never by scaling a token at its definition.
namespace bloom::ui::kit {

// Semantic color roles. Data* roles are the data-type palette used to identify item kinds;
// Brand is the Bloom logo color and is never used for interface chrome.
enum class Color : std::uint8_t {
    Background,
    Surface,
    SurfaceRaised,
    Field,
    Foreground,
    Muted,
    Faint,
    Border,
    BorderHover,
    Accent,
    AccentHover,
    AccentPressed,
    Keyframe,
    Ok,
    Warn,
    Error,
    Brand,
    DataSequence,
    DataClip,
    DataComposition,
    DataImage,
    DataAudio,
};

// The four-step surface ladder, darkest first. "hover = surface + 1 step" and "pressed =
// surface - 1 step" walk exactly this ladder; a raised surface (popup, dialog, drag) also steps up
// one level and keeps its hairline.
inline constexpr int kSurfaceLevelCount = 4;

[[nodiscard]] QColor color(Color token);

// Lowercase "#rrggbb" -- the one function the generated stylesheet uses, so a QSS rule and a
// painter can never disagree about a role's value.
[[nodiscard]] QString hex(Color token);

// Steps `token` along the surface ladder (Background/Surface/SurfaceRaised/Field), clamped at both
// ends. A non-surface role is returned unchanged.
[[nodiscard]] Color surfaceStep(Color token, int steps);

// Corner radii. Full is a pill: resolved against the control's own extent at use, because a pill
// radius is only meaningful relative to the shape it rounds.
enum class Radius : int {
    Small = 3,
    Medium = 6,
    Large = 12,
    XLarge = 16,
    Full = -1,
};

[[nodiscard]] int radiusPx(Radius token, int extentPx);

// Layout spacing. Gutter is the visible Background gap between panels.
enum class Spacing : int {
    XXS = 2,
    XS = 4,
    S = 8,
    M = 12,
    L = 16,
    XL = 24,
    XXL = 32,
    Gutter = 6,
};

// Control and chrome extents.
enum class Size : int {
    ControlCompact = 22,
    Control = 26,
    ControlRoomy = 32,
    IconSmall = 12,
    IconMedium = 16,
    IconLarge = 20,
    TitleBar = 34,
    PanelHeader = 30,
    TimelineRow = 34,
    ScrollBar = 8,
    ScrollBarHover = 12,
};

[[nodiscard]] constexpr int px(const Spacing token) noexcept { return static_cast<int>(token); }
[[nodiscard]] constexpr int px(const Size token) noexcept { return static_cast<int>(token); }

// Border widths in design pixels. The focus ring is drawn OUTSIDE the control's own rectangle so
// gaining focus never shifts layout.
inline constexpr qreal kHairlineWidth = 1.0;
inline constexpr qreal kFocusRingWidth = 1.5;
inline constexpr qreal kWindowBorderWidth = 1.0;

// A hairline that lands on whole physical pixels at any device pixel ratio: at 1.25x or 1.5x a
// plain 1.0 logical-pixel pen straddles two physical pixels and reads as a blurred grey line.
// Returns the logical width whose physical width is a whole number of pixels.
[[nodiscard]] qreal snappedHairlineWidth(qreal devicePixelRatio);

// Elevation shadows. Flat casts nothing.
enum class Elevation : std::uint8_t {
    Flat,
    Popup,
    Dialog,
    Drag,
};

struct Shadow {
    int offsetX = 0;
    int offsetY = 0;
    int blurRadius = 0;
    QColor color;

    [[nodiscard]] bool isFlat() const noexcept { return blurRadius == 0 && !color.isValid(); }
};

[[nodiscard]] Shadow shadow(Elevation token);

// Type roles. Sizes are in design pixels; Value is the monospaced role and every numeric, unit,
// hex, and timecode surface uses it.
enum class TypeRole : std::uint8_t {
    Ui,
    UiSmall,
    Value,
    Title,
};

// The bundled family names. font() lists them ahead of Qt's own style-hint fallback, so a missing
// or unregistered bundled face degrades to the platform sans-serif/monospace family instead of
// producing a wrong or empty face.
[[nodiscard]] QString interfaceFontFamily();
[[nodiscard]] QString monospaceFontFamily();

[[nodiscard]] QFont font(TypeRole role);

// Interaction states shared by every kit widget's state machine.
enum class State : std::uint8_t {
    Normal,
    Hover,
    Pressed,
    Selected,
    Disabled,
    Focused,
};

// Disabled ink is the normal ink at this opacity; a disabled control also stops responding to
// hover entirely.
inline constexpr qreal kDisabledOpacity = 0.40;

[[nodiscard]] QColor withOpacity(const QColor& value, qreal opacity);

// Motion. Fast is hover and toggle feedback; Pop is the menu/popup entrance with a 4 design-pixel
// rise; None is mandatory for playhead, scrub, and viewer transforms -- direct editor feedback is
// never eased, because an eased playhead lies about where time is.
enum class Motion : std::uint8_t {
    Fast,
    Pop,
    None,
};

inline constexpr int kPopRisePx = 4;

[[nodiscard]] int durationMs(Motion token);
[[nodiscard]] QEasingCurve easing(Motion token);

// Reduced-motion kill switch. Returns false when the platform asks for reduced motion or when
// BLOOM_REDUCED_MOTION is set in the environment; every kit animation must consult this and jump
// straight to its end state instead of animating.
[[nodiscard]] bool motionEnabled();

} // namespace bloom::ui::kit
