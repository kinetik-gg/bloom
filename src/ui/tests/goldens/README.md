# Whole-window references

`bloom_window_golden_test` captures the real 1920 × 1200 logical-pixel MainWindow with
`window_fixture.hpp`: all five panels, a selected Solid background with its timeline groups expanded and Properties showing it, plus a text layer.
Settings are isolated, the CPU preview must be ready, motion is disabled, keyboard focus is
cleared and every widget receives Leave before the grab. No design image is used.

References are `window-dpr1.png` (1920 × 1200 physical pixels) and `window-dpr15.png`
(2880 × 1800). Dimensions must match exactly. The maximum per-pixel RGB-channel error is
averaged over the complete image; its mean must be at most 1.2 out of 255, and no more than
3% of pixels may exceed a 24-level channel error. Both limits must pass. This permits small
rasterizer differences between supported Qt versions and between a developer machine and the
CI runner's FreeType (glyph-edge antialiasing measured at 1.1% there), while catching changed
layout, palette or text, which move tens of percent. There are no excluded regions or automatic updates.

To deliberately approve a change, run the executable with `--update-goldens` once with
`QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 BLOOM_REDUCED_MOTION=1` and once with scale 1.5.
Inspect both captures, commit the references, and explain the visual change in that commit.
Normal ctest runs only compare. Every run also writes `build/ui3-window-dpr1.png` or
`build/ui3-window-dpr15.png` for inspection.

Initial approval: GRAMMAR-1 introduces shared 26 px controls, 32 px chrome and declared overflow.
The subsequent Inter and timeline row migrations deliberately re-approve their resulting changes.

GRAMMAR-1 row approval: timeline toggles now occupy four real 24 px kit cells with 20 px SVG
glyphs. Assets uses the same row's fixed Kind column. Property cards use 28 px rows without
extra inter-row spacing; narrow rows remove column gaps to retain readable field widths.
Shared chrome keeps overflow reachable and preserves the declared footer names.

Final Qt qualification approval: the fixture installs the application's mnemonic proxy after
the theme. Menu bars and table headers explicitly use TypeRole Ui, including after a style
change, eliminating Qt-version-dependent platform font substitution and window-layout drift.
Both references are reapproved for this typography correction with the same tolerance.

GRAMMAR-2 approval: the status strip uses muted UiSmall text with a tinted colour-state label.
The native menu bar is unchanged. The sidebar spans the workspace height, Assets is above
Properties, and Timeline spans only Viewer and Nodes. The padded viewer has six real tool
choices and a compact, ordered footer with one resolution readout. Cards use mixed-case
kind eyebrows, Title Case labels, kit controls at property-row pitch, aligned edge sockets,
and accent selection. Empty timeline rows use the shared alternating backdrop.
The sample graph is arranged through MoveNodes to keep its saved cards distinct; production
preserves artist-authored positions, including old compact placements. Both DPR references
are approved for these intentional changes; comparison limits and coverage are unchanged.

UI-3 approval: shared chrome padding and rounded clipping; square toggle/disclosure cells;
flat timeline rows with Background separators, collapsible Title Case groups and compact
property controls; padded time axis and gray work area with blue handles; sticky viewer
tool strip and measured dropdowns; node titles and muted categories in one band, separate
vector diamond columns and compact numeric precision. The selected Solid fixture exposes
the same Object/Transform/Source structure in Timeline and Properties. Both DPR references
are reapproved for these changes with the existing comparison tolerance and full coverage.

GRAPH-1 approval: the timeline's graph-editor toggle is a real control now, so its header glyph
reads in Foreground ink rather than the muted Faint of a disabled one. That single 16 px chrome
glyph is the whole difference: graph mode defaults OFF, so the fixture window still shows the key
lanes, the same five panels, the same layout and the same type. Both DPR references are reapproved
under Qt 6.8.3 with the existing comparison tolerance and full coverage.
