# Preferences

Bloom keeps two kinds of saved choice apart:

- **Global preferences** are user-wide choices for your installation. They live in this Preferences
  window and are stored outside any project.
- **Project settings** are part of one `.bloom` file. Colour configuration is the current example,
  under **File → Project Settings…**.

Open **Edit → Preferences…** (⌘, on macOS, where the operating system moves it into the Bloom
application menu). Choose a category in the sidebar on the left (the arrow keys, Home, End and
typing a category's name move through it); Bloom shows the last category you used again for the
rest of the session. The window uses **OK**, **Cancel**, and **Apply**. Apply saves without
closing; Cancel discards anything not applied. **Restore Defaults** is in the menu button at the
foot of the sidebar: it asks first (Cancel is the default answer) and only resets the window's
values; nothing is saved until OK or Apply. Each section's reset button returns just that section to
its defaults, again only in the window. A cache folder that is a file, or one Bloom cannot write
or create, is refused under the field and OK waits.

## Pages

| Page | What it controls | When it takes effect |
| --- | --- | --- |
| General | Audio playback, loop playback | Immediately |
| Sounds | Play sounds (master), volume, event sounds, interaction sounds, render notifications, and a Play test sound button. See [UI sound feedback](../ux/sound.md) | Immediately |
| Viewer | Resolution, the background around the composition frame (Solid, Grey, Black, White), the transparency grid inside it, and the safe-areas, centre-cross, thirds, rulers and pixel-grid overlays | Immediately |
| Timeline | Time format, snapping, keyframes, graph editor, layer column width | Immediately |
| Node Graph | Link style, snap to grid, grid size | Immediately |
| Memory & Caches | RAM and VRAM headroom, media disk cache enable/directory/budget | Headroom on Apply or OK; disk cache after restart |
| Performance | Read-only acceleration status | Not a preference |

Immediate changes reach every open panel: the window hands the committed value to each editor,
which applies it through the same setters its own menus use. Headroom reaches the engine on Apply or OK. Values marked **Restart** (the disk
cache enable, location and disk-space budget, marked once in the Media disk cache header) are read
when Bloom starts. Leave the cache directory empty for the default location; **Browse…** picks a
folder and the reset button beside it returns to the default.

## Memory headroom and disk budget

**RAM headroom** and **VRAM headroom** are the memory Bloom leaves free for the rest of your
computer; Bloom uses everything else for rendering and caching. Each is a size from 0 to 256 GiB and
defaults to 2 GiB, so a busy render leaves room before the system runs short and crashes. They are
stored as decimal byte strings under `memory/ramHeadroomBytes` and `memory/vramHeadroomBytes`; a
missing, non-numeric or overflowing value reads as 2 GiB, and a number above 256 GiB reads as 256 GiB.
The older `playback/operation-cache-bytes` and `playback/ram-preview-memory-bytes` preferences are
no longer settings and are ignored.

The disk cache budget, and the timeline layer column width, have a mode: **Automatic** (stored as 0)
selects the machine-derived default, and **Custom** shows the value to enter. See
[Memory](memory.md) for how Bloom derives the defaults. The disk cache can be purged at any time from
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
