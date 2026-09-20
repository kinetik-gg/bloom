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

KEY-2 approval: both references are reapproved under Qt 6.8.3 with `--update-goldens` at DPR 1
and 1.5. The Background fixture now keys only Position X and keys Opacity, exposing gold half-filled
parameter, filled component and empty animated component diamonds alongside muted constants. Its
Position disclosure opens separate X and Y rows and lanes. Properties and node cards show the new
component diamonds, node colour cards expose RGBA rows, and both Parent dropdowns are enabled.
Both full-window captures were visually inspected for control containment and readable row alignment.
Comparison tolerances and full-image coverage are unchanged; process/output identity goldens are
unchanged.

DM-1 approval: both whole-window references are reapproved under Qt 6.8.3 at DPR 1 and 1.5.
The selected Background now shows eight Surface-filled, Accent-outlined scale handles and an
anchor crosshair on its selection box. Cosmetic hairlines and handle vertices are aligned to
device pixels; rotation hit regions sit outside the corners without adding persistent chrome.
Both complete captures were inspected for clear handles and unchanged panel/control layout.
Comparison tolerances and full-image coverage are unchanged. Process/output identity goldens
are unchanged; these controls are viewer overlays and never alter composition pixels.
TL-FIX2 approval: both references are reapproved under Qt 6.8.3 with `--update-goldens` at DPR 1
and 1.5 (automatic comparison against the prior references had stayed under tolerance -- the changed
region is a real but small fraction of the full window -- so this is a deliberate re-approval of an
intentional change, not a forced-through failure). Two fixes to the Background fixture's expanded
timeline hierarchy: Object/Transform/Source and their parameter rows now NEST one step per depth
(layer, group, parameter, component) instead of sitting flush with the layer row and each other, so
Position's own X/Y rows land one step further right than Position itself, which lands one step right
of Object/Transform. And Position -- a vector parameter -- now shows exactly its own one diamond
instead of that diamond plus one more per component field; its expanded X and Y rows still show their
own one diamond each, unchanged. Both full-window captures were visually inspected for the new
staircase indent and the corrected diamond count. Comparison tolerances and full-image coverage are
unchanged; process/output identity goldens are unchanged.

SHAPE-2 approval (2026-09-17, Qt 6.8.3): the viewer column now contains ten enabled tools.
Text, Rectangle and Pen use enabled ink; Ellipse, Polygon, Star and Line add SVG glyphs,
and Pen moves below them. Both references were regenerated with `--update-goldens` at DPR 1
and 1.5 and visually inspected. Against the previous references, changes are confined to the
tool column: (8,166)–(40,356) at DPR 1 and (12,250)–(60,534) at DPR 1.5. Window geometry,
composition pixels and all other panels are unchanged. Rendering identity goldens are unchanged.

WORKSPACE-1 approval (2026-09-17, Qt 6.8.3): both references were regenerated with
`--update-goldens` at DPR 1 and 1.5 and visually inspected. The intentional change is the
proportions-only workspace arrangement: Assets, Viewer, Nodes and Properties now share the full
top row at 16% / 31% / 32% / 19%, with Timeline across the 68% / 32% lower row and its layer-table
divider at 37%. The fixture's sample graph, selected content, composition pixels, controls and
full-window dimensions are otherwise unchanged. Comparison tolerances and full-image coverage
are unchanged; process/output identity goldens are unchanged.

ASSETS-2 approval (2026-09-17, Qt 6.8.3): Assets now has a separate expandable Compositions
root, kit disclosure and indentation, consistent selected-row backgrounds, and a live New Folder
footer control. The fixture has no asset folders; its builtin font retains the captured face label.
Both complete captures were visually inspected. These references also capture the ROI/exposure/gamma
viewer-footer controls already present at the lane base `55319c3`; the previous reference approval
`33f4f84` preceded those viewer commits. ASSETS-2 changes no viewer source.
The test printed mean channel errors of 0.474275 (DPR 1) and 0.468285 (DPR 1.5) against the previous
references, and 0 at both DPRs after `--update-goldens`. Comparison tolerances, full-image coverage
and rendering identity goldens are unchanged.

TABS-1 approval (2026-09-17, Qt 6.8.3): both references were regenerated with
`--update-goldens` at DPR 1 and 1.5 and visually inspected. The intentional change is the new
32px Properties leading filter strip, with five exclusive choices beside the existing panel body;
the workspace proportions, content dimensions, and rendering identity remain unchanged. Against
the previous references, the printed mean channel errors were 0.447695 at DPR 1 and 0.403819 at
DPR 1.5; after approval both were 0 with changed fraction 0. Comparison tolerances and full-image
coverage are unchanged.

Merged-tree approval (2026-09-17, Qt 6.8.3): ASSETS-2 and TABS-1 each re-approved the references on their own base; the integration merge regenerated both captures once more so the references carry both the Assets tree changes and the Properties filter strip. Tolerances unchanged.

DATA-1 approval (2026-09-17, Qt 6.8.3): the Assets panel adds the Media/Data/Compositions filter row.
Both references were regenerated with `--update-goldens` at DPR 1 and 1.5 and visually inspected.
Before approval, the mean channel errors against the prior references were 0.340005 (DPR 1) and
0.336177 (DPR 1.5), with changed fractions 0.00219792 and 0.00193171; both were within the
existing tolerance. The fixture contains no data blocks, so the approved change is limited to the
Inspector filter chrome. Rendering identity goldens are unchanged.

COLOR-4 approval (2026-09-18, Qt 6.8.3): the Viewer footer adds the Display / View picker and
Look toggle. Both references were regenerated with `--update-goldens` at DPR 1 and 1.5 and
visually inspected. Before approval, the mean channel errors against the prior references were
0.112840 (DPR 1) and 0.110435 (DPR 1.5), with changed fractions 0.000763889 and 0.000756173;
both were within the existing tolerance. After approval both means were 0. The change is
viewer-only chrome; composition and rendering identity goldens are unchanged.

PROPS-2 approval (2026-09-21, Qt 6.8.3): the Properties panel loses its leading five-choice
filter strip, its header search field, and every section's "..." menu (with Collapse all /
Expand all); sections keep chevron, title, and Reset. Object and registry booleans render as
left-aligned KSwitch controls instead of stretched checkboxes, the scroll body reserves an XS
gutter only while overflowing, the anchor grid owns a two-line row so the Transform section no
longer clips it, and panel margins, section spacing, and section body padding compensate area
and row chrome so every visible band reads as the 6px inter-panel Gutter. Both references were
regenerated with `--update-goldens` at DPR 1 and 1.5 and visually inspected. Before approval,
the mean channel errors against the prior references were 1.2433 (DPR 1) and 1.190048
(DPR 1.5), with changed fractions 0.00906337 and 0.00784703; the changed pixels are confined
to the Properties panel and one node-card switch row, with composition pixels, viewer, and
timeline unchanged. After approval the DPR 1 error is 0. Comparison tolerances and full-image
coverage are unchanged; rendering identity goldens are unchanged.
