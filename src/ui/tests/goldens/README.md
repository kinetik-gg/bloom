# Whole-window references

`bloom_window_golden_test` captures the real 1920 × 1200 logical-pixel MainWindow with
`window_fixture.hpp`: all five panels, a solid background and a selected text layer.
Settings are isolated, the CPU preview must be ready, motion is disabled, keyboard focus is
cleared and every widget receives Leave before the grab. No design image is used.

References are `window-dpr1.png` (1920 × 1200 physical pixels) and `window-dpr15.png`
(2880 × 1800). Dimensions must match exactly. The maximum per-pixel RGB-channel error is
averaged over the complete image; its mean must be at most 1.2 out of 255, and no more than
1% of pixels may exceed a 24-level channel error. Both limits must pass. This permits small
rasterizer differences between supported Qt versions while catching changed layout, palette
or text. There are no excluded regions or automatic updates.

To deliberately approve a change, run the executable with `--update-goldens` once with
`QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 BLOOM_REDUCED_MOTION=1` and once with scale 1.5.
Inspect both captures, commit the references, and explain the visual change in that commit.
Normal ctest runs only compare. Every run also writes `build/grammar1-window-dpr1.png` or
`build/grammar1-window-dpr15.png` for inspection.

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
