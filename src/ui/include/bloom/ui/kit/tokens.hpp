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
    Canvas,
    CompositionFrame,
    SurfaceSunken,
    Surface,
    SurfaceRaised,
    Field,
    // task U8 (issue 131), formal amendment 1, A2: the recessed fill for header-variant icon
    // buttons and every dropdown closed field. Not a surfaceStep() rung -- darker than
    // Background itself by design, so it never joins the surface ladder.
    ControlSurface,
    Foreground,
    Muted,
    Faint,
    Border,
    BorderHover,
    // The active-panel indicator (task U8, issue #131, fix 1): a subtle neutral border a focused
    // EditorArea takes instead of Accent. Panel chrome never reads as accent-colored; this is the
    // one role that marks "this is the active panel."
    BorderActive,
    Accent,
    OnAccent,
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
    Label1,
    Label2,
    Label3,
    Label4,
    Label5,
    Label6,
    Label7,
    Label8,
    DataImage,
    DataAudio,
    // task S1, item 6: the socket palette. A node socket and the link leaving it identify a
    // TRANSPORT kind, which is a different question from what an item in a project is, so they take
    // their own roles rather than borrowing the Data* ones -- where Image had been reading in
    // exactly AccentHover's blue and could not be told apart from a hovered accent surface. Six
    // distinct hues, none of them Accent or AccentHover.
    SocketImage,
    SocketAudio,
    SocketColor,
    SocketScalar,
    SocketInteger,
    SocketVector,
    SocketString,
    // Task S7. Integer reuses the existing SocketInteger token; the two vector widths share
    // SocketVector so they read as one family, and a Boolean needed its own.
    SocketBoolean,
    // GRAPH-1: the graph editor's per-component curve strokes. X/Y/Z/W is the axis vocabulary and
    // R/G/B/A borrows it unchanged, so one colour always means "the first component" whatever the
    // parameter is. A scalar curve takes Keyframe; selection takes Accent; none of these four is
    // either, so a curve can never be mistaken for a selection.
    ComponentX,
    ComponentY,
    ComponentZ,
    ComponentW,
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
    // task U8 (issue 131), formal amendment 1, A3: the panel body's own radius, exactly 4 --
    // deliberately its own named step, not a reuse of Small (3) or any other named radius.
    Panel = 4,
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
    // task U8 (issue 131), formal amendment 1, A4: the panel header's own vertical padding.
    // Deliberately off the base-4 scale -- not rounded to XS/S/M or any nearby step.
    PanelHeader = 10,
    ChromePadding = 3,
    ChromeGap = 4,
    FieldMargin = 1,
    RowPadding = 1,
    PropertyGutter = 8,
    SectionPadding = 8,
    LanePadding = 12,
    // task F1, item F5: a menu row's own padding, vertical and horizontal. Two more deliberate
    // off-scale steps, named for their purpose exactly as Gutter and PanelHeader are: a menu row
    // is denser vertically and roomier horizontally than the base scale offers, and rounding
    // either to XS/S/M would change how every application menu reads.
    MenuItemY = 6,
    MenuItemX = 10,
};

// Control and chrome extents.
enum class Size : int {
    // Owned canvas, dialog and multiline extents; shared by projections and audits.
    Hairline = 1,
    NodeGrid = 16,
    NodeCardMin = 128,
    NodeCardWidth = 240,
    NodeTitleBand = 32,
    ImageThumbnail = 72,
    NodeSocketDot = 8,
    NodeLinkHandleMin = 32,
    NodeColumnGap = 80,
    NodeRowGap = 24,
    NodeRerouteDot = 10,
    SelectionEdge = 2,
    PlayheadHalfWidth = 5,
    PlayheadHeight = 6,
    RulerLabelGap = 10,
    RulerLabelInset = 3,
    MinorTick = 4,
    MajorTick = 8,
    ViewerChecker = 22,
    ViewerWorkPadding = 48,
    ToolColumnWidth = 32,
    ViewerChannelWidth = 80,
    ViewerModeWidth = 108,
    ViewerTimecodeWidth = 104,
    ViewerZoomWidth = 64,
    ViewerResolutionWidth = 96,
    ViewerMinWidth = 220,
    ViewerMinHeight = 176,
    MultilineHeight = 78,
    PropertyLabelCompact = 64,
    DialogTextWidth = 520,
    DiagnosticHeight = 140,
    HeaderRow = 32,
    FooterRow = 32,
    ListRow = 32,
    PropertyRow = 28,
    IconChrome = 16,
    IconControl = 20,
    ToggleCell = 24,
    DropdownWidth = 100,
    DropdownWidthCompact = 108,
    DropdownWidthWide = 120,
    DropdownWidthExpanded = 240,
    ControlCompact = 26,
    Control = 26,
    ControlRoomy = 32,
    IconSmall = 12,
    IconMedium = 16,
    IconLarge = 20,
    TitleBar = 34,
    PanelHeader = 30,
    // Compatibility name for panel chrome; node-card PanelHeader is independent.
    EditorHeader = HeaderRow,
    TimelineRow = 32,
    TimelineBar = 20,
    TimelineToggleColumn = 4 * ToggleCell,
    TimelineNameMin = 120,
    TimelineNameDefault = 280,
    TimelineLeftColumn = TimelineToggleColumn + TimelineNameDefault + 2 * DropdownWidth,
    TimelineColumn = 100,
    TimelineWorkArea = 10,
    TimelineSeparator = 2,
    TimelineChromeGutter = 32,
    TimelineWorkAreaHandle = 6,
    TimelineNavigatorThumb = 6,
    // GRAPH-1: the graph editor's left value-axis gutter, and the dot that terminates an ease
    // handle's hairline.
    GraphValueAxis = 48,
    GraphHandleDot = 6,
    GizmoHandle = 8,
    GizmoRotateZone = 16,
    ScrollBar = 8,
    ScrollBarHover = 12,
    // task S1, item 3: the narrowest a QMenu popup may be, roughly half again the width Bloom's
    // shortest menus used to collapse to. Applied by kit::AltUnderlineProxyStyle to every menu ROW,
    // because a menu's width is the widest row it holds -- forcing the popup window wider instead
    // would leave its rows short of the frame, and a menu row's hover bar must be full width.
    MenuMinWidth = 200,
    // task WIDTH-1 (owner: "min width of something like 300px in figma pixel, so ... users can
    // compromise to also have that strict min width instead of kicking borders around"). Every
    // EditorArea reports exactly this as its own minimumSizeHint() width, independent of whatever
    // the hosted editor's own content demands -- see editor_area.cpp. Figma design px, through
    // kit::px() like every other Size token.
    PanelMinWidth = 300,
    // task WIDTH-1: the floor a Properties value cell (kit::KValueField) may shrink its own width
    // to before the row it lives in falls back to the panel's own horizontal scrollbar as a last
    // resort. See kit::KValueField::minimumSizeHint().
    ValueCellMin = 72,
    PropertiesLabelWidth = 96,
    PropertiesLabelMinWidth = 64,
    PropertiesRowPitch = 28,
    PropertiesDiamondColumn = 20,
    PropertiesFieldWidth = 72,
    PropertiesFieldMinWidth = 64,
    PropertiesColorMinWidth = 60,
    PropertiesComponentMinWidth = 48,
    PropertiesDropdownWidth = 108,
    PropertiesSearchWidth = 180,
    PropertiesCheckBox = 12,
    PropertiesSwatchHeight = 26,
    PropertiesAnchorDot = 8,
    PropertiesStepperWidth = 12,
};

[[nodiscard]] constexpr int px(const Spacing token) noexcept { return static_cast<int>(token); }
[[nodiscard]] constexpr int px(const Size token) noexcept { return static_cast<int>(token); }

// Default Compositing split fractions, independent of physical DPI.
namespace Layout {
inline constexpr double SidebarShare = 0.1875;
inline constexpr double TimelineShare = 0.48;
inline constexpr double NodesShare = 0.50;
inline constexpr double PropertiesShare = 0.68;
inline constexpr int WorkspaceVersion = 2;
} // namespace Layout

// Border widths in design pixels. The focus ring is drawn OUTSIDE the control's own rectangle so
// gaining focus never shifts layout.
inline constexpr qreal kCompositionFrameWidth = 1.0;
inline constexpr qreal kNodeLinkWidth = 1.0;
inline constexpr int kLinkActiveLightness = 135;
inline constexpr qreal kDiamondStroke = 1.5;
inline constexpr double kMenuWindowHeightShare = 0.5;
inline constexpr qreal kKeyDiamondRadius = 4.5;
inline constexpr qreal kNodeCanvasHalfExtent = 256.0 * 1024;
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
    TimelineBar,
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
