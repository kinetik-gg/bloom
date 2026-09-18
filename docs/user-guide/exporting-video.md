# Exporting video

Choose **File → Export Composition…**, select a preset and profile, then set the inclusive first
and last frame and destination. The range uses the composition's frame rate. **Continue** opens
the preservation report; **Export** approves the displayed settings and digest.

- **ProRes MOV (preview)** offers 422 Proxy, LT, 422, HQ, 4444 and 4444 XQ. Only the 4444 profiles
  retain alpha. The dialog states: "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes
  implementation". These files are preview-workflow exports, without Apple authorization or
  strict delivery qualification.
- **DNxHR MXF** offers LB, SQ, HQ, HQX and 444. Codec and container restrictions are checked by
  the worker; an unsupported resolution, cadence or audio rate produces an export error.
- **PCM WAV / BWF** writes 16-bit or 24-bit PCM. The host API can supply a BWF description;
  ordinary dialog exports write WAV. Large WAV files use RF64 automatically.
- **TIFF, PNG and OpenEXR sequences** write a numbered file per frame. TIFF uses straight-alpha
  16-bit sRGB; PNG uses 8-bit sRGB; OpenEXR keeps the scene-linear float image.
- **H.264 MOV (review)** uses Cisco OpenH264 2.6.0 at a 30 Mbps default target, H.264 High,
  8-bit 4:2:0, one-second GOP and Rec.709 limited range. It is a review deliverable — not for
  archival. If the encoder is absent, choose **Install OpenH264…** and accept Cisco's binary
  licence; Bloom downloads only from Cisco, verifies the archive and library digest, and records
  `media/openh264-consent=2.6.0` in `Bloom.conf`. **Locate downloaded file…** performs the same
  digest check. No binary is bundled. When available, **Use hardware (VA-API)** selects the
  hardware alternative and reports its driver in QC evidence.

Opening H.264 and ProRes media never requires the Cisco binary. Bloom always starts the isolated
worker with its private loader shim for decode and fixture generation; only H.264 software encode
needs the verified Cisco library. If neither software nor VA-API encode is available, the dialog
keeps the typed reason **H.264 encoder not installed**.

Video presets can include the composition audio mix. The dialog shows the selected source sample
rate; mixed-rate sources pass through the timeline mixer's normal resampling. WAV always includes
audio. H.265 software export remains unavailable.

The status bar reports progress and offers **Cancel**. Movies and WAV files are published only
after the worker closes and reopens them, verifies their pixels/audio/timing, and Bloom verifies
the staged file digest. Cancel or a worker failure leaves the destination unchanged. For image
sequences, complete frames already published remain on disk.

These worker exports currently run on Linux. On macOS and Windows, missing process providers
appear unavailable with a reason. PNG and OpenEXR keep their existing supported paths. The
[media I/O contract](../architecture/media-io.md#time-based-export-media-4) describes the precise
preservation bounds and pending platform qualification.
