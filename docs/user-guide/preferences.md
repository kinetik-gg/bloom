# Preferences

Bloom keeps two kinds of saved choice apart:

- **Global preferences** are user-wide choices for your installation. They live in this Preferences
  window and are stored outside any project.
- **Project settings** are part of one `.bloom` file. Colour configuration is the current example,
  under **File → Project Settings…**.

Open **Edit → Preferences…** (⌘, on macOS, where the operating system moves it into the Bloom
application menu). The window uses **OK**, **Cancel**, **Apply**, and **Restore Defaults**. Apply
saves without closing; Cancel discards anything not applied. Restore Defaults asks first (Cancel is
the default answer) and only resets the window's values; nothing is saved until OK or Apply. A cache
folder that is a file, or one Bloom cannot write or create, is refused under the field and OK waits.

## Pages

| Page | What it controls | When it takes effect |
| --- | --- | --- |
| General | Audio playback, loop playback | Immediately |
| Memory & Caches | Operation-cache and RAM-preview budgets, media disk cache enable/directory/budget | Memory budgets immediately; disk cache after restart |
| Timeline | Time format, snapping, keyframes, graph editor, layer column width | Immediately |
| Node Graph | Link style, snap to grid, grid size | Immediately |
| Viewer | Resolution, the background around the composition frame (Solid, Grey, Black, White), the transparency grid inside it, and the safe-areas, centre-cross, thirds, rulers and pixel-grid overlays | Immediately |
| Sounds | Play sounds (master), volume, event sounds, interaction sounds, render notifications, and a Play test sound button. See [UI sound feedback](../ux/sound.md) | Immediately |
| Performance | Read-only acceleration status | Not a preference |

Immediate changes reach every open panel: the window hands the committed value to each editor,
which applies it through the same setters its own menus use. Memory budgets update the running cache owners on Apply. Values marked **Restart** (the disk
cache enable, location and disk-space budget) are read when Bloom starts.

## Memory and disk budgets

**Automatic** (stored as 0) selects the machine-derived default. See [Memory](memory.md) for how Bloom derives
the defaults, and for the two cache ceilings. The disk cache can be purged at any time from
**Edit → Purge… → Purge media cache**, which clears the on-disk store and the applicable in-memory
decoded media off the interface thread; the source files are never changed.

## Project, session, and per-area state

Not everything persisted is a global preference. This window deliberately excludes:

- per-area and per-revision viewer analysis, display-view, and look state;
- per-composition safe-area presets;
- per-project export settings;
- window geometry and the workspace layout, which are session state rather than preferences.

Those are restored by the surface that owns them. Keeping them out of Preferences keeps one owner for
each value.

## Performance and GPU

The Performance page is read-only. It reports which backend is evaluating work. This version has no
GPU backend, so it reports the CPU reference path and leaves deterministic output unaffected. When
GPU execution ships, a capability-report-backed provider fills in the device, driver, and
per-operation qualification; the page needs no change, and GPU policy preferences (currently a
reserved `render/` key namespace) will be added deliberately rather than inferred from having a GPU.
See [`architecture/gpu-backend.md`](../architecture/gpu-backend.md).
