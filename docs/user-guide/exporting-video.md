# Export

Exports run from the **Export** panel, a queue saved in the project, much like the After Effects
Render Queue. Open the panel from **Window → Export**, or choose **File → Add to Export Queue**
(Ctrl+M). The same **Add to Export Queue** item is on a composition row in Assets, and you can drag
a composition from Assets onto the list. Bloom adds a job for the composition, reveals the panel
and selects the new job. A job renders one composition range to one or more outputs, one job at a
time in queue order, in the background while you keep working.

The panel is one list of jobs. Each job is a row: a caret, a label colour, the **Include in Start
All** switch, name, composition, range, status and one button that fits what the job is doing
(**Start**, **Pause**, **Resume**, **Stop** or **Retry**), with a menu for the rest (Duplicate,
Move up and down, Remove, Copy as bloom-cli Command and Copy as Python). While a job runs, its row
shows a progress bar with the frame count and the time left once there is a reliable estimate.
Click the caret, or double-click the row, to open a job and see its settings under it. Double-click
a name, or press F2, to rename it. Every button that cannot act says why in a line in the job (and
in its tooltip), rather than only dimming.

An open job has a **Render** group (range) and one section for each output. An output shows its
format and where it saves on one line; open it for **Format**, **Save to**, **If a file is there**,
**Quality** and **After render** (**Add to Assets when done**). **Add output** renders the job in
another format too; a job keeps at least one output. **Start All** appears, as the one blue button,
only while a job is ready, and **Stop All** only while something runs. When a job has run, its
**Result** says in one sentence what was saved and whether it was checked, and **Show in folder**
opens the file. A failure reads as a sentence that says what to do; **Show details** opens the
technical text and **Copy details** copies it for a bug report. The size shown for an output is
only what Bloom can estimate; nothing is shown when it cannot.

## Jobs and outputs

A new job renders the composition's work area (or the whole composition when it has none) to one
**H.264 MP4** output at `Exports/{name}.mp4` beside the project. If H.264 cannot run on this
computer, the output starts as **ProRes 422 HQ MOV** and the panel says why H.264 was not used. The
default is never an image sequence and never the last preset you used. An unsaved project has no
default destination; Start then says "Choose where to save this output".

Each job has a name, a range (**Work area**, **Whole composition**, **Custom frames** with a first
and last frame, or **One frame**), the **Include in Start All** switch and an optional label colour.
Each output has a format, its quality settings, a place to save with a choice for what to do when a
file is already there, a switch for whether it renders and **Add to Assets when done**. Every edit is undoable and saved in the project. Run status, progress, ETA,
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
computer that lacks the user preset it came from. A format that cannot run here stays listed, with
its reason on a line under the dropdown and, where one exists, a button to fix it (**Install
H.264 encoder…**, **Locate encoder…**, **Project settings…**, **Use software encoder**).

**Quality** shows only the settings the chosen format has: **Include audio** and **Sample rate**
for movies, **Quality target** and **Use** (Automatic, Software or GPU) for H.264 and HEVC,
**Compression** for OpenEXR, **File numbering** and **File names** for image sequences, **Include
the look** and **Reuse cached frames** for anything with pictures. **Colour space**, **Display**
and **View** are lists of what the project's colour setup offers, with **Project default** first,
so a name cannot be mistyped.

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

**Save to** is relative to the project folder (moving a project with its renders keeps it
working) or an absolute file. It may use `{name}` (composition name), `{job}` (job name), `{format}`
(`mp4`, `prores`, `exr`, ...) and `{date}` (frozen when the job starts). A sequence's naming
pattern also accepts `<base>`, `<ext>` and exactly one `####` or `{frame}`; **Start frame** changes
file names, not which frames render. The panel shows the first and last file under the field before you start, and a small **?** next
to it lists the name tokens.
Unknown tokens, a name that becomes a folder, or a changed extension are refused.

Before Start, Bloom checks each destination in the background ("Checking where it saves…"): whether
its folder can be written (missing folders are created), how much space is free, and whether files
already exist. A size estimate appears when the format's size can be estimated honestly; when the
estimate does not fit the free space, Start is blocked.

## Replacing files

**If a file is there** is **Ask me first** (the default), **Replace it** or **Don't render**. When
the file exists, Ask blocks Start until you answer a question inside that output ("A file called
Shot 010.mp4 is already in Exports.") with **Replace it** or **Choose another name**; the job opens
itself and shows **Waiting for your OK** until you do. **Start All** starts the jobs that can run
and says in the header how many wait for your OK; it never opens a second dialog. The confirmation covers exactly the files that were checked; if they change before the
export publishes, Bloom refuses to replace them. Files this session itself exported for the output
need no confirmation on a re-run. Bloom never replaces a file silently.

## Running

**Start** and **Start All** queue jobs. **Pause** waits for the current frame to finish (the job button for a running job; **Resume**
continues it); **Stop** (in the job's menu, or in the button for a job that has not started
rendering) returns a waiting job to Ready, or stops a running one. A stopped movie or WAV leaves the destination
unchanged; a stopped sequence keeps the frames it already published. **Retry** starts a finished
job again. The Jobs panel shows a running export as one row with **Show in Export**.

The Viewer always comes first. While you scrub, play or build a RAM preview, an export only uses
spare GPU capacity and may wait with no time left shown; it continues at full speed when the Viewer is idle. Waiting and
paused time do not count toward the export's time limits. This is not a fault, and the job reads
**Paused while you work in the Viewer** in plain grey, not in a warning colour.

Quitting or closing the project while exports run asks "N exports are running. Stop them and
quit?"; **Keep Exporting** is the default. Saving the project during an export is allowed.

## Verification

The old approval prompt is gone. Before Start each output shows its preset's expected
preservation; each frame's analysis is accepted only when it fits that declaration. After
publication the job's **Result** says in plain lines what was checked: **File opens**, **Right
number of frames**, **Colours tagged as** the colour space it was saved in, and **Rendered on the
GPU** or **Some frames rendered on the CPU**. A check that failed or warned (timing, container,
audio, file names, whether the saved file changed, whether it could be put in place) says so in a
sentence with what was expected and found. A frame that had to be evaluated on the CPU is never
shown as a GPU frame.

**Add to Assets when done** adds the checked result to Assets (a movie as a video asset,
a sequence as a sequence, a still as an image, a WAV as audio) with one undoable import. A file
already in Assets is refreshed rather than duplicated. A failed import never fails the export; the
panel says "Couldn't add to Assets" and why, on its own line.

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
