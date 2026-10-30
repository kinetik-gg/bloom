# Export

Exports run from the **Export** panel, a queue saved in the project, much like the After Effects
Render Queue. Open the panel from **Window → Export**, or choose **File → Add to Export Queue**
(Ctrl+M). The same **Add to Export Queue** item is on a composition row in Assets, and you can drag
a composition from Assets onto the list. Bloom adds a job for the composition, reveals the panel
and selects the new job. A job renders one composition range to one or more outputs, one job at a
time in queue order, in the background while you keep working.

Each job is a row: label colour, an enabled check (the job takes part in **Start All**), name,
composition, range and status. Its outputs are child rows with the preset and the resolved
destination, shortened in the middle with the whole path as the tooltip. Drag a job to reorder it.
The running job is shown in a strip above the list with its progress, the frame, the elapsed time
and, once there is a reliable estimate, the time left. Every command shows why it cannot run: hover
a dimmed button or menu item to read the reason.

Select a job or an output to edit it in the detail pane, which has four sections: **Render** (name,
range, enabled, label colour and the job's own Start, Pause, Stop and Retry), **Outputs** (preset,
settings, destination, overwrite policy, **Add output** and **Remove output**; a job keeps at least
one), **Post-render** and **Log and verification**. The size shown for an output is only what Bloom
can estimate; nothing is shown when it cannot. A failure reads as a sentence that says what to do;
**Details** opens the technical text and **Copy details** copies it for a bug report.

## Jobs and outputs

A new job renders the composition's work area (or the whole composition when it has none) to one
**H.264 MP4** output at `Exports/{name}.mp4` beside the project. If H.264 cannot run on this
computer, the output starts as **ProRes 422 HQ MOV** and the panel says why H.264 was not used. The
default is never an image sequence and never the last preset you used. An unsaved project has no
default destination; Start then says "Choose where to save this output".

Each job has a name, a range (**Work area**, **Composition**, **Custom** first and last frame, or a
single **Frame**), an enabled flag for **Start All** and an optional label colour. Each output has
a preset, its settings, a destination with an overwrite policy, an enabled flag and **Import into
Assets after render**. Every edit is undoable and saved in the project. Run status, progress, ETA,
logs and verification are not saved: a reopened project shows every job **Ready**. A running job
cannot be edited ("Stop the job to change it; it keeps the settings it started with").

An image output writes one file per frame, or a single still when the range is one frame.

## Presets

Presets are listed in four groups:

- **Video**: H.264 MP4 (default), H.264 MOV, Review H.264 (Rec.709, look on), HEVC MP4 and MOV
  (hardware), ProRes 422 Proxy, LT, 422, HQ, 4444 and 4444 XQ MOV, DNxHR HQ MXF.
- **VFX / sequences**: VFX handoff EXR (ACES2065-1, PIZ, from 1001, `####`), OpenEXR, DPX, PNG
  and TIFF sequences.
- **Audio**: PCM WAV/BWF (RF64 when large).
- **User**: your own presets, saved per computer outside any project.

Applying a preset copies its settings into the output, so a project exports the same way on a
computer that lacks the user preset it came from. A preset that cannot run here stays listed with
its reason and, where one exists, a remedy (**Install OpenH264**, **Locate OpenH264**, **Open
Project Settings**, **Choose Software Encoder**).

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

A destination is relative to the project folder (moving a project with its renders keeps it
working) or an absolute file. It may use `{name}` (composition name), `{job}` (job name), `{format}`
(`mp4`, `prores`, `exr`, ...) and `{date}` (frozen when the job starts). A sequence's naming
pattern also accepts `<base>`, `<ext>` and exactly one `####` or `{frame}`; **Start frame** changes
file names, not which frames render. The panel shows the first and last file before you start.
Unknown tokens, a name that becomes a folder, or a changed extension are refused.

Before Start, Bloom checks each destination in the background ("Checking destination…"): whether
its folder can be written (missing folders are created), how much space is free, and whether files
already exist. A size estimate appears when the format's size can be estimated honestly; when the
estimate does not fit the free space, Start is blocked.

## Replacing files

Each output's overwrite policy is **Ask before replacing** (the default), **Replace** or
**Refuse**. When the destination exists, Ask blocks Start until you confirm replacing it for that
output. The confirmation covers exactly the files that were checked; if they change before the
export publishes, Bloom refuses to replace them. Files this session itself exported for the output
need no confirmation on a re-run. Bloom never replaces a file silently.

## Running

**Start** and **Start All** queue jobs. **Pause** waits for the current frame to finish; **Stop**
returns a waiting job to Ready, or stops a running one. A stopped movie or WAV leaves the destination
unchanged; a stopped sequence keeps the frames it already published. **Retry** starts a finished
job again. The Jobs panel shows a running export as one row with **Show in Export**.

The Viewer always comes first. While you scrub, play or build a RAM preview, an export only uses
spare GPU capacity and may wait in **Waiting for Viewer** with no ETA; it continues at full speed
when the Viewer is idle. Waiting and paused time do not count toward the export's time limits. This is not a fault, and
the panel reads **Waiting for the Viewer** rather than showing a time left.

Quitting or closing the project while exports run asks "N exports are running. Stop them and
quit?"; **Keep Exporting** is the default. Saving the project during an export is allowed.

## Verification

The old approval prompt is gone. Before Start each output shows its preset's expected
preservation; each frame's analysis is accepted only when it fits that declaration. After
publication each output shows its checks: reopen, frame count, timing, colour tags, container,
audio, naming, artifact digest, publication and device provenance. A frame that had to be
evaluated on the CPU reference path is a device-provenance warning, never shown as a GPU frame.

**Import into Assets after render** adds the verified result to Assets (a movie as a video asset,
a sequence as a sequence, a still as an image, a WAV as audio) with one undoable import. A file
already in Assets is refreshed rather than duplicated. A failed import never fails the export; the
panel reports it on the output.

## Scripting and the command line

`bloom-cli export --project shot.bloom --all` renders the saved queue headlessly and prints the
verification and import results as JSON; `--job <id>` selects jobs, `--replace` confirms existing
destinations, and `--save` writes asset imports back to the project. See
[headless scripting](scripting.md) and [MCP](mcp.md). **Copy as bloom-cli Command** and **Copy as Python** on a job's context
menu produce a script that recreates the job and starts it.

## Platforms

Export runs on Linux in this version. On macOS and Windows the queue, presets, saving and script
edits work the same, and **Start**, **Start All**, **Retry** and `bloom-cli export` say "Export runs
on Linux in this version". A project prepared there exports unchanged on Linux. The
[media I/O contract](../architecture/media-io.md#time-based-export-media-4) describes preservation
bounds.
