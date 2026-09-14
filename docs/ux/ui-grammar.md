# UI grammar

Status: accepted

Updated: 2026-09-14

Bloom's mechanical interface contract is owned here and implemented by `src/ui/kit`.
[ADR 0021](../decisions/0021-ui-grammar.md) records its rationale and extension procedure.
This contract supersedes conflicting component metrics in [Visual Language](visual-language.md).

## Control vocabulary

Every interactive control is a kit class: `KDropdown`, `KMenuButton`, `KButton`,
`KIconButton`, `KIconToggle`, `KCheckBox`, `KSwitch`, `KValueField`, `KSlider`,
`KSearchField`, `KSection`, or `KColorSwatch`. Text uses TypeRole-bound `KLabel`.
The panel switcher is the icon variant of **KDropdown**, with the same height, styling,
interaction and popup. Menus are created only through `kit::makeMenu`.
Constructing QToolButton, QPushButton, QComboBox, QLineEdit, QCheckBox, QSlider,
QLabel or QMenu outside the kit is a quality violation.

## Metric grid

All dimensions resolve through tokens. One design pixel is one Qt logical pixel.

| Token | Logical pixels | Contract |
| --- | --- | --- |
| `Size::Control` | 26 | All header, footer and row controls |
| `Size::HeaderRow` | 32 | Panel header |
| `Size::FooterRow` | 32 | Optional panel footer |
| `Size::ListRow` | 32 | List and timeline pitch |
| `Size::PropertyRow` | 28 | Property pitch |
| `Size::IconChrome` | 16 | Chrome glyph box |
| `Size::IconControl` | 20 | Control and toggle glyph box |
| `Size::ToggleCell` | 24 | Toggle column pitch |
| `Size::DropdownWidth` | 100 | List dropdown column |
| `Size::DropdownWidthCompact` | 108 | Property dropdown |
| `Size::DropdownWidthWide` | 120 | Header selectors |
| `Size::DropdownWidthExpanded` | 240 | Header selector cap |
| `Spacing::XXS / XS / S / M / L / XL / XXL` | 2 / 4 / 8 / 12 / 16 / 24 / 32 | Gutters |
| `Spacing::Gutter` | 6 | Panel separation |

Editor literals in fixed extents, QSize, pixel multiplication or geometry arithmetic are
quality violations. Existing specialized canvas tokens remain valid; new dimensions require
an owned semantic token, never a local pixel constant.

## Rasterization and type

Glyphs are SVG rasterizations at integer physical pixel extents for the current DPR, tested
at 1, 1.25, 1.5 and 2. Do not scale an existing pixmap or paint a glyph by hand.
Toggle on uses Fill; off uses Regular, centered in a ToggleCell column.
`TypeRole` is the only font API. The interface family is pinned, bundled Inter
(Regular, Medium, SemiBold; SIL OFL); Geist Mono remains the value face. Intake includes
upstream release identity, digests computed from downloaded bytes, license and manifest.
Render-module text-source DejaVu is independent and unchanged. `QFont(` outside the kit
is a quality violation.

## Declared chrome and rows

Panels return `EditorChromeSpec`: header switcher, selectors, menus and trailing actions;
footer leading controls, transport, readouts and trailing controls. `EditorArea` owns their
layout and one measure-only overflow algorithm, with hysteresis and a stable minimum.
Panels do not construct or paint header/footer widgets. The old take-header/footer and split
provider interfaces are removed. Object names remain stable for interaction and automation.

`KRow` owns list layout: fixed-pitch toggle cells, flexible name, fixed-width dropdown columns
and trailing cell. `KPropertyRow` owns the label, control and reserved diamond columns,
promoting the Properties layout into the kit. Timeline, assets and properties use these
shared row layouts as migration proceeds.

Painting outside the kit is limited to viewer canvas, node scene items, and timeline
ruler, lanes, work area and navigator. Other surfaces compose kit widgets.

## Enforcement and migration

`bloom-quality-check` rejects new raw controls, forbidden paintEvent, literal dimensions and
QFont construction. `tools/quality/ui_grammar_allowlist.txt` records remaining debt as
`file:line:kind`, prints its count, and may shrink during migration. Remaining entries are
GRAMMAR-2; the allowlist is not permission to introduce new variants.

Offscreen fixture-window metric audits run at DPR 1, 1.5 and 2. Whole-window references run
at DPR 1 and 1.5. Clear keyboard focus and deliver Leave before capture. Pixel assertions use
relative brightness. Goldens use a documented small tolerance; drift fails until an explicit
`--update-goldens` commit explains the visual change. References are generated from the
application and never from a design mockup.
