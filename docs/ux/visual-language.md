# Visual Language

Status: accepted

Updated: 2026-09-02

## Purpose

Bloom needs a coherent visual foundation for a dense professional interface. Icons and typography
must remain legible at small sizes, work across Linux, macOS, and Windows, and be usable without a
web runtime or JavaScript toolchain.

This document owns Bloom's current iconography, interface type, and design-token decisions. The
tables below are product truth and match `src/ui/include/bloom/ui/kit/tokens.hpp` exactly; the
header is the single implementation of them, and `src/ui/tests/kit_tokens_tests.cpp` asserts each
value listed here.

## Design Tokens

Kinetik is Bloom's visual language. Every color, gap, corner, size, elevation, type role, state,
and duration in the interface comes from the vocabulary below. No Bloom surface spells a raw hex
value, a raw pixel gap, or a raw duration of its own: the tokens are implemented once in
`bloom::ui::kit` (`src/ui/include/bloom/ui/kit/tokens.hpp`), and both the application stylesheet and
every painter resolve through them, so a stylesheet rule and a `QPainter` cannot disagree.

### Unit Rule

One design pixel is exactly one Qt logical pixel. Every number in these tables is in design pixels
at 1x. Physical pixels are derived from the device pixel ratio only inside painting code, never by
scaling a token where it is defined.

The one place that derivation is mandatory is a hairline. A plain one-logical-pixel pen straddles
two physical pixels at 125% and 150% scaling and reads as a blurred grey line, so hairlines are
snapped to a whole number of physical pixels at the device pixel ratio in use
(`kit::snappedHairlineWidth`).

### Color

| Role | Value | Use |
| --- | --- | --- |
| `Background` | `#111111` | The window, the workspace, and the visible gutters between panels |
| `Surface` | `#161616` | Panel chrome: headers, status bar, toolbars |
| `SurfaceRaised` | `#1B1B1B` | Menus, popups, dialogs, and raised controls |
| `Field` | `#202020` | Input cells: dropdowns, value fields, slider tracks |
| `Foreground` | `#FFFFFF` | Primary text and active icons |
| `Muted` | `#999999` | Secondary text, resting icons, units |
| `Faint` | `#666666` | Placeholder text, tertiary labels, ruler ticks and separators |
| `Border` | `#222222` | Resting hairlines |
| `BorderHover` | `#454545` | Hovered hairlines, scrollbar thumbs |
| `Accent` | `#0C8CE9` | Selection, focus, active state, the primary action |
| `AccentHover` | `#3AA5F0` | An accent surface under the pointer |
| `AccentPressed` | `#0A73C2` | An accent surface being pressed |
| `Keyframe` | `#F5C542` | Keyframes and animation markers |
| `Ok` | `#3FBF6B` | Success and ready states |
| `Warn` | `#E6A23C` | Warnings and unsupported states |
| `Error` | `#E0554E` | Failures and destructive actions |
| `Brand` | `#E879AB` | The Bloom logo only, never interface chrome |

Data-type palette, for identifying what a thing *is*:

| Role | Value | Kind |
| --- | --- | --- |
| `DataSequence` | `#E0554E` | Image sequences |
| `DataClip` | `#3FBF6B` | Clips |
| `DataComposition` | `#8B5CF6` | Compositions |
| `DataImage` | `#3AA5F0` | Still images |
| `DataAudio` | `#7C5CFF` | Audio |

`Background`, `Surface`, `SurfaceRaised`, and `Field` form the surface ladder, darkest first. The
state recipes below step along exactly this ladder and nothing else.

### Radius

| Token | Value | Use |
| --- | --- | --- |
| `Small` | `3` | Controls, chips, item rows |
| `Medium` | `6` | Popups, menus, cards |
| `Large` | `12` | Dialogs and panels |
| `XLarge` | `16` | Full-screen surfaces |
| `Full` | pill | Resolved as half the shape's own extent: switches, scrollbar thumbs, slider handles |

### Border

| Token | Value | Rule |
| --- | --- | --- |
| Hairline | `1` | Snapped to whole physical pixels at any device pixel ratio -- no blur at 125% or 150% |
| Focus ring | `1.5`, `Accent` | Drawn **outside** the control's rectangle, in margin the control's size hint already reserves, so focus never shifts a layout |
| Window | `1`, `Border` | The application window's own edge |

### Spacing

| Token | Value |
| --- | --- |
| `XXS` | `2` |
| `XS` | `4` |
| `S` | `8` |
| `M` | `12` |
| `L` | `16` |
| `XL` | `24` |
| `XXL` | `32` |
| `Gutter` | `6` |

`Gutter` is the visible `Background` gap between panels. Panels float on the window; they do not
share edges.

### Size

| Token | Value | Use |
| --- | --- | --- |
| `ControlCompact` | `22` | Dense chrome controls |
| `Control` | `26` | The default control height |
| `ControlRoomy` | `32` | Prominent controls and dialog buttons |
| `IconSmall` | `12` | Dense chrome |
| `IconMedium` | `16` | Default |
| `IconLarge` | `20` | Prominent actions |
| `TitleBar` | `34` | The application title bar |
| `PanelHeader` | `30` | An editor panel's header row |
| `TimelineRow` | `34` | One timeline row |
| `ScrollBar` | `8` (`12` on hover) | Overlay scrollbars with pill thumbs |

### Elevation

| Token | Shadow |
| --- | --- |
| `Flat` | none |
| `Popup` | `0 4 16 rgba(0,0,0,.5)` |
| `Dialog` | `0 12 40 rgba(0,0,0,.6)` |
| `Drag` | `0 18 48 rgba(0,0,0,.55)` |

A raised surface also steps up one level on the surface ladder and keeps its hairline. Elevation is
never the only thing separating a surface from what is behind it.

### Type Roles

| Role | Family | Size | Weight | Use |
| --- | --- | --- | --- | --- |
| `Ui` | Plus Jakarta Sans | `12.5` | 500 | The default interface text |
| `UiSmall` | Plus Jakarta Sans | `11` | 500 | Panel headers: uppercase, `+0.07em` tracking |
| `Value` | Geist Mono | `11.5` | 500 | Every numeric, unit, hex, and timecode surface |
| `Title` | Plus Jakarta Sans | `13` | 600 | Dialog and section titles |

Sizes are in design pixels. `Value` is monospaced so a column of numbers stays aligned and a
changing digit does not reflow the text beside it.

Static faces are shipped rather than the upstream variable fonts (see Font Packaging And Loading
below). A static face names its heavier weights as separate families -- the Medium face registers
as `Plus Jakarta Sans Medium`, not as `Plus Jakarta Sans` at weight 500 -- so a role asks for the
exact face first and the base family second, and the platform family last.

### State

| State | Recipe |
| --- | --- |
| Hover | One surface step up, plus `BorderHover`. At the top of the ladder the step clamps and the border change carries the state alone |
| Accent-item hover | A full-width `Accent` bar with `Foreground` text -- menu and list rows, never a rounded pill |
| Pressed | `AccentPressed` for an accent surface; one surface step down otherwise |
| Selected | An `Accent` fill, or a 2px inset accent edge where a fill would hide content |
| Focus | The focus ring, always visible for keyboard focus, always drawn outside the control |
| Disabled | Ink at 40% opacity, and no hover response at all |

A filled control that is not accent-colored -- a destructive action, for instance -- reproduces the
relation the accent triple already states rather than inventing its own pair of colors: hover
blends toward `Foreground` by the same amount `AccentHover` does, and press scales the channels by
the same factor `AccentPressed` does.

Color is never the only carrier of a state. A disabled control also stops responding; a selected
one also changes its fill or its edge; a toggle also moves its thumb.

### Motion

| Token | Duration | Curve | Use |
| --- | --- | --- | --- |
| `Fast` | `80ms` | ease-out | Hover feedback and toggles |
| `Pop` | `140ms` | ease-out, with a `4` design-pixel rise | Menus and popups appearing |
| `None` | `0ms` | -- | Playhead, scrub, and viewer transforms |

`None` is a rule, not a default. Direct editor feedback is never eased: an eased playhead or an
eased slider handle shows a value that is not the value.

Reduced motion is honored through an explicit kill switch (`BLOOM_REDUCED_MOTION`), because Qt 6.8
exposes no cross-platform reduced-motion style hint. Under it every duration collapses to zero and
every animated control jumps straight to its end state -- a toggle that cannot animate must still
be a toggle.

## Iconography

Phosphor Icons is Bloom's default interface icon family.

- Use the official raw SVG assets. Bloom does not depend on a JavaScript, Node, npm, or web runtime
  to obtain or render icons.
- Vendor a curated set of icons used by the product, pinned to an upstream release or commit. Do
  not use a Git submodule or download interface assets while configuring, building, or launching
  Bloom. The vendored subset, its pinned release, its archive digest, and a digest for every file
  are recorded in `src/ui/kit/third_party/phosphor-icons/provenance.md`.
- Retain the upstream MIT license and record asset provenance beside the vendored files.
- Preserve vendored upstream SVGs unchanged and enumerate them explicitly with `qt_add_resources`
  under a Bloom-owned resource prefix.
- Use `regular` as the default visual weight and `fill` for selected or toggled states. Add another
  weight only when testing shows a concrete legibility need at Bloom's supported control sizes.
- Access icons through a typed, semantic C++ API such as `IconId::SplitHorizontal`. Product code
  must not spread upstream filenames or resource paths through widgets.
- Render and tint SVGs through Qt, with caching that accounts for icon identity, size, state,
  palette color, and device-pixel ratio. Resolve Phosphor's `currentColor` from the applicable Qt
  palette role; do not assume the raw SVG automatically follows the application palette.
- An icon does not replace an accessible name. Icon-only controls require a tooltip and accessible
  text; destructive or ambiguous actions should include a visible label where space permits.
- Do not communicate an important state through icon color alone.

The complete Phosphor catalog is not embedded in Bloom's main executable by default. If a concrete
workflow later needs the entire catalog, package it as a pinned external Qt `.rcc` resource and load
it through `QResource`. This preserves a native C++/Qt build and avoids making every application
build compile thousands of unused assets.

Official sources:

- [Phosphor core assets](https://github.com/phosphor-icons/core)
- [Phosphor MIT license](https://github.com/phosphor-icons/core/blob/main/LICENSE)
- [Qt resource system](https://doc.qt.io/qt-6/resources.html)

## Typography

Plus Jakarta Sans is Bloom's primary interface typeface. Geist Mono is Bloom's monospaced
typeface.

Use Plus Jakarta Sans for:

- menus, editor headers, controls, labels, dialogs, properties, and timeline text
- headings and ordinary artist-facing documentation rendered inside the application
- numeric controls when proportional text is appropriate

Use Geist Mono for:

- code, expressions, scripts, logs, console output, and technical identifiers
- timecode, frame counters, channel values, and aligned numeric readouts where fixed character
  widths materially improve scanning

Do not use the monospaced family as a decorative substitute for hierarchy. Weight, spacing, and
layout should carry hierarchy in the normal interface.

## Font Packaging And Loading

- Vendor native font assets from pinned upstream releases; do not use a Git submodule, install them
  through npm, or fetch them from a network while configuring, building, or launching Bloom.
- Retain each upstream SIL Open Font License 1.1 file and record the exact version or commit.
- Prefer upstream variable TTF assets when they behave consistently through the supported Qt
  version on all three platforms. Keep a tested static-font fallback if variable-font behavior or
  packaging differs.
- Load bundled fonts through Qt's application font facilities. A missing or invalid bundled font
  must produce a diagnostic and fall back to the platform sans-serif or monospace family rather
  than preventing Bloom from opening.
- Let Qt provide glyph fallback for writing systems not covered by the bundled families.
- Use device-independent font sizes and verify the interface at common fractional and high-DPI
  scale factors. Do not rasterize interface text into image assets.
- Platform-owned chrome may retain its operating-system typeface when the platform does not allow
  application font control. Bloom does not introduce platform-specific menu or window chrome solely
  to force typography.

Initial interface weights are Regular, Medium, and SemiBold for Plus Jakarta Sans, and Regular and
Medium for Geist Mono. Additional weights or italics should enter the shipped set only when an
implemented component uses them.

The shipped set is the static TTFs, not the upstream variable fonts: the weight set above is
exactly five faces, those five are what implemented components use, and a static face resolves the
same way on every supported Qt platform without depending on the platform font engine's
named-instance handling. The vendored assets, their pinned releases, their archive digests, and a
digest for every file are recorded in `src/ui/kit/third_party/plus-jakarta-sans/provenance.md` and
`src/ui/kit/third_party/geist-mono/provenance.md`, and inventoried in the repository's
`THIRD_PARTY_NOTICES.md`.

Official sources:

- [Plus Jakarta Sans](https://github.com/tokotype/PlusJakartaSans)
- [Geist and Geist Mono](https://github.com/vercel/geist-font)

## Ownership Boundary

Icon IDs, font roles, palette roles, and rendering helpers belong to `src/ui`. UI components consume
these semantic roles rather than owning independent asset-loading or font-selection logic.
Optional editor modules use the same host-owned visual roles for standard controls. A module may
provide domain-specific artwork through an explicit registered resource boundary, but it must not
silently replace Bloom's global visual language.

Project documents never store an interface font or icon choice as render-affecting state. Fonts
selected by artists for composition content are project assets and follow a separate media,
licensing, substitution, and missing-dependency workflow.
