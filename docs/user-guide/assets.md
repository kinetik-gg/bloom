# Organizing Assets

## The project panel

Assets works like the After Effects Project panel. One tree holds your folders, compositions and
imported media, with columns for **Name**, **Type**, **Size**, **Duration**, **Frame Rate**,
**Tags** and **File Location**. Click a column header to sort by it, and click again to reverse;
folders always stay at the top. Drag a header divider to resize a column or a header to move it.
Right-click the header, or open **View > Columns**, to choose which columns show. Bloom remembers
your columns for the next session.

Select an item to see it at the top of the panel: a thumbnail (or the item's symbol), its size,
duration and frame rate, how its colour is interpreted (including the note when Bloom had to assume
the colour of an untagged video), its alpha, and how often it is used in compositions.

The **Data** group at the bottom lists authored non-media blocks with their kind, provenance
summary and digest. Missing media shows a warning symbol instead of its type symbol; hover over it
for the reason.

Import media with **File > Import**, **Add > Import…**, the footer's **Import** button, or by
dropping files or folders onto the panel. Imports land in the selected folder (or the folder of the
selected item); a drop lands in the folder you drop on, or at the top level on empty space. Files
and folders dropped on the Timeline or Nodes are imported into the selected folder too (they are not
placed as layers). Dropping a folder imports every image, image sequence, video, audio file and LUT
inside it and its subfolders; other files and hidden files are left out. Imports keep stable
identities, so organizing them preserves the nodes and layers that already use them.

### Import progress

An import that takes longer than a moment shows its progress. First Bloom finds what to import:
it searches folders you dropped and counts the frames of each image sequence ("Finding frames…
522 found"), with a moving bar because it does not know the total yet. Then it reads each file and
the bar fills: a sequence shows "Reading frame 120 of 522", and its name with "· sequence"
("Importing SC020_SH070_image · sequence"). An image sequence is one item however many of its
frames you chose, but every frame counts towards the bar, so a long sequence moves it steadily.
When you import several items the heading also says which one it is on ("Importing 2 of 5: …").
Bloom waits 300 milliseconds before showing the window, so a quick import does not flash it at you.
The rest of Bloom waits while the import runs.

Choose **Cancel** (or press Escape) to stop, while finding or while reading. Bloom finishes at the next safe point: the file it was
reading is left out, and everything imported before you cancelled is kept. One **Undo** removes
what was kept.

When an import adds everything, the window closes and the status bar says how many assets were
imported. If anything was skipped or you cancelled, the window shows a summary instead, such as
"Imported 10 · Skipped 2", with every skipped file and the reason it was skipped. A file that is
already in the project is listed as skipped too. Choose **Close** or press Return when you have read
it. The import job in **Jobs** keeps the same reasons.

One unreadable or unsupported file never stops the rest: every valid file is added in a single step
that one **Undo** reverses.

## Folders and names

Choose **Add > New Folder**, the footer's **New Folder** button, or **New Folder** in any context
menu. The new folder opens for renaming immediately and is created inside the selected folder (or
the folder of the selected item). Folders hold media, compositions and other folders. Use the arrow
beside a folder, the Left and Right keys, or **View > Expand All / Collapse All** to open and close
them.

To rename an asset, composition or folder, select it and press **F2** or **Enter**, or right-click
and choose **Rename**. Press Enter to commit or Escape to cancel. Asset names are display names
inside the project; the media file's path stays the same.

Drag items onto a folder to move them there, onto any row to move them into that row's folder, or
onto empty space to move them to the top level. You can drag media, compositions and folders
together; one **Undo** reverses the whole move. A folder cannot be moved into itself.

## Context menus

Right-click an item for the commands that apply to it:

- **Media:** New Composition from Selection, Input Colour… (how its colours are interpreted),
  Replace Footage…, Reload Footage, Reveal in File Manager, Rename, Edit Tags…, Delete.
- **Compositions:** Open, Composition Settings…, Rename, Duplicate, Delete.
- **Folders:** Rename, Delete.

Every menu ends with New Folder, New Composition… and Import…, which is all you get on empty space.
**New Composition from Selection** makes a composition named and sized after the first selected
clip, with its frame rate and duration, holding a layer for each selected clip, and opens it.

## Deleting

Press **Delete**, use the footer's **Delete** button, or choose **Delete** in a menu. Unused items
go at once. If an item is used in a composition, Bloom tells you how often and asks first; the
layers that used it then show it as missing. Deleting a folder that holds items asks whether to
**Delete Folders Only**, which moves everything inside up one level, or **Delete with Contents**.
Every delete is a single step that one **Undo** restores.

## Tags and search

Right-click an asset and choose **Edit Tags…**. Enter one tag per field, use **Add Tag** for
another, or the × button to remove one, then choose **Apply**. Blank fields are ignored. When
multiple assets are selected, Apply replaces the tags on all of them. Tags are stored as a
sorted, unique set, with up to 64 tags per asset. The **Tags** column (choose it from the header
menu) lists them; click a Tags cell to filter by its first tag.

Type a name or tag in Search to narrow the list. Use **tag:hero** to match only tags containing
“hero”; names alone will not match that query. Searches ignore letter case. Matching items keep
their folders visible, and clearing Search restores your previous folder expansion.

## Using organized media

Dragging an image or sequence to Nodes creates an image source; dragging audio creates an audio
source. Dropping media on Timeline creates the matching layer. Cards, Properties asset selectors
and imported default Timeline labels show the asset's display name. A layer name you author
separately remains independent.

Compositions sit in the tree with your media and can be filed in folders. Double-click one to
open it. Dragging a
composition onto Nodes creates a Composition source. Connect its image or audio output as needed.
Drop it onto Timeline to create one nested layer, initially as long as the source composition
(up to the containing composition's duration). This is one undoable edit.

Select the source or its layer to choose the referenced composition in Properties. **Open** switches
to that composition so you can edit its contents. Changes appear wherever it is used. **Time Offset**
delays the source in seconds; **Time Scale** sets its playback speed. **Hold** stops on the last frame,
**Loop** repeats, and **Ping-pong** alternates forward and backward. Bloom refuses a reference that
would create a cycle. If a source composition is deleted, choose a replacement in Properties.

Nested audio follows the same offset, speed and loop settings. Multiple nested layers mix
together; a composition without audio is silent. Layer mute, solo and time ranges apply.

Folder edits, names, tags and moves are undoable and saved with the project; composition folders
need project schema 1.27, and older projects open with their compositions at the top level. Use
**Replace Footage…** for missing media; it preserves the asset's name, folder and tags.

## Importing Video

On Linux, choose **Import** in Assets and select an MP4, MOV, MKV, MXF, M4V or supported
transport-stream file. Bloom probes the file in its media worker and lists it as a **Video** with
its size, duration and frame rate. A malformed or unsupported file produces a diagnostic. Native video
providers for macOS and Windows are not available yet.

Drag the asset into the Timeline to create a Video source and Layer at the normal Arrange position.
The Layer carries image and, when present, audio connections. Its node card shows the first frame;
a video with audio also supplies a waveform. Properties exposes Asset, Start Frame, Loop Mode and
legacy Colour Space plus **Input colour space**. Input colour space is Auto by default and is
resolved from the selected OCIO configuration; choose an explicit config space when the container
tags are incomplete or the footage is LogC3/Log3G10. Start Frame offsets both image and audio. Loop and Ping-pong affect the image;
audio follows the ordinary clip duration.

Auto maps Rec.709-tagged streams to the config's Rec.709 camera/video space, sRGB transfer to its
sRGB-texture space, and linear transfer to the working space. The asset tooltip shows the numeric
container tags. Rec.2020, HLG and PQ footage remains unavailable on the unqualified preview path
and reports why. A file changed outside Bloom is marked unavailable until relinked, including when
its old frame was cached.

A ProRes asset's tooltip states: “Decoded by FFmpeg; not an Apple-authorized ProRes implementation”.
This preview workflow carries no Apple authorization or ProRes delivery qualification.
