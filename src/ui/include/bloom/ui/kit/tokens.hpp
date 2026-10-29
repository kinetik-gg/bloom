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

// Semantic color roles. Data* roles are the data-type palette used to identify item kinds.
enum class Color : std::uint8_t {
    Background,
    Canvas,
    CompositionFrame,
    SurfaceSunken,
    Surface,
    SurfaceRaised,
    Field,
    Foreground,
    Muted,
    Faint,
    Border,
    BorderHover,
    // A neutral border used by navigator-style controls while dragged: one clear step brighter than
    // BorderHover, so hover and drag read as two states. The active-panel border is Accent.
    BorderActive,
    // Selection, focus, the active-panel border, and the playhead.
    Accent,
    OnAccent,
    AccentHover,
    AccentPressed,
    Keyframe,
    Ok,
    Warn,
    Error,
    // The small-swatch alpha checkerboard's two mid-grey tones (chips, swatches, the picker's alpha
    // bar). A mid-grey grid reads as "transparent" behind a few pixels of any colour, where a pair
    // of interface surfaces would vanish under a dark one.
    CheckerLight,
    CheckerDark,
    // The Viewer's Transparency Grid: a deeper pair for a whole frame of image, readable but quiet
    // next to dark footage, and distinct from the Canvas surround so the frame edge still reads.
    ViewerCheckerLight,
    ViewerCheckerDark,
    // The Viewer's Black, Grey and White surrounds: exactly black, a neutral mid grey and exactly
    // white, because judging an edge or a colour against a known value is the point of choosing them.
    CanvasBlack,
    CanvasGrey,
    CanvasWhite,
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
    // ADR 0026: the multi-channel ChannelSet transport. A green beside Image's teal: related, but a
    // ChannelSet never connects to an Image socket.
    SocketChannelSet,
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
// one level and keeps its hairline. The steps are spaced so each is perceptible on its own
// (hover, selection, alternating rows and Secondary buttons all ride one step).
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
    // The panel body's own radius, deliberately its own named step rather than a reuse of Small or
    // Medium. Trial: square panels (0). See docs/ux/ui-grammar.md before changing.
    Panel = 0,
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
    // The space between two groups of one chrome row (the Viewer footer's view, time and framing
    // groups): three ChromeGaps, so a group boundary reads as a boundary without a drawn divider.
    ChromeGroupGap = 12,
    // The air between a control widget's own rectangle and the box it paints, on every side and
    // for every control kind (buttons, dropdowns, value fields, icon buttons). One number, so a
    // KButton, a KDropdown and a KValueField in one row paint boxes of exactly the same height:
    // Size::Control minus twice this, which is Size::ToggleCell.
    ControlInset = 1,
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
    NodeCardWidth = 240,
    NodeTitleBand = 32,
    // The node editor's group frame title band (a node group's own header, not a panel header).
    NodeGroupTitle = 30,
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
    MinorTick = 4,
    MajorTick = 8,
    ViewerChecker = 22,
    ViewerWorkPadding = 48,
    ToolColumnWidth = 32,
    ViewerChannelWidth = 80,
    ViewerModeWidth = 108,
    ViewerZoomWidth = 64,
    ViewerResolutionWidth = 96,
    // The Resolution dropdown's compact form (Auto, Full, ½, ¼) when the footer runs short.
    ViewerResolutionCompactWidth = 64,
    // Display / View at most: typical view names ("ACES 1.0 SDR", the built-in config's
    // srgb_rec709_display) fit whole; longer ones elide and the tooltip carries the full display,
    // view and colour space.
    ViewerDisplayViewWidth = 176,
    // The exposure (EV) and gamma fields.
    ViewerAdjustWidth = 72,
    // The no-composition invitation's text column.
    ViewerEmptyStateWidth = 320,
    // The dot a Viewer value field shows while it differs from its default.
    NonDefaultMark = 4,
    ViewerMinWidth = 220,
    ViewerMinHeight = 176,
    MultilineHeight = 78,
    PropertyLabelCompact = 64,
    DialogTextWidth = 520,
    DialogWideWidth = 700,
    DiagnosticHeight = 140,
    // The KProgressBar track (#377): thicker than the 4-pixel slider track so a bar with no
    // handle still reads at a glance.
    ProgressTrack = 6,
    // Panel chrome rows. Independent of ListRow by design: a header can change height without
    // changing the pitch of every list in the application.
    HeaderRow = 32,
    FooterRow = 32,
    // The one list pitch: timeline layer and property rows, Assets, Jobs and every kit list.
    ListRow = 32,
    PropertyRow = 28,
    ToggleCell = 24,
    DropdownWidth = 100,
    DropdownWidthWide = 120,
    DropdownWidthExpanded = 240,
    Control = 26,
    ControlRoomy = 32,
    IconSmall = 12,
    IconMedium = 16,
    IconLarge = 20,
    TitleBar = 34,
    TimelineBar = 20,
    TimelineToggleColumn = 4 * ToggleCell + 3 * static_cast<int>(Spacing::XS),
    TimelineNameMin = 120,
    TimelineNameDefault = 280,
    TimelineLeftColumn = TimelineToggleColumn + TimelineNameDefault + 3 * DropdownWidth,
    TimelineColumn = 100,
    TimelineWorkArea = 10,
    // task TL-FIX2: the layer-table/lanes divider's draggable hit zone. KSplitHandle paints one
    // snapped hairline centered in it, so the handle is easy to grab without widening the line.
    SplitHandle = 6,
    TimelineWorkAreaHandle = 6,
    TimelineNavigatorThumb = 10,
    // GRAPH-1: the graph editor's left value-axis gutter, and the dot that terminates an ease
    // handle's hairline.
    GraphValueAxis = 48,
    GraphHandleDot = 6,
    // A node link's hit stroke: wide enough to grab a one-pixel wire.
    NodeLinkHit = 12,
    GizmoHandle = 8,
    GizmoRotateZone = 16,
    // A scrollbar's track is one constant width so hovering never relayouts its view; the thumb
    // rests at ScrollBarThumb inside it and fills the whole track under the pointer.
    ScrollBarTrack = 12,
    ScrollBarThumb = 8,
    // task S1, item 3: the narrowest a QMenu popup may be, roughly half again the width Bloom's
    // shortest menus used to collapse to. Applied by kit::AltUnderlineProxyStyle to every menu ROW,
    // because a menu's width is the widest row it holds -- forcing the popup window wider instead
    // would leave its rows short of the frame, and a menu row's hover bar must be full width.
    MenuMinWidth = 200,
    // A tooltip wraps rather than running across the screen.
    TooltipMaxWidth = 320,
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
    PropertiesDiamondColumn = 20,
    PropertiesFieldWidth = 72,
    PropertiesFieldMinWidth = 64,
    PropertiesColorMinWidth = 60,
    PropertiesComponentMinWidth = 48,
    PropertiesDropdownWidth = 108,
    // The Settings window's category rail: wide enough for the longest category label plus its
    // selection bar, sized in Figma design px through kit::px() like every other Size token.
    PreferencesRailWidth = 180,
    PreferencesLabelWidth = 184,
    PropertiesCheckBox = 12,
    PropertiesSwatchHeight = 26,
    PropertiesAnchorDot = 8,
    PropertiesStepperWidth = 12,
    // Kit control internals. Each is the control's own design measurement, named here so no kit
    // painter carries a private pixel constant.
    SliderTrack = 4,
    SliderHandle = 12,
    RangeTrack = 6,
    RangeHandle = 8,
    SwitchTrackWidth = 30,
    SwitchTrackHeight = 16,
    RadioMark = 14,
    // The selected radio's dot: an even number of pixels, so it centers on the pixel grid inside
    // the even RadioMark at every whole device pixel ratio.
    RadioDot = 6,
    // KColorPicker: the saturation/value square, a hue/alpha marker's diameter, and the alpha
    // checkerboard cell every colour widget uses.
    PickerSquare = 160,
    PickerMarker = 8,
    CheckerCell = 8,
};

[[nodiscard]] constexpr int px(const Spacing token) noexcept { return static_cast<int>(token); }
[[nodiscard]] constexpr int px(const Size token) noexcept { return static_cast<int>(token); }

// The control inset rule in one line: a control's painted box is exactly a toggle cell tall, so a
// row mixing toggles, buttons, dropdowns and value fields shows one box height.
static_assert(px(Size::ToggleCell) == px(Size::Control) - 2 * px(Spacing::ControlInset));

// Default Compositing split fractions, independent of physical DPI.
namespace Layout {
inline constexpr double SidebarShare = 0.1875;
inline constexpr double TimelineShare = 0.48;
inline constexpr double NodesShare = 0.50;
inline constexpr double PropertiesShare = 0.68;
inline constexpr int WorkspaceVersion = 2;
} // namespace Layout

// Stroke widths in design pixels.
inline constexpr qreal kCompositionFrameWidth = 1.0;
inline constexpr qreal kNodeLinkWidth = 1.0;
inline constexpr int kLinkActiveLightness = 135;
inline constexpr qreal kDiamondStroke = 1.5;
inline constexpr double kMenuWindowHeightShare = 0.5;
inline constexpr qreal kKeyDiamondRadius = 4.5;
inline constexpr qreal kNodeCanvasHalfExtent = 256.0 * 1024;
inline constexpr qreal kHairlineWidth = 1.0;
// An emphasised canvas stroke: a hovered or selected-endpoint node link, a cut/reroute gesture
// line, and the thickness that gesture tests crossings with.
inline constexpr qreal kEmphasisStroke = 2.0;
// The colour widgets' outside focus ring (drawFocusRing()), and nothing else: it is drawn OUTSIDE
// the focused element, in a margin the widget reserves, so focusing never shifts layout.
inline constexpr qreal kFocusRingWidth = 1.5;
// A hand-drawn stroke on a colour widget: the picker's SV dot and hue/alpha marker rings, and
// the colour chip's swap glyph.
inline constexpr qreal kMarkerStroke = 1.5;
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
    // UiSmall in the bundled SemiBold face: section headers and other small emphasis. A weight
    // set at a call site cannot reach the SemiBold file (the Medium family has no 600 face), so
    // emphasis is a role, never a setWeight().
    UiSmallStrong,
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

// The ONE disabled recipe: a role's own resting ink at kDisabledOpacity. The kit painters and the
// application palette both call this, so a disabled Muted label and a disabled Foreground button
// each fade from what they were rather than collapsing to one grey.
[[nodiscard]] QColor disabledInk(const QColor& resting);

// The selected-row wash: Accent composited over the row's own surface at this opacity, under a
// Size::SelectionEdge accent edge (painting.hpp, paintSelectedRow()). Subtle enough that content
// drawn over a selected row -- timeline bars in their kind or label colours -- stays readable.
inline constexpr qreal kSelectionWashOpacity = 0.24;

// Keyboard stepping for continuous kit controls (slider, colour picker canvases): one arrow press
// moves this fraction of the range, and Shift/Page moves kCoarseStepMultiple times as far.
inline constexpr double kKeyStepFraction = 0.01;
inline constexpr double kCoarseStepMultiple = 10.0;

// Canvas overlays: the dim over a layer bar outside its source range, a marquee's fill, and a
// drag ghost (the dragged thing's outline while it is in the hand).
inline constexpr qreal kDimOverlayOpacity = 0.6;
inline constexpr qreal kMarqueeFillOpacity = 0.15;
inline constexpr qreal kGhostOpacity = 0.6;

// The hover wash for a control that cannot know which ladder rung it sits on (a discrete radio
// row): Foreground at this opacity over whatever is behind it, about one ladder step.
inline constexpr qreal kHoverWashOpacity = 0.06;

// Secondary ink (a menu row's shortcut) on an Accent surface: OnAccent at this opacity. Faint
// would fall to ~1.6:1 on the Accent bar.
inline constexpr qreal kSecondaryInkOnAccentOpacity = 0.72;

// The opaque selection wash over `surface`: what a selected row's background is.
[[nodiscard]] QColor selectionWash(Color surface);

// Motion. Fast is hover and toggle feedback; Pop is the menu/popup entrance with a 4 design-pixel
// rise; None is mandatory for playhead, scrub, and viewer transforms -- direct editor feedback is
// never eased, because an eased playhead lies about where time is.
enum class Motion : std::uint8_t {
    Fast,
    Pop,
    // One indeterminate progress sweep across the track and back: slow enough to read as
    // "working", never as an alarm.
    Sweep,
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
