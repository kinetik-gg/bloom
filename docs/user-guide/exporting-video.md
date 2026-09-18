# Export

Choose **File → Export Composition…**. The two deliverables at the top of the preset list produce
an effects handoff and a review movie from the same composition. First and last frame select an
inclusive composition range; the composition's frame rate sets the timing.

## VFX handoff

1. Use the ACES 1.3 CG config with ACEScg as the project or composition working space.
2. Select **VFX handoff EXR (ACES2065-1, PIZ, 1001, ####)**.
3. Set the range and a destination such as `shot.exr`. The preset fills **Output colour space**
   with `ACES2065-1`, **Compression** with **PIZ**, **Start frame** with **1001**, **Padding**
   with **4**, and **Pattern** with `<base>.####.<ext>`. You can edit these fields.
4. Leave **Bake look-tagged effects** off. **Continue** opens the preservation report showing
   **Look: OFF (handoff)** and **ACES2065-1; PIZ**. Check these before choosing **Export**.
5. A 48-frame range produces `shot.1001.exr` through `shot.1048.exr`, containing premultiplied
   32-bit float AP0 pixels. The look bypass applies only to this export.

Start frame changes filenames, not which composition frames are rendered. Pattern accepts
`<base>` for the destination basename, `<ext>` for its extension, `{name}` for the composition
name, and exactly one `####` or `{frame}` for the padded number. Bloom rejects names that collide,
fail to ascend, contain path separators or change the preset extension.

## Review movie

1. With the same ACES composition, select **Review H.264 (Rec.709, look on)**.
2. Choose the range, a `.mov` destination and whether to include audio. The preset uses
   **Rec.1886 Rec.709 - Display / ACES 1.0 - SDR Video** with the look on.
3. If needed, choose **Install OpenH264…** and accept Cisco's binary licence. Bloom downloads
   Cisco OpenH264 2.6.0 only after consent and verifies its digest. **Locate downloaded file…**
   verifies an existing download. When available, **Use hardware (VA-API)** selects the hardware
   alternative. No Cisco binary is bundled.
4. Choose **Continue**. Check the display/view pair and `look: baked (N look-tagged effects)` in
   the approval report, then choose **Export**.

The review movie uses H.264 High, a 30 Mbps default target, 8-bit 4:2:0, a one-second GOP and
Rec.709 limited-range tags. Look-tagged effects and the display transform are baked into the
movie. It is a review deliverable, not an archival master. The Viewer look switch does not change
either deliverable's export policy.

## Other presets

- **ProRes MOV (preview)** offers 422 Proxy, LT, 422, HQ, 4444 and 4444 XQ. Only the 4444 profiles
  retain alpha. The dialog states: "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes
  implementation". These files have no Apple authorization or strict delivery qualification.
- **DNxHR MXF** offers LB, SQ, HQ, HQX and 444. Unsupported dimensions, cadence or audio rates
  produce an export error.
- **PCM WAV / BWF** writes 16-bit or 24-bit PCM. The host API can supply a BWF description;
  ordinary dialog exports write WAV. Large WAV files use RF64 automatically.
- **TIFF and PNG sequences** bake the config's default display/view into straight-alpha 16-bit
  and 8-bit samples, respectively. **OpenEXR sequence** defaults to unchanged working-space
  float pixels and ZIP; its output colour space and compression remain editable.
- **H.264 MOV (review)** uses the raw preset with the config's default display/view. Select the
  named review deliverable above it for the explicit ACES Rec.709 display/view pair.

Opening H.264 and ProRes media never requires the Cisco binary. Bloom always starts the isolated
worker with its private loader shim for decode and fixture generation; only H.264 software encode
needs the verified Cisco library. If neither software nor VA-API encode is available, the dialog
keeps the typed reason **H.264 encoder not installed**.

Video presets can include the composition audio mix. The dialog shows the selected source sample
rate; mixed-rate sources use the timeline mixer's resampling. WAV always includes audio.
H.265 software export remains unavailable.

The status bar reports progress and offers **Cancel**. Movies and WAV files publish only after
worker reopen verification and the staged-file digest check. Cancellation or worker failure
leaves the destination unchanged. For image sequences, complete frames already published remain.

Worker exports currently run on Linux. On macOS and Windows, unavailable process providers show
a reason; PNG and OpenEXR retain their supported paths. External File Transform LUT evaluation
also requires the supported Linux helper. The [media I/O contract](../architecture/media-io.md#time-based-export-media-4)
describes preservation bounds and platform fallbacks.
