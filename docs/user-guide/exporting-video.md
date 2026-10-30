# Export

Exports run from the **Export** panel, a queue saved in the project, much like the After Effects
Render Queue. Open it by choosing **Export** in any panel's editor picker, or choose **File → Add to Export
Queue** (Ctrl+M). If an Export panel is already open Bloom brings it forward; otherwise the Timeline's
panel becomes Export, and with no Timeline a new panel opens along the bottom
([Workspace presets](workspace-presets.md#opening-the-export-panel)). The same **Add to Export Queue**
item is on a composition row in Assets, and you can drag
a composition from Assets onto the list. Bloom adds a job for the composition, reveals the panel
and selects the new job. A job renders one composition range to one or more outputs, one job at a
time in queue order, in the background while you keep working.

The panel is laid out like the After Effects Render Queue. At the top, **Current Render** says in
one line what renders now ("Rendering SC020 · frame 404 of 2156 · 0:28 elapsed · about 2 min
left", or **Idle**); open it with its caret for a progress bar. Below it is the render list, with
the columns **Render** (a check box: include the job in **Start All**), **#** (queue order),
**Composition**, **Status**, **Started** and **Render Time**. While a job renders, a thin bar sits
in its Status cell. Drag a row to change the order. Each row's **⋯** menu (or a right-click) has
**Start** (or **Pause**, **Resume**, **Stop** or **Render again**, whichever fits), **Details…**,
**Duplicate**, **Remove**, **Move up**, **Move down**, **Copy as bloom-cli command** and **Copy as
Python**. On a narrow panel Started and Render Time give way first.

Open a job with its caret to see its settings, one line each, as in After Effects:

- **Render Settings:** the range, for example **Work area · 0–239**. Click it (it is blue) for the
  **Render Settings** dialog; the **▾** before it picks **Work area**, **Whole composition** or
  **Current frame** at once.
- **Output Module:** the format, for example **ProRes 422 HQ**. Click it for the **Output Module**
  dialog; its **▾** picks another format, video first, then image sequences. **+** adds an output
  and **−** removes this one (a job keeps at least one output). **Output To:** shows where it saves;
  click it to choose a file, or use its **▾** for **Default name** or a folder you saved to before.

Each line opens with its own caret to one gray line of what it comes to: the frames and timecode,
the first and last file, the size Bloom can estimate (nothing when it cannot) and anything that
blocks it. **Start All** is the one blue button while a checked job is ready, and **Stop All**
shows only while something renders. A control that cannot act now says why in its tooltip.

## Jobs and outputs

Output keeps alpha: a composition's Background Colour is only shown in the Viewer behind the
composition, so exports and renders of a format that can carry transparency keep it, like After
Effects.

A new job renders the composition's work area (or the whole composition when it has none) to one
**H.264 MP4** output at `Exports/{name}.mp4` beside the project. If H.264 cannot run on this
computer, the output starts as **ProRes 422 HQ MOV** and the panel says why H.264 was not used. The
default is never an image sequence and never the last preset you used. An unsaved project has no
default destination; Start then says "Choose where to save this output".

Each job has a range, set in **Render Settings** (**Work area**, **Whole composition**, **Custom**
with a start and end frame and their timecode, or **One frame**; the dialog shows where the range
starts and ends and how long it is), and the **Render** check box. Each output has a format, its
quality settings, where it saves, what to do when a file is already there and **Add to Assets when
done**, set in **Output Module**. A dialog's **OK** is one undoable edit, and **Cancel** changes
nothing. Every edit is undoable and saved in the project. Run status, progress, ETA,
logs and verification are not saved: a reopened project shows every job **Ready**. A running job
cannot be edited ("Stop the job to change it; it keeps the settings it started with").

An image output writes one file per frame, or a single still when the range is one frame.

## Presets

Formats are listed in four groups:

- **Video**: H.264 MP4 (default), H.264 MOV, Review H.264 (Rec.709, look on), HEVC MP4 and MOV
  (hardware), ProRes 422 Proxy, LT, 422, HQ, 4444 and 4444 XQ MOV, DNxHR HQ MXF.
- **Image sequences**: VFX handoff EXR (ACES2065-1, PIZ, from 1001, `####`), OpenEXR, DPX, PNG
  and TIFF sequences.
- **Audio**: PCM WAV/BWF (RF64 when large).
- **User**: your own presets, saved per computer outside any project.

Choosing a format copies its settings into the output, so a project exports the same way on a
computer that lacks the user preset it came from. Every format is listed and chosen the same way;
the panel never asks you to install anything. If a format still cannot run when the job starts, the
job reads **Failed** with a plain sentence saying why, like any other failure.

The **Output Module** dialog is grouped like After Effects': the preset's own group (**Format**,
**If a file is there**, **Post-render**: **Add to Assets when done**, and **Cached frames**: **Use
cached frames**), **Quality** and, for image sequences, **Numbering** (**Start number**, or **Use
the frame numbers**, and **Digits**). **Quality** shows only what the chosen format has: **Profile**
where a format has several, **Compression** for OpenEXR, **Color space** for OpenEXR and DPX,
**Display** and **View** for movies, PNG and TIFF, **Bake in the look**, and **Include audio** for
movies. **Color space**, **Display** and **View** are lists of what the project's color setup
offers, with **Project default** first, so a name cannot be mistyped. The bit rate, encoder choice,
audio sample rate and file-name pattern an output already has are kept and used; the panel does not
edit them.

- **H.264** uses Cisco OpenH264 after your consent (Bloom downloads Cisco OpenH264 2.6.0 only after
  you accept Cisco's licence and verifies its digest; **Locate** verifies a download you already
  have) or VA-API hardware. No Cisco binary is bundled. Opening H.264 media never needs it.
- **HEVC** is hardware-only (VA-API). Without a VA-API render node that encodes HEVC, HEVC presets
  say **HEVC needs a VA-API render node with HEVC encode** and cannot start.
- **ProRes** is written by FFmpeg; it is not an Apple-authorized ProRes implementation.
- The **VFX handoff** and **Review H.264** deliverables need the built-in ACES CG configuration
  with a pinned revision.
- **DPX** sequences are listed but not available in this build; the preset says so.

MP4 movies carry AAC audio; MOV and MXF carry PCM. Movie containers are tagged from the output's
colour intent: an sRGB display is tagged with the sRGB transfer, a Rec.1886/Rec.709 display with
BT.709, P3 and Rec.2100 displays with their own code points.

## Destinations

**Output To** is relative to the project folder (moving a project with its renders keeps it
working) or an absolute file. The default, `Exports/{name}.<ext>`, follows the composition's name;
the panel always shows the name it resolves to (`Exports/SC020.mov`), never the token. A script may
also use `{job}` (job name), `{format}` (`mp4`, `prores`, `exr`, ...) and `{date}` (frozen when the
job starts), and a sequence's naming pattern `<base>`, `<ext>` and exactly one `####` or `{frame}`;
**Start number** changes file names, not which frames render. The Output Module line's summary shows
the first and last file before you start. Unknown tokens, a name that becomes a folder, or a changed
extension are refused.

Before Start, Bloom checks each destination in the background ("Checking where it saves…"): whether
its folder can be written (missing folders are created), how much space is free, and whether files
already exist. A size estimate appears when the format's size can be estimated honestly; when the
estimate does not fit the free space, Start is blocked.

## Replacing files

**If a file is there** is **Ask first** (the default), **Replace** or **Don't render**. When the
file exists under Ask first, the job reads **Waiting for your OK**, and **Start** or **Start All**
asks once: "Replace SC020.mov?" with **Replace**, **Choose another name** (the file chooser) and
**Cancel**. Several files are asked about in one question. The confirmation covers exactly the files that were checked; if they change before the
export publishes, Bloom refuses to replace them. Files this session itself exported for the output
need no confirmation on a re-run. Bloom never replaces a file silently.

## Running

**Start All** queues every checked job; **Start** in a row's menu queues that job alone. **Pause**
waits for the current frame to finish (**Resume** continues it); **Stop** returns a waiting job to
Ready, or stops a running one. A stopped movie or WAV leaves the destination unchanged; a stopped
sequence keeps the frames it already published. **Render again** starts a finished job again. The Jobs panel shows a running export as one row with **Show in Export**.

The Viewer always comes first. While you scrub, play or build a RAM preview, an export only uses
spare GPU capacity and may wait with no time left shown; it continues at full speed when the Viewer
is idle. That wait is shown only while the Viewer is actually rendering, and the job reads **Paused
while you work in the Viewer** in plain gray, not in a warning color. When a frame does not fit the
memory other active work holds (after Bloom's caches gave back theirs), the job reads **Waiting for
memory** with the amount needed. Waiting and paused time do not count toward the export's time
limits. Neither is a fault.

Quitting or closing the project while exports run asks "N exports are running. Stop them and
quit?"; **Keep Exporting** is the default. Saving the project during an export is allowed.

## Verification

The old approval prompt is gone. Before Start each output shows its preset's expected
preservation; each frame's analysis is accepted only when it fits that declaration. After
publication the job's **Details…** say in plain lines what was checked: **File opens**, **Right
number of frames**, **Colors tagged as** the colour space it was saved in, and **Rendered on the
GPU** or **Some frames rendered on the CPU**. A check that failed or warned (timing, container,
audio, file names, whether the saved file changed, whether it could be put in place) says so in a
sentence with what was expected and found. A frame that had to be evaluated on the CPU is never
shown as a GPU frame.
The **File opens** check decodes the movie and compares it with what Bloom rendered, after taking
the codec's own 4:2:2 or 4:2:0 colour sampling into account, so small saturated detail such as thin
coloured text passes while damaged pixels or wrong colour tags still fail the export.

**Add to Assets when done** adds the checked result to Assets (a movie as a video asset,
a sequence as a sequence, a still as an image, a WAV as audio) with one undoable import. A file
already in Assets is refreshed rather than duplicated. A failed import never fails the export; the
job's **Details…** say "Couldn't add to Assets" and why, on their own line.

## Scripting and the command line

`bloom-cli export --project shot.bloom --all` renders the saved queue headlessly and prints the
verification and import results as JSON; `--job <id>` selects jobs, `--replace` confirms existing
destinations, and `--save` writes asset imports back to the project. See
[headless scripting](scripting.md) and [MCP](mcp.md). **Copy as bloom-cli Command** and **Copy as Python** on a job's context
menu produce a script that recreates the job and starts it.

## Platforms

Export runs on Linux, macOS and Windows. Each system uses its own media framework where it is
present and falls back to Bloom's bundled FFmpeg otherwise, so every preset works everywhere and
a project prepared on one system exports unchanged on another. The
[media I/O contract](../architecture/media-io.md#time-based-export-media-4) describes preservation
bounds.
