# Organizing Assets

## Inspector

The Assets panel has Media, Data, and Compositions filters. Data shows authored non-media blocks
with their kind, provenance summary, and digest. Media continues to use the existing asset folders,
tags, relinking, and missing-media indicators.

Import media with **File > Import**, the Assets footer's **Import** button, or by dropping files
onto the Assets panel. Imports keep stable identities, so organizing them preserves the nodes
and layers that already use them.

## Folders and names

Choose **Add > New Folder** or the footer's **New Folder** button. The new folder opens for
renaming immediately. Select an existing folder first to create a child folder; select a root
asset or the Compositions root to create a root folder. Use the caret beside a folder to open
or close it, or **View > Expand All / Collapse All** for the tree.

To rename an asset or folder, select it and press **F2**, or right-click and choose **Rename**.
Press Enter to commit or Escape to cancel. Asset names are display names inside the project;
the media file's path stays the same. Existing projects receive file-stem display names when
opened, and you can change them independently.

Drag one or more assets onto a folder to move them there. Drop on empty space to move them back
to the project root. Drop above or below another asset to change their order. Dragged assets keep
their relative order, and the arrangement is saved with the project.

**Remove Folder** moves its contents into its parent: assets append after that parent's existing
assets, and child folders become siblings. If a child folder would conflict with an existing
sibling name, rename the conflicting folder first. Removing a folder does not remove media.

## Tags and search

Right-click an asset and choose **Edit Tags…**. Enter one tag per field, use **Add Tag** for
another, or the × button to remove one, then choose **Apply**. Blank fields are ignored. When
multiple assets are selected, Apply replaces the tags on all of them. Tags are stored as a
sorted, unique set, with up to 64 tags per asset.

A tagged row shows its first tag and a count of any others. Click the tag to filter by it, or
click the count to edit the complete set. Narrow panels show just the complete tag count to keep
asset names readable. Hover over a chip to see all tags.

Type a name or tag in Search to narrow the list. Use **tag:hero** to match only tags containing
“hero”; names alone will not match that query. Searches ignore letter case. Matching assets keep
their folder ancestors visible, and clearing Search restores your previous folder expansion.

## Using organized media

Dragging an image or sequence to Nodes creates an image source; dragging audio creates an audio
source. Dropping media on Timeline creates the matching layer. Cards, Properties asset selectors
and imported default Timeline labels show the asset's display name. A layer name you author
separately remains independent.

Compositions stay in their own **Compositions** listing. Double-click one to open it. Dragging a
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

Folder edits, names, tags, moves and reorder are undoable. Saving records the organization in
project schema 1.17. Opening a 1.15 project supplies the new defaults without changing its media
references; saving then writes the current schema. Use **Relink…** for missing media; relinking
preserves the asset's name, folder, tags and order.

## Importing Video

On Linux, choose **Import** in Assets and select an MP4, MOV, MKV, MXF, M4V or supported
transport-stream file. Bloom probes the file in its media worker and lists it as
**Video · HH:MM:SS**. A malformed or unsupported file produces a diagnostic. Native video
providers for macOS and Windows are not available yet.

Drag the asset into the Timeline to create a Video source and Layer at the normal Arrange position.
The Layer carries image and, when present, audio connections. Its node card shows the first frame;
a video with audio also supplies a waveform. Properties exposes Asset, Start Frame, Loop Mode and
Colour Space. Start Frame offsets both image and audio. Loop and Ping-pong affect the image;
audio follows the ordinary clip duration.

This preview path supports explicit Rec.709 matrix/primaries with Rec.709 or sRGB transfer.
Rec.2020, HLG and PQ footage is currently unavailable. A file changed outside Bloom is marked
unavailable until relinked, including when its old frame was cached.

A ProRes asset's tooltip states: “Decoded by FFmpeg; not an Apple-authorized ProRes implementation”.
This preview workflow carries no Apple authorization or ProRes delivery qualification.
