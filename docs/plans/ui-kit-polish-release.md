# UI kit polish release

Status: shipped

Updated: 2026-10-30

This record states what the UI kit polish release delivered, the evidence it was validated with,
and what has not been verified on macOS and Windows. The contracts themselves live in
[`ux/ui-grammar.md`](../ux/ui-grammar.md), [`ux/visual-language.md`](../ux/visual-language.md) and
[`ux/interaction-model.md`](../ux/interaction-model.md); this page does not restate them.

## What shipped

- **Tokens and painting.** One surface ladder (Background, Surface, SurfaceRaised, Field), the wider
  Accent triple, one selected-row recipe (wash plus a 2 px Accent edge) shared by Timeline, Assets,
  Jobs and `KRow`, device-pixel snapping for every rounded surface and hairline, and a cached icon
  engine. `kit::buttonColors()` and `kit::fieldColors()` are the only sources of control colour.
- **One control look and height.** Buttons, icon buttons, dropdown fields, radio groups and colour
  chips share the Secondary recipe; text entry shares the Field recipe; every control is one
  `Control` slot tall (`control_geometry_tests` audits 130 controls and 42 node rows at DPR 1, 1.5
  and 2). Ghost remains only on disclosure carets and the title bar buttons.
- **Typography.** Inter Variable and Geist Mono are bundled and identical on every platform; every
  chrome text path takes its font from `kit::font(role)`. Only text-layer content uses other fonts.
- **Popups.** `KPopupWindow` and `KPopupList` are the one popup mechanism for dropdowns, menus,
  searches and the colour picker, with placement that flips at the screen edge, a shared elevation
  shadow, and keyboard-usable flow menus.
- **Fields.** `KValueField` fits its text, commits on Escape, takes the shared refusal style, and
  has `QAccessible` support; `KTextField`, `KSearchField`, `KTextArea` and `KFieldFrame` use the
  Field recipe; the Viewer frame field reads a frame index or a timecode.
- **Dialogs.** Every dialog is a `KDialog`; `kit::confirm`, `kit::inform`, `kit::promptText`,
  `kit::promptRange`, `kit::chooseFile` and `KTaskDialog` replace the Qt convenience dialogs, and the
  Composition, Export, Preferences and import dialogs follow the one-height rule.
- **Viewer.** Priority chrome rows with ranked collapse, the Transparency Grid, the surround
  background separate from the composition background, and a GPU present path that draws the
  checker only inside the frame.
- **Context bar and tools.** ADR 0031: one app-wide active tool, a fixed contextual bar under the
  menu bar, and Align/Distribute as one undoable command (`docs/architecture/layer-alignment.md`).
- **Assets, sounds, preferences.** The Assets panel rewrite, UI sound feedback with its Preferences
  page, and the Preferences window pages.
- **Quality gate.** `bloom-quality-check` and the UI grammar check now reject, outside the kit: raw
  controls (including `QPlainTextEdit` and `QTextEdit`), the Qt convenience dialogs and spin boxes,
  numeric `QColor` construction and colour strings, `.darker(`/`.lighter(`, palette overrides,
  `setStyleSheet`, raw `Qt::Popup`/`Qt::ToolTip` windows, literal pen widths, literal
  `resize(w, h)`, and Ghost buttons other than disclosure carets. The kit's own sources are held to
  the pixel-literal rule. The allowlist is empty. The `architecture_boundaries` check passes: its
  nine findings in test files (relative private includes, a render include from host tests, a color
  include from a scripting test and a comment word) were fixed in the tests.

## Validation evidence

- Full desktop build of every target and the full desktop CTest run on the merged tree: 497
  tests, 477 pass, 19 are skipped because they need a native GPU presentation device, and one
  (`bloom.runtime.gpu_coverage_native_acceptance`) fails offscreen by design and passes under the
  route proofs below.
- Route proofs in a private headless Weston: routes 9/9 (Viewer, RAM preview, still frame, sequence
  range, video, headless scripted export); native coverage 98 pass, 0 fail, 0 missing.
- Window goldens at DPR 1 and 1.5 pass.
- The four load-sensitive tests (`live-value-edit`, `main_window_chrome`,
  `composition_preview_controller`, `node-card-value-gesture`) were reproduced under 32-way
  parallel stress and fixed at the cause: each asserted state after a fixed delay or acted on a
  control that was not yet enabled. They now wait for the state itself, and pass 100 to 400
  consecutive stressed runs each.
- Review captures (offscreen, DPR 1 and 1.5): the main window under each tool group's context bar,
  a dropdown flipped above a field at the screen bottom, the searchable font dropdown, the
  Composition, Export, Preferences, Unsaved Changes and Delete dialogs, value fields at rest, being
  edited and refusing text, selected rows in the Timeline and Assets, and the Viewer checkerboard.

## Not verified on macOS and Windows

The release builds and runs its automated checks on Linux only. The following have no macOS or
Windows evidence:

- Popup translucency and the elevation shadow gutter (macOS and Windows should carry alpha; the
  Linux X11 fallback is flat popups), and the gutter-press replay, which is disabled on macOS.
- Native file dialog sidebars and the kit file dialog's network-share entries.
- Bundled font rendering and hinting differences, and the title bar buttons.
- Pointer wrap during a value-field scrub on Wayland (verified only against the offscreen platform).
- The UI sounds' audio output backends.
- Native GPU execution; macOS and Windows use the supported CPU fallback.
