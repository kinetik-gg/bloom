# Professional Media I/O And Codec Providers

Status: working research

Updated: 2026-10-31

## v0 implemented boundary

The accepted v0 amendment in ADR 0020 admits the image pipeline under `src/media/image`:
a private, pinned stb_image PNG/JPEG decoder and a bounded OpenEXR reader compiled in process with
hard input and allocation limits. PNG16 is included. Images and numbered sequences become asset
records; authoring source nodes reference stable asset IDs. Linux video preview read is implemented
by MEDIA-3, described in [Video Read Implementation](#video-read-implementation-media-3). Other
delivery paths remain working research below. Linux export is described under
[Time-based Export](#time-based-export-media-4).
The v0 gap rule is hold-previous with a visible warning; it supersedes the policy-selection
research below for this closed image profile. See the component security review for limits.

The implemented APIs are `probeImage`, `decodeImage`, `encodeImage`, and `scanSequence`. Selection
admits PNG (including 16-bit samples), JPEG, and the bounded EXR subset. The scanner recognizes a final fixed-width number in
any name whose stem ends in, or contains, a run of digits: the last digit run is the frame number, everything before it is the prefix and everything after it the suffix, and both must match for members to belong together (`shot.0001.png`, `shot_0001.png`, `shot0001.png`, `0001.png`, `shot0001_left.png`); padding may differ between members and is reported once; two or more matching members create one
sequence asset. Different padding is diagnosed rather than merged. A manifest records explicit
members, SHA-256 digests, first/last frame and gaps. Hold repeats the nearest endpoint; Loop and
PingPong wrap after the end, while time before the node start always holds the first member.
A missing or changed selected member produces transparent pixels and a warning; it is not a gap.

### OpenEXR image boundary

EXR intake is deliberately narrower than the format. The in-process reader accepts version 2
single-part and multipart files whose parts are scanline or single-level tiled, with any number of
half, float or uint channels, within the image limits above. It honors each part's data window,
retains the display window offset in the Bloom image descriptor, defaults alpha to
associated/premultiplied, and maps each part's optional chromaticities attribute to the linear color
tag (or records the absent-metadata scene-linear assumption in the probe). Deep, multi-level tiled,
subsampled, malformed, and truncated files return typed diagnostics and never become project truth.
EXR decode runs on a worker and uses the same disk-cache key and thumbnail path as PNG/JPEG.

**Multi-channel images (ADR 0026, #363).**

- *Probe.* `ImageProbe::parts` lists every part: name, view, scanline or tiled, data and display
  windows, alpha association, color tag, channels with their sample kind, the distinct layers
  (everything before a channel name's last `.`; the root layer is empty) and the layers that carry
  R/G/B or Y color channels. PNG and JPEG probe as one unnamed part whose channels are R, G, B and,
  when the file carries alpha, A. The asset record persists no part list.
- *Color selection.* `ImageInterpretation::part` (empty: the first part) and `layer` (empty: the
  root layer) choose the color image: the layer's `R/G/B[/A]` (or `Y[/A]`) channels go through the
  unchanged color path, while every other channel of the part is ignored by it. A missing part or
  layer is a typed `PartNotFound` or `LayerNotFound`; a layer without color channels
  (`NoColourChannels`) gives no color image. The decoded-image identity is `bloom-image-decode-4`,
  and a non-default part or layer is part of the memory, raw-upload and disk keys.
- *Channel sets.* `decodeImageChannels()` returns a `render::ChannelSetImage`: the named channels of
  one part (all, or a selection in order) as planar Float32 over the shared data window, with the
  part's string attributes (Cryptomatte manifests) as metadata and the asset's input color space
  recorded for the Color channels. Values are source values: half widens exactly, uint keeps its
  bit pattern, and nothing is converted, premultiplied, repaired or clamped, so negatives, NaN and
  ID hashes survive. Admission charges every plane plus one bounded band and the metadata. Stills
  expose their decoded R, G, B (and A) as straight Color channels in [0, 1]; formats without an
  immutable revision decoder report `UnsupportedChannelSetFormat`.
- *Cryptomatte.* A Cryptomatte type is read from a part's channels and string metadata only: the
  `<layer>NN` rank channels keep their Float32 bit patterns through `decodeImageChannels()`, and
  `cryptomatte/<key>/name`, `hash`, `conversion` and `manifest` arrive as metadata, where `<key>` is
  any metadata key whose `name` is the layer. Half rank channels widen exactly, so IDs survive only
  when the renderer wrote them as Float32, as the specification requires. Only the rank channels of
  the selected layer are decoded (#364). A `<type>NN` layer named by a `cryptomatte/<key>/name`
  attribute is a rank layer: the probe never lists it among a part's color layers, so a
  Cryptomatte-only sidecar EXR has no color layer and is a data-only asset.
- *Asset state in Assets.* "Missing — Replace Footage" means only that the imported bytes are gone or
  changed. A present source that cannot be shown as it is (for example a Rec.2020 video awaiting an input
  color space) states that problem and its fix instead, and its audio still decodes. An untagged
  video is shown under the assumed Rec.709 or Rec.601 matrix with the sRGB transfer, with an Info
  mark and its color note. A
  data-only image is valid: it is labelled like any image or sequence (a Sequence of N frames) and its
  thumbnail is the Cryptomatte ID colors of its first type, or its first data channel as gray. The
  Image source's Part and Layer pickers read a sequence's parts from its first member.
- *Sequences.* An unreadable member never aborts a scan: it is reported (up to 32 frames by name,
  then a count) and its frame becomes a gap that holds the preceding frame, as for the #373 batch
  import policy.

**OpenEXR ingest correctness (#362).**

- *Alpha association.* A new image layer or node-canvas image source inherits the asset's probed
  association: `premultiply` is on only for straight-alpha assets, so a premultiplied EXR is never
  multiplied by alpha twice. Existing layers keep their authored value.
- *Windows.* The probe records the frame (display window) and the data window's offset inside it.
  CPU evaluation and GPU preparation publish the data window at that offset, scaled by the shared
  PointResampleV1 window rule (`render::pointResampleProxyWindow`), and an image source whose data
  differs from its frame uses the frame as its local content bounds. Crops, overscan and passes of
  one shot therefore register under the default layer transform; ordinary images are unchanged.
- *Non-finite and out-of-range pixels.* Decode never fails a frame for them. A NaN or infinite
  component becomes 0, alpha is clamped to [0, 1], and a component that overflows on
  premultiplication or color conversion is repaired the same way. The v1 process image cannot carry
  premultiplied RGB at zero alpha, so additive "emission" pixels become transparent. Every changed
  pixel is counted: the CPU evaluator reports a warning naming the count and GPU preparation records
  `imageRepairedPixels`. The count is reported when the source is decoded; a decoded-cache hit
  returns the same repaired pixels without repeating it. Preserving zero-alpha emission would change
  the process image contract for every operation and needs its own ADR.

TIFF is content-probed by classic and BigTIFF magic, but its read and write operations are provider
operations. `ImageProvider` carries explicit Decode and Encode callbacks matching the worker image
product; no global registry is consulted. MEDIA-4 enables `TiffRgba16SrgbV1` export through the isolated FFmpeg worker on Linux.
It writes RGBA16 little-endian, with the config display/view baked into straight RGB, and verifies every decoded sample before
publication. The general `ImageProvider` read callback remains a separate intake seam.
Platforms without the process backend expose an unavailable preset with its reason.

| Bound | v0 value |
| --- | --- |
| Encoded file | streamed in bounded chunks; no whole-file cap |
| Either dimension | codec-representable window (stb per-axis `STBI_MAX_DIMENSIONS` = 2^24); no fixed image ceiling |
| Pixel count | codec sample-count limits; no fixed image ceiling |
| Decoder allocator live bytes | thread-local, scoped to the request pixel budget (default 256 MiB) |
| Float image storage | admitted against the request pixel budget (default 256 MiB), including bounded decode scratch |
| Sequence matching members | 100,000; unrelated directory entries and frame-number span have no fixed cap |
| Decoded image LRU | 1 GiB per evaluator cache |
| UI thumbnail cache | 512 RGBA8 images, at most 64 × 64 (8 MiB) |

These are separate bounds, not an aggregate process-memory guarantee. Cancellation is checked
between file chunks, before and after stb, and during conversion/proxy rows; an in-progress stb
call is cooperatively cancellable only after it returns. Import and relink preparation and
thumbnail work use the blocking-I/O task executor, with Jobs progress, diagnostics and Cancel.
Image and sequence asset metadata retains positive, representable source dimensions without a
second fixed 16,384-axis or 16,777,216-pixel limit. Actual decode and proxy work still enter the
caller's checked pixel budget before allocation. Video assets follow the media provider protocol's
extent and frame-index limits (issue #352: 16,384 per axis, 67,108,864 pixels, 16,777,216 frames).
Publication checks the originating project/revision and discards work after project replacement.
Sequence scanning checks cancellation while enumerating files and hashes only matching image
members. Missing intervals in sparse spans are derived from adjacent sorted member numbers, so
large numeric holes do not allocate one record per absent frame. Spans under 100,000 retain their
legacy enumerated `gaps` encoding for compatibility with older project readers. Older binaries
cannot open a newly admitted wider-span sequence, which their own validation already rejected.
The evaluator decodes only on worker execution; UI paint reads cached thumbnails.

The same C++ decoder, filesystem paths and Qt dialogs/drop events serve Linux, macOS and Windows;
the file dialog is always Bloom's own (Qt's widget dialog styled by the kit), never a native one.
The project-relative locator is preferred. When roots differ or a relative component cannot be
stored portably, import records an explicit absolute file-URI locator. Ordinary relative locators
first resolve beside the project, then use their absolute file-URI hint; external locators resolve
directly. This also covers the first Save As of an unsaved project. Content-digest verification
still applies. Relink records a fresh relative locator when the media moves into the project tree.
Media is referenced, never embedded or silently copied.

This slice does not add movie containers, EXIF orientation, ICC interpretation, metadata
round-tripping, external codec processes, or a general proxy service. Files are interpreted by
the closed sRGB/override rule in `color-management.md`, not by embedded profiles. The broader
research below does not supersede this implemented boundary.

## Media provider chain (ADR 0035, 2026-10-31)

Each operating system orders its media providers; the bundled FFmpeg worker is the last provider
everywhere, so no format FFmpeg serves is ever refused for the platform:

| Platform | Chain |
| --- | --- |
| Windows | Media Foundation (slot; no provider yet, reports NotInstalled) → FFmpeg |
| macOS | VideoToolbox/AVFoundation (`bloom-media-videotoolbox-worker`) → FFmpeg |
| Linux | FFmpeg with VA-API (when hardware is requested) → FFmpeg software |

`MediaProviderChainV1` (`provider_chain.hpp`) selects per operation -- decode or encode of one codec
family, plus the container for encode -- using each provider's handshake declarations. A provider
that is not installed, does not declare the operation, or fails it with a typed error other than
Cancelled, Shutdown or SourceChanged is skipped silently. Encode selection happens in
`EncodeSessionV1::begin()`: each candidate is a full launch/handshake/`EncodeBegin` attempt whose
refusal reaps the worker before any frame is sent; the serving provider is then pinned. Decode asks
the platform provider first and retires it for the source on its first failure, after which FFmpeg
answers (the same sticky rule as the VA-API preview decoder). `MediaProviderProvenanceV1` records
the serving provider and every fallback in `SequenceExportResultV1::mediaProvider`, in the export QC
evidence (tool, version, pipeline and the ProRes implementation note follow the serving provider)
and in `FrameDecodeProvenance`; it is diagnostics, never artist text. `PlatformNotSupported` is no
longer produced.

Worker generations sit beside the application: `libexec/bloom/media/<generation>/` on Linux,
`Bloom.app/Contents/libexec/bloom/media/<generation>/` on macOS, and
`<Bloom.exe dir>\media\<generation>\` on Windows, located from the executable the operating
system reports, with the build-tree worker as the development fallback. A macOS or Windows
developer build without an FFmpeg package builds no FFmpeg worker; Linux and qualified builds
require it.

Not done: a Media Foundation provider (Windows therefore has no H.264/HEVC encoder until a bundled
software encoder lands), the Windows staged-artifact publication provider, mid-stream encode
fallback, and VideoToolbox HEVC encode. The Windows and macOS branches are not compiled or run in
Linux validation.

## FFmpeg dependency intake (2026-09-17)

FFmpeg 8.1.2 was selected from the maintained 8.1 release line for this intake, first for Linux
and, since ADR 0035, for macOS and Windows too. MEDIA-3 and MEDIA-4 consume its shared libraries
exclusively in `bloom-media-worker`;
the desktop `bloom` target must not link or load `libav*`. The dependency review and worker
implementation are separate qualification boundaries.

The lock records the official `ffmpeg-8.1.2.tar.xz` archive, detached-signature evidence, exact
configure arguments, and a corresponding-source obligation. The recipe is shared-only,
library-only, LGPL-2.1-or-later, disables GPL/nonfree/version-3/network/autodetection, and has no
external codec or crypto libraries. Its closed preview allow-list covers MOV, Matroska, MXF, MPEG-TS,
WAV, MP3, and AAC demuxing; MOV, Matroska, MXF, and WAV muxing; H.264/HEVC, ProRes, DNxHD, MJPEG,
TIFF, AAC, MP3, and explicit PCM-family decoding; ProRes KS, DNxHD, PCM-family, AAC, and TIFF
encoding; the required parsers/bitstream filters; the libavfilter `buffer`, `buffersink` and
`bwdif` filters for deinterlacing; and the `file` protocol. VA-API H.264/HEVC
hardware acceleration is enabled when `libva` is available and can be disabled for a host without
the development files. On x86 and x86-64 hosts FFmpeg's hand-written SIMD is enabled: the superbuild
assembles it with NASM 2.16.03, a source-built, build-host-only tool installed outside the
dependency prefix that is never linked or shipped.

FFmpeg ProRes is a non-authorized preview/workflow capability only. It is not evidence of Apple
authorization, certification, patent clearance, or delivery qualification. The worker resolves its
own bundled shared libraries privately (Linux `$ORIGIN/lib`, macOS `@loader_path/lib` with
`@rpath` install names, Windows DLLs beside the worker); the desktop binary receives no FFmpeg
search path. VA-API is Linux-only; the macOS and Windows FFmpeg builds omit the Linux x86-64
OpenH264 link-time stub, and Windows builds use FFmpeg's MSVC toolchain from an MSYS2 shell. Source, license, security, and review
records remain under `dependencies/licenses/ffmpeg/` and the lock is the authority for the exact
configuration.

## Purpose

Bloom needs reliable, performant media ingest and export without making one codec library, operating
system framework, GPU vendor, or commercial SDK part of project semantics. Professional media is a
pipeline: a successful decoder call is not proof of frame-accurate ingest, and a successful encoder
call is not proof that a deliverable satisfies its requested profile.

This document establishes the architecture to research and qualify that pipeline. It does not place
broad video delivery inside the first compositing proof. PNG and flat OpenEXR frame output remain
owned by [`frame-output.md`](frame-output.md); this contract extends the same analysis, verification,
task, and publication principles to time-based media.

## Inherited Constraints

- Container, codec, profile, pixel format, color interpretation, audio layout, and delivery preset
  are distinct contracts.
- Media location and interpretation are project truth. Probes, indexes, decoded frames, waveforms,
  and proxies are derived runtime state.
- Probe, scan, demux, decode, encode, mux, proxy, hashing, verification, and QC never run on the UI
  thread.
- Bloom owns provider-neutral time, color, alpha, channel, metadata, and frame semantics. A
  third-party library type never becomes the document or render model.
- Every provider is selected by an exact, immutable capability report. Availability on one machine
  or successful initialization does not qualify a codec/profile tuple.
- Bounded CPU decode products and Bloom-owned conversions establish portable semantic references.
  Software and hardware codecs are independently qualified providers; neither may silently change
  final-output intent.
- Export uses analysis, explicit approval where needed, private staging, close/reopen verification,
  QC, and atomic publication. The final target is never handed directly to an encoder.
- Linux, macOS, and Windows expose the same project and task semantics. A platform without a
  qualified provider reports the exact missing capability and offers only an explicitly different
  fallback.

## Separate Claims

Bloom must never collapse these claims into one “supported” badge:

| Claim | Evidence required |
| --- | --- |
| Container readable | bounded probe and demux fixtures for the declared container/version subset |
| Codec decodable | exact codec/profile/pixel-format fixtures and malformed-input containment |
| Frame-accurate ingest | sequential and random-access decode agree on presentation time, frame identity, field handling, color, alpha, and audio synchronization |
| Editorial-preview qualified | bounded latency and visible degradation under a declared proxy/preview profile |
| Conform-ingest qualified | full-resolution decode, metadata preservation, deterministic time mapping, and independent fixture evidence |
| Codec encodable | the provider can produce a declared elementary stream tuple |
| Container writable | the muxer emits the closed track, timing, metadata, and index profile |
| Bloom export qualified | staged artifact reopens and passes semantic, structural, metadata, and resource checks |
| Delivery-profile qualified | the exact preset passes the named recipient/broadcaster/studio specification and QC suite |
| Vendor authorized or certified | dated evidence from the named authority for the exact Bloom product, provider, direction, platforms, and workflow |

Passing a lower row never implies a higher row. “Certified” and “authorized” are attributed claims,
not Bloom-defined quality adjectives.

## Ownership And Dependency Direction

```text
Document AssetRegistry and interpretation overrides
                         |
                         | immutable asset request
                         v
                 src/media services
        probe / index / decode / proxy / audio
              |                       |
              | provider-neutral      | task requests
              v                       v
      Media descriptors          Task system
      and decoded products            |
              |                       v
              |              qualified media provider
              |              (usually isolated worker)
              v
       render/color intake <---- CPU planes or runtime-only surface lease

Immutable render snapshot -> render/color/output preparation
                         -> preservation analysis and approved preset
                         -> qualified encoder + muxer worker
                         -> private staged artifact
                         -> reopen + structural/semantic/QC verification
                         -> StagedArtifactCoordinator -> final target
```

On Linux the private staged artifact is the publication target's own staged file
([ADR 0033](../decisions/0033-export-queue.md), section 16): the host stages the target before the
first frame, the supervised worker inherits a read-write descriptor of that file as fd 5
(`ProcessOptions::stagedOutput`, worker flag `--staged-output`), writes the container into it and
reopens it for its own QC, and the host adopts exactly the bytes the worker hashed
(`StagedArtifactLease::adoptExternalWrite`) before its independent verification read and the atomic
publication. No encoded copy crosses the protocol and no capped spool file exists. Where the
platform cannot hand out the descriptor, the lease answers `UnsupportedPlatform` and the encoded
bytes are copied into a lease staged at publication as before.

`src/media` owns provider-neutral discovery, stream descriptions, indexing, decode products, proxy
requests, audio products, provider selection, and media diagnostics. `src/output` owns export intent,
preservation analysis, render-to-encode preparation, verification policy, and publication. The
task system owns scheduling and lifecycle. `src/platform` owns narrow process, sandbox, file-handle,
and publication services. `src/render` and `src/color` consume explicit interpreted image products;
they do not call codec APIs.

## Project, Session, And Derived State

### Project Truth

A durable media asset records only meaning that cannot be safely rediscovered:

- stable `AssetId` and portable source locator;
- source content identity or the strongest recorded fingerprint, with explicit stale-source state;
- selected video, audio, timecode, caption, or data stream identities;
- source interpretation overrides for rate, start time, field order, pixel aspect, alpha, color,
  audio layout, and channel roles;
- trim or conform relationships expressed in Bloom's rational-time model; and
- preserved namespaced metadata required to round-trip a declared workflow.

The document does not store a provider name, decoder pointer, GPU surface, cache path, probe result,
or hardware decision as render truth. It may store the source's declared codec/container facts and
the interpretation chosen by the artist, but provider selection remains runtime policy.

### Session State

Session state includes the active media stream, audition/monitor choices, proxy preference, visible
diagnostics, scrub intent, and temporary interpretation proposals. Committing an interpretation
change goes through a command.

### Derived Runtime State

Provider probes, packet/frame indexes, decoded frame and audio caches, thumbnails, waveforms,
proxies, conformed cache media, hardware surfaces, and qualification reports are rebuildable. Each
entry is keyed by source content identity, exact interpretation inputs, provider execution identity,
and the version of the Bloom semantic conversion it represents.

## Canonical Media Descriptors

The public media contract uses Bloom values and stable vocabularies rather than `AV*`,
`CMSampleBuffer`, `IMFSample`, VA-API, CUDA, D3D, or vendor SDK types.

A bounded `MediaProbe` conceptually contains:

- container ID/version and exact probe confidence/evidence;
- file byte size and content/fingerprint status;
- every stream's stable in-file identity, media kind, codec/profile/level, codec tag, and extradata
  digest;
- rational stream time base, optional start/duration, edit-list or composition offsets, and
  timestamp-completeness state;
- for video: coded and clean dimensions, pixel aspect, pixel format, sample depth, range, chroma
  sampling/location, field order, frame-rate evidence, alpha, and orientation;
- color-primary, transfer, matrix, range, mastering-display, content-light, and dynamic-metadata
  records, preserving the original code points and whether they were absent, contradictory, or
  inferred;
- for audio: sample format/rate, channel count, explicit channel roles/layout, priming, padding, and
  loudness metadata;
- timecode tracks and source timecode evidence, including drop-frame state and user bits where the
  source represents them; and
- bounded preserved metadata and unknown side-data records under a format-specific namespace.

ITU-T H.273 is the source vocabulary for coding-independent video signal code points; decoding a
numeric code is not the same as resolving a project OCIO color interpretation. SMPTE ST 12 timecode
is a label carried alongside rational media time, never Bloom's arithmetic timebase.

Closed delivery presets additionally name the exact metadata standards they preserve or emit.
Initial vocabularies include SMPTE ST 2086 mastering-display metadata, CTA-861.3 HDR static-metadata
values where a declared delivery mapping uses them, and the applicable SMPTE ST 2094 dynamic-
metadata application. A parsed value retains its original code points or bounded payload and its
source namespace, track, and element location; a provider cannot collapse absent, contradictory,
malformed, and unsupported metadata into one default. A proprietary dynamic-HDR system, metadata
generator, or validator is a separately licensed and qualified capability, never implied by
carrying an H.273 transfer characteristic or a generic side-data blob.

Contradictory container and elementary-stream declarations remain separate evidence until a versioned
interpretation rule or artist override resolves them. Missing information stays missing. A filename,
extension, decoder default, or “most common” television convention cannot silently become project
truth.

## Ingest Pipeline

### 1. Secure Open And Bounded Probe

The host canonicalizes and opens the source under the platform file policy, captures the observed
identity, and gives the worker only a bounded read capability. Format detection uses content and
declared hints; an extension alone is never authoritative. Probe limits bound bytes, packets,
streams, tracks, nested atoms/boxes/KLV sets, metadata records, strings, dimensions, sample rates,
and wall/CPU time.

Probe produces an immutable untrusted result. The host validates every count, size, enum, rational,
timestamp, and UTF-8 field again before constructing a trusted `MediaProbe`.

### 2. Stream Selection And Interpretation

Default stream selection is a visible proposal. The artist or importer may choose another stream
and explicitly resolve color, alpha, field, rate, timecode, and audio-layout ambiguity. The selected
interpretation receives its own canonical identity and participates in decode, proxy, render, and
cache keys.

#### Color tags and config-managed input spaces (COLOR-2)

Video H.273 tags remain evidence from the container; they are not themselves OCIO color-space
ids. With an asset in Auto mode, Bloom maps a video stream with Rec.709 primaries and matrix plus
BT.709 transfer to the selected config's Rec.709 camera/video space, sRGB transfer to the config's
sRGB-texture space, and linear transfer to the effective working space. Missing, contradictory, or
unsupported tags refuse with a typed diagnostic. Rec.2020, HLG, and PQ remain the MEDIA-3 typed
refusal unless the selected config and an explicit artist id provide a separately qualified path.

ARRI LogC3 and RED Log3G10 do not have a container Auto mapping in this contract. Their config
color spaces are explicit artist choices. The Assets and Properties pickers enumerate every
non-data color space in the exact selected config, grouped by its OCIO family, and preserve the
chosen id on the asset or source node. A source-node id overrides the asset; an empty source id
inherits the asset's automatic or explicit interpretation.

Image Auto rules are equally explicit: 8/16-bit PNG, JPEG, and TIFF use the config's sRGB-texture
space; EXR AP0, AP1, and Rec.709/D65 chromaticities map to `ACES2065-1`, `ACEScg`, and
`lin_rec709_scene` respectively; other EXR chromaticities refuse; and an EXR without
chromaticities assumes the working space with a warning. The exact config revision is part of every
resolved input identity.

### 3. Packet And Frame Index

An index is keyed by source identity, provider/version, selected stream, and interpretation. It
records checked byte ranges, decode timestamps, presentation timestamps, durations, key/random
access points, dependency/preroll requirements, discontinuities, and decode-order-to-presentation-
order relationships. It may be built lazily and progressively, but partial coverage is explicit.

Seek is a request for a presentation-time result, not permission to return the nearest decoded
frame. The provider seeks to a safe random-access point, decodes dependency frames, applies edit
mapping, and identifies the exact returned frame. A fast approximate scrub request is a different,
visibly degraded quality mode.

### 4. Decode And Audio Products

The CPU reference video product carries immutable bounded planes plus exact dimensions, strides,
sample representation, chroma geometry/location, range, field state, alpha, source color evidence,
presentation interval, source frame identity, and decode provenance. Conversion into Bloom's
premultiplied `RGBA32F` process image is an explicit media/color operation with its own tested
version; the decoder does not silently resize, deinterlace, range-expand, transfer-convert, or
premultiply.

Audio decode produces immutable planar or interleaved sample blocks with exact source and project
time intervals, sample representation, rate, channel roles, priming/padding state, and provenance.
Resampling, layout conversion, loudness processing, and monitoring gain are separate operations.
Bloom does not need DAW-grade mixing to preserve source audio correctly.

## Audio v0 (AUDIO-1)

The in-process audio path supports WAV and MP3 import and local preview. It remains a narrow
exception to the general worker/provider design; audio is represented in project assets and UI
previews, while broader audio editing is a separate concern.

### Formats And Limits

- WAV is identified from RIFF/RF64 plus WAVE content and decoded through the pinned `dr_wav` header.
  PCM and IEEE float WAV encodings supported by that adapter are converted to Bloom-owned planar
  float32 samples.
- MP3 is identified from an ID3 tag or MPEG frame sync and decoded through the pinned `minimp3`
  core and extended headers with float output. No MP3 encoder or broad codec claim is made.
- `probeAudio(path)` returns the container, sample rate, channel count, frame count, and exact
  `core::RationalTime` duration. `decodeAudio(path, limits)` returns an `AudioBuffer` with one
  float32 plane per channel and rejects partial reads.
- Probe and decode read WAV/MP3 from file-backed decoder callbacks; neither retains the whole
  encoded file. Decode deinterleaves in 4,096-frame chunks. There is no default encoded-file cap;
  callers may set an explicit `fileSizeCap`. Whole-buffer decode has no implicit sample ceiling:
  its caller must supply a sample allowance. The asset worker probes first, checks planar output
  and chunk scratch arithmetic, and reserves those bytes from the shared processing host policy
  before allocation. Its retained lease follows the decoded buffer through preview and playback;
  the audio cache's 2 GiB target governs retention, not decode admission. A buffer larger than the
  cache target crosses a temporary controller delivery slot into the current audio mix's playback
  pin, and that pin releases when the mix stops or is replaced. Cancellation is checked
  during probe, file callbacks, and chunked decode. Wrong magic, truncated input, malformed
  headers, changed source content, invalid formats, and budget violations report typed or exact
  diagnostics without publishing a partial buffer. Import reports the typed reason; a rejected
  audio preview shows it in the asset tooltip instead of claiming the source file is missing.
- The processing lease covers known requested planar storage and one decode chunk. After decode,
  the owner measures the actual `AudioBuffer` object and planar vector capacities, keeping the
  full allocation claimed under the lease. A later physical-memory sample credits only the object
  and initialized vector elements while the owner remains alive; spare capacity and unmaterialized
  phase estimates earn no credit. The credit is revoked before the owner destroys its vectors,
  and the claim is released afterward.
  Provider/decoder internal allocations are still opaque and are not credited as owned audio
  storage; complete accounting of those internals remains part of #304.
- `waveformSummary(buffer, bucketCount)` derives per-bucket minimum and maximum values per channel
  from the immutable buffer. Waveforms are derived runtime state, never document truth.

The three headers are unmodified, checked-in sources. Each is reached through one Bloom-owned
translation unit and a private object-library target; third-party warnings are suppressed only in
that translation unit and strict floating-point flags remain enabled. The public media headers
expose no decoder or device-library types. The security records beside each license state the
accepted in-process risk: resource bounds do not provide a process-isolation or memory-corruption
guarantee, and a future task adapter must check cancellation before and after each bounded call.

### Preview Engine And Synchronization

`AudioEngine` owns a small mix graph of clips containing an `AudioBuffer`, rational start time, level,
mute state, and solo state. The CPU mixer renders fixed-size interleaved blocks into a preallocated
single-producer/single-consumer ring. The backend callback only consumes that ring and zero-fills an
underrun; neither side allocates, locks, decodes, or performs file I/O on the callback path.
`NullBackend` pulls the same callback contract for deterministic tests. `MiniaudioBackend` adapts
the contract to the platform playback device without leaking miniaudio types.

While playing, the audio clock is the master clock. `positionNow()` is derived from the number of
frames actually written by the backend callback, the output rate, the rational play origin, and the
transport rate; it does not advance from wall-clock guesses. The transport follows this clock, so
device callback cadence and any underrun remain visible in the authoritative position rather than
creating a second drifting timeline. `play(at)`, `stop()`, `seek(at)`, and `setRate()` operate on
the same rational transport state. AUDIO-2 connects this clock to the viewer/timeline transport. A
revision-guarded UI audio session publishes the evaluator's device-free mix description, while the
Asset Controller resolves decoded buffers on its blocking-I/O worker. `PlaybackController` replaces
clips on publication, plays from the current rational session time, stops on pause or composition
change, and follows `positionNow()` through `FrameTimeMapping` while audio is active. No backend or
decoded sample is owned by the evaluator, and the wall-clock transport remains the fallback for an
empty mix or unavailable device.

Looped playback wraps inside the mixer. `setLoopRange({start, end})` holds a half-open loop in
timeline time; the mixer maps each unwrapped output position p at or past `end` to
`start + (p - start) mod (end - start)` with exact integer arithmetic in units of
1/lcm(denominators) output frames, so a wrap is sample-accurate and gapless and a loop that is not a
whole number of samples long alternates pass lengths instead of drifting. Clip start/end checks,
mute, solo and per-clip time mappers all evaluate at the wrapped position. `positionNow()` stays
unwrapped and monotonic, so the transport's frame-modulo video wrap and the mixer's rational wrap
derive from one clock. `PlaybackController` hands the mixer the work area's frame grid,
[first valid frame, last valid frame + 1), whenever a run starts, the work area or frame rate
changes, or looping is toggled; with looping off there is no loop and a run plays past the end
exactly as before.

A loop change while playing needs no device restart. The mixer stops pushing, the next device
callback plays what it already took from the ring and discards the rest, and the mixer rewinds by
exactly the discarded frame count and re-mixes them under the new range (the mixer settles the
request itself when nothing is queued). The change is therefore audible after at most one device
buffer. Clearing the loop mid-run keeps the pass the run is in: the offset of that pass stays
fixed, so the run continues to the work-area end like one that never looped. A work-area change
still relocates the run with a seek, because the transport re-anchors its playhead; a wrap never
seeks. Frames the device consumes while the ring is empty are counted, and the mixer skips them so
the mix stays on the device clock after an underrun (most visibly the first callbacks after start)
instead of lagging it for the rest of the run.

Composition sources retain the evaluator's ordered offset, scale and loop mappings on each leaf
clip. The application attaches a time mapper alongside the shared decoded buffer; the mixer worker
applies it per output sample before the leaf clip's start/end checks and sample-rate conversion. An
enclosing Layer outside its range contributes silence. Nested clips sum with resolved mute and
solo controls, and an empty nested mix clears previously published clips. The mapper performs no
I/O or allocation and never runs on the device callback. The evaluator and the playback engine
remain independent: the application supplies the runtime mapping through the clip adapter.

### 5. Proxies, Thumbnails, And Waveforms

Proxies are derived assets with a manifest binding source content, selected streams, interpretation,
time mapping, conversion version, codec/container preset, provider provenance, and semantic fixture
profile. A proxy is used only when that manifest matches. The UI always indicates proxy or reduced-
resolution playback; final render never consumes a proxy unless an explicit offline-media policy
allows it and records the consequence.

Thumbnail and waveform generation share the same interpreted source and bounded task contracts.
They do not run independent “quick” probes that can disagree with the Asset Registry.

## Audio v1 Wiring (AUDIO-2)

Audio assets are imported by the same worker transaction as images. A batch import records each
file that fails probing, hashing or scanning as an `ImportFailure` (path and diagnostic) and keeps
going; the remaining files publish as one undoable `ImportAssets` transaction, and each failure is
attached to the import job as a warning diagnostic and to the operation result as an issue.
Cancellation keeps every file already prepared and abandons the one in progress (#377,
[ADR 0029](../decisions/0029-import-progress-and-cancellation.md)); only cancellation before anything
was prepared, an over-large request, or a batch with no importable file refuses the whole import. A
chosen folder contributes the media files below it, and the numbered members of one sequence are
prepared once. `probeAudio` supplies the
persisted rate, channel count, frame count and exact duration; `decodeAudio` and `waveformSummary`
run only on the blocking-I/O worker. The UI publishes a bounded waveform summary for Assets, the
timeline bar and the audio source node card. Painting reads the immutable summary and never opens or
decodes a file.

The UI/audio boundary maps a compiled clip's stable asset ID to a published planar `AudioBuffer`.
`AudioPlaybackSession` pins the current mix's buffers before calling `AudioEngine::replaceClips`;
the controller can then release an above-cache-target delivery buffer. Audio asset records,
source parameters, layer flags and
graph edges are project truth; decoded buffers, waveform summaries and device state are disposable
runtime state. WAV/MP3 availability and the fallback behavior are the same on Linux, macOS and
Windows; a missing or unreadable file shows an explicit warning and contributes no samples.

### Image-Sequence Assembly

An image sequence is an explicit ordered asset, not a filename glob evaluated during every render.
Discovery parses one selected portable path pattern under bounded directory enumeration, records the
exact frame-number-to-source mapping and content/fingerprint evidence, and reports duplicates,
missing numbers, changing padding, mixed extensions, unreadable members, and per-frame descriptor or
metadata changes. Case folding and filesystem enumeration order never define frame order.

The artist chooses how gaps map to time: fail, hold, transparent/black placeholder, or another
versioned policy. Defaulting that choice is visible and becomes project truth when committed. The
derived sequence manifest binds the chosen rational rate and start label separately from member
frame numbers. A source member changing invalidates only the affected identities and dependent
caches, but a final render with an unapproved gap or stale member fails rather than silently using an
old cached frame.

## Disk cache

Task CACHE-2 adds `src/media/cache` (Qt-free): a bounded, content-addressed, on-disk store of
decoded `Rgba32fImage` frames that persists across restarts, complementing the two v0 in-process
caches above (the evaluator's memory-only decoded-image LRU and the UI's memory-only 512-entry
thumbnail cache). It is the same store both consult: `decodeThroughDiskCache()`
(`bloom::media::cache`) is the one decode-through-caches entry point `evaluateImageSource()`
(`src/runtime/image_source.cpp`) and the Asset Controller's proxy/thumbnail decode
(`src/ui/asset_controller.cpp`) both call, so a frame decoded by one is a disk hit for the other.

**Location.** The platform cache directory (`bloom::platform::userCacheDirectory()`, Linux
`$XDG_CACHE_HOME/bloom` or `$HOME/.cache/bloom`) plus a `media` leaf, or a `media/disk-cache-directory`
QSettings override (an absolute path; anything else falls back to the platform default). The
enable flag, directory, and byte budget are edited under **Edit → Preferences… → Memory & Caches**
(the disk cache can also be purged at any time from **Edit → Purge… → Purge media cache**); the
operation-cache and RAM-preview budgets share that page. These values are read once at startup, so a change takes effect after
restart. The owning reader is `bloom::ui::media_disk_cache_settings` for the disk cache and
`preview_frame_cache.hpp`'s `ramPreviewByteBudgetFromSettings()`/`operationCacheByteBudgetFromSettings()`
for the two memory ceilings.

**Cache key.** Content-addressed and restart-stable, deliberately narrower than the evaluator's
in-process operation-cache key: asset content digest, member/frame, legacy interpretation, resolved
input color-space id, effective working-space id, exact OCIO config revision, alpha association,
the synthesized-space identity of a Bloom-synthesized input space (Rec.2100 HLG: its definition and
OCIO version; empty, and absent from the key bytes, for ordinary config spaces), and a decoder
identity/version string (`bloom::media::cache::kImageDecoderIdentity`) bumped
whenever the decode or config-managed conversion pipeline could change decoded pixels for the same
source bytes. The key omits the resolved path/relink hint and availability that the memory-cache key
includes, so a relink of identical content still hits the disk entry. Changing the input id,
working id, or config revision necessarily misses both decoded image and video-frame caches.

**Format (v0).** Uncompressed Float32 RGBA, matching the process image exactly: a small fixed
header (data/display window, pixel aspect, payload byte count) plus the raw pixel payload, and a
SHA-256 digest of the payload bytes stored alongside it and revalidated on every read. Any
structural or digest mismatch (truncation, a flipped bit, an unrelated file) is treated as an
ordinary miss and the entry is removed -- corruption never reaches a caller as pixels. A future
task may add a lossless codec (LZ4/zstd) through the dependency intake; v0 deliberately does not,
per the CACHE-2 task package.

**Writes.** Atomic: a temp file in the entry's own shard directory, fsync'd where the platform
supports it (Linux), then renamed over the target -- a reader never observes a torn file. An
index file (`index.v1`, itself rewritten atomically) records each entry's byte size and last-use
time so a restart rebuilds LRU order without re-stat'ing or re-hashing every entry. `store()`
writes synchronously on the calling thread; `storeAsync()` -- what `evaluateImageSource()` uses on
a disk miss -- hands the write to the cache's own single background writer thread and returns
immediately, so evaluation never waits on the write. A full write queue drops the newest write
(counted, never blocks): losing one write only means the next read decodes again.

**Budget and eviction.** LRU under a byte budget (default `min(10% of the cache volume's free
space, 32 GiB)`, `media/disk-cache-budget-bytes` overrides it) and, independently, a bounded entry
count (`kDefaultMediaDiskCacheMaxEntries`, 200,000) so a cache of many tiny images cannot grow the
index and directory-entry count without limit either. Both are enforced on every write.

**Never cached.** An interactive or overridden evaluation request's pixels belong to a gesture,
not to a revision (see `animation-and-time.md`'s "Direct Manipulation And Preview Overrides"); the
same condition that already excludes such a request from the evaluator's derived operation cache
(`request.bypassOperationCache` / `plan->bypassOperationCache()`) also passes a null disk cache
into `evaluateImageSource()`, so an override is never read from or written to disk either.

**Read-only decoded-image exception (CACHE-1).** The evaluator's memory cache now distinguishes the
two bypasses that used to share one switch. An explicit evaluation bypass
(`request.bypassOperationCache`) disables derived operation memoization and the decoded still-image
memory and disk entries completely, consult-and-store alike. It is a per-evaluation control, not the
preview controller's frame-cache refresh, and it does not touch the video decoded cache or
color-processor caches. A plan compiled for an interactive parameter override
(`plan->bypassOperationCache()`) still bypasses derived operation memoization, but may READ an
already-verified, immutable native decoded STILL-IMAGE entry under its EXISTING content-addressed
memory source key (`selected.cacheKey`: asset digest, member/frame, interpretation, alpha, input
processor/color-space id, OCIO identity, plus the resolved path/relink/availability that key already
carried). It never inserts a source entry on a gesture miss, so gesture data cannot land under an
unchanged source key, and the disk cache stays null: an interactive miss decodes directly and
uncached. Source verification follows the immutable revision contracts below. Motion video uses
its own decoded-frame cache and disk-backed source revision, with the same read-only gesture and
explicit bypass lookup/insertion gates. Nested
compositions remain conservatively fully bypassed on an override (no read-only reuse in the child)
for this slice.

**Immutable still-source revisions (2026-10-13).** A worker captures one bounded encoded byte vector,
hashes those exact bytes in cancellation-aware chunks and probes that vector. PNG/JPEG callbacks and
the OpenEXR input stream subsequently decode the same immutable bytes. The revision is runtime
state; its SHA-256 must match the selected asset or sequence-member digest. A concurrent path
replacement cannot change an admitted revision's decoder input.

**Identity-verified sources (2026-10-23, [ADR 0025](../decisions/0025-identity-verified-media-sources.md)).**
Every full source hash or capture records the file identity (length, modification and
status-change times, volume and file id) observed before and after the pass in the bounded
process-wide `platform::VerifiedSourceDigests` memo. While a later observation of the same path is
identical, the recorded digest stands for the bytes; any difference forces a full rehash, and a
changed digest still reports `SourceChanged` and the relink diagnostic. A pathname or size alone
never proves content. Whole-file reads are counted by `platform::sourceReadWork()`.

**Image-sequence read-ahead.** Each evaluator owns a `runtime::ImageSequenceReadAhead`, carried
in its `GpuSceneMediaContext`, so preview, playback and export requests sharing the evaluator
share it. When GPU scene preparation verifies a sequence member, it queues the distinct members the
next eight composition frames select (start frame, loop mode and gap hold included). Four I/O
workers capture them as ordinary verified revisions under the same revision-cache keys, so a later
selection reuses them after the usual identity observation and reads nothing itself. Several reads
stay outstanding, which is what a network share needs: on an SMB share measured at 61-66 MB/s with
one reader, four and eight concurrent readers reached 75-82 MB/s. Read-ahead never evicts; a file
that does not fit the cache's free budget, or one already cached, queued or in flight, is skipped.
Destroying the evaluator cancels outstanding captures and joins the workers.

`EvaluationRequest::mediaSourceRevisionPolicy` defaults to `Revalidate`: source verification
precedes CPU operation memoization and GPU prepared-upload lookup, including nested evaluation. A
retained still revision is reused under `Revalidate` only after an identity observation of the
original matches the memoized digest; otherwise the source is captured and hashed again. Final/headless requests retain that default. The production GPU final preparation and host output-
analysis CPU fallback explicitly force `Revalidate`, including when passed a copied interactive
request that says `Retained`. CPU final callers also set `requireVerifiedMediaSources`, which forces
fresh source verification and fails unavailable/changed media before memoization instead of accepting
the preview placeholder. The flag propagates through nested evaluation and is availability policy,
not pixel identity. Explicit `bypassOperationCache` always
revalidates and disables revision lookup/insertion. Interactive callers explicitly choose `Retained`
to use the last verified, owned revision while its source entry remains resident. A warm parameter
edit then performs no source probe, hash or decode; this is captured-source provenance, not a claim
that the current external path was rechecked. A missing or externally replaced file can still show
its retained interactive revision until a refresh boundary. Final revalidation reports it unavailable
or changed and never substitutes the retained source for the missing/changed bytes.

Tickets share the evaluator's existing decoded-media LRU and live host retention assignment.
Encoded bytes, probe strings and entry/key metadata are charged; there is no separate owning cache.
Interactive override plans may read an existing ticket but never insert one on a gesture miss;
uncached captured bytes remain transient. Source tickets are accepted only as typed decoded-media
entries, so operation purge/retirement cannot fold their debit through the wrong accounting class.
Rooted tickets use the same explicit host retirement owner as prepared uploads. Eviction and purge
detach the ticket, then the worker drops its backing and metadata before folding back the decoded
host charge. Pending tickets cannot donate capacity, and producer close detaches them before host
owner shutdown. Thus UI sampling callbacks never free a large rooted encoded vector. Standalone
unbound pools retain their existing direct-release behavior.
Capture is admitted against the current request/source working allowance before allocation, and
encoded storage is subtracted before codec working-set admission. A zero retention assignment still
allows bounded fresh preparation, but stores no revision. Eviction releases only the cache's owner;
an already-held revision remains valid. Refresh/reconnect/media purge clears decoded-media tickets;
a generation guard prevents a capture begun before that boundary repopulating source authority.
Relink, changed locator/base directory or changed asset/member digest uses another ticket identity.
`OperationCache::invalidateSourceRevisions()` is the neutral New/Open authority boundary. It
advances the capture generation and detaches still tickets and video source sessions while preserving
decoded/derived pixels and disk entries; the desktop rooted retirement worker releases their backing.
New/Open document handling invokes it before session rebind and interactive reuse. These
rules do not extend the still-image byte-vector ticket to TIFF's locator-based worker provider.
Linux motion video uses the separate disk-backed revision described below.

Per-build GPU scene statistics expose encoded bytes read/hashed, revision captures/hits and monotonic
nanosecond durations for source open/read/seek, SHA-256, metadata probe and actual image decoder
calls. Decoder duration includes image construction and any requested connected-path input color
processing; it excludes source I/O, digest, probe, cache lookup and proxy rebasing. New counters
saturate rather than wrap. A retained upload hit records zero source I/O/hash/probe/decode work,
instead of replaying the original revision's capture measurements. These are observed stage costs,
not a sustained-throughput qualification.

**Immutable Linux video-source revisions (2026-10-13).** Supported movie imports, frame/audio
reads and demux indexing capture a local regular file once into bounded private spool storage while
hashing the exact chunks written. A live capture is indexed by digest (non-owning), so a second
session for the same verified digest — Assets thumbnails and audio, preview, export — adopts it
instead of copying the movie again. Capture subtracts snapshot/temporary-path metadata from the current host working allowance,
then uses the remaining allowance for a chunk buffer of at most 64 KiB, without a fixed buffer floor.
It never retains the
whole encoded movie in RAM. The original pathname authorizes hash elision only through an identical
identity observation recorded by a full hash (ADR 0025). Before a
revision is published, the write descriptor is closed, the temporary filename and private directory
are removed, and only an owning read-only file descriptor remains. The SHA-256 and captured length
are tied to that host-created type. Publication also verifies that the no-follow read-only open has
the writer's exact device/inode identity and length, and that its link count is zero after unlink and
writer close. A replaced temporary pathname, symlink or surviving hard link fails with typed I/O
diagnostics and releases the capture quota; it cannot publish a mismatched digest/decoder source.
A malicious process controlling the same user/debugger is outside
this ownership contract; filesystem timestamps and mutable artist paths cannot change admitted bytes.

The worker-pool host accepts that owning capability separately from serialized request fields.
The process supervisor passes only its read descriptor to the fresh Linux worker, whose custom
FFmpeg AVIO reads/seeks that exact file. A request claiming immutable bytes without the inherited,
read-only, anonymous, exact-length regular file is rejected. Ordinary external-path worker requests
retain their existing source verification. Schema 4 carries bounded snapshot length; a wire value
alone cannot manufacture source authority. Standard FFmpeg demux/codec and color boundaries remain
unchanged, and this mechanism does not upgrade provider/delivery qualification.

The evaluator retains at most four source-keyed decode sessions. A shared eight-file disk quota
bounds retained revisions, captures in flight and unlinked handles pending retirement. Its byte
ceiling is derived from the snapshot spool volume when the context is created (issue #352): three
quarters of the free space there, never below 32 GiB and never above four 1 TiB sources; a capture
beyond the real free space still fails with its I/O error. Each source is also subject to the
1 TiB intake limit. These are separate from decoded-frame RAM
retention. Exhaustion returns typed `Oversized` rather than overcommitting or reusing stale bytes.
Capture exposes chunk cancellation/progress and numeric read/hash stage costs. Source-key identity
contains asset digest, locator/relink hint and base directory. `Retained` can reuse captured bytes for
interactive edits, including uncached frame decode when the artist path disappears, with zero source
I/O/hash. Read-only gestures may look up existing sessions/revisions/frames but cannot insert new ones.
`Revalidate` observes the original's identity before decoded/prepared cache lookup and recaptures
only when it changed or is unknown; production final preparation forces it even for an explicitly
copied `Retained` request. A changed digest or missing original is
reported before cached pixels are accepted. Media refresh/reconnect removes both decoded frames and
source sessions. Sessions are installed before capture, so an in-flight old session may finish its
own revision but cannot repopulate the refreshed session list.

Rooted snapshots register with the existing composition-root retirement lane. Session and snapshot last release enqueue
only their small intrusive nodes; the worker closes the anonymous file before the disk quota is returned.
Pending handles cannot donate capacity. The existing source pool close also closes its typed weak
video producer attachment, so retained sessions cannot prevent host-worker exit while an evaluator
remains alive. Bootstrap/library verification and session construction hold no UI-close mutex;
refresh generations reject construction that finishes after invalidation. Unbound headless sessions
release on their calling worker.
The current qualification scope is Linux. macOS verifies video sources through the same
identity memo before each path-based worker call; Windows runs the FFmpeg worker through the
Windows supervisor and verifies through the same memo. Neither is described as qualified by this Linux implementation.

**Controls.** `media/disk-cache-enabled` (default on), `media/disk-cache-budget-bytes`, and
`media/disk-cache-directory` in QSettings; **Edit → Purge… → Purge media cache** clears every entry
and resets statistics, together with the applicable in-memory decoded media, on the task system's
BlockingIo lane (never the UI thread, and with no confirmation, since the caches are rebuildable).
Source files are never read, written, or changed. The window status bar's own disk-
cache cell (`mediaDiskCacheStatusText()`, beside the existing RAM-preview cache cell) reports a
lightweight-polled hit rate and resident byte count, or "Disk cache off" when disabled -- a
second, independent budget from the RAM preview and operation caches `animation-and-time.md`
documents.

| Bound | v0 value |
| --- | --- |
| Media disk cache budget | `min(10% of free disk space, 32 GiB)` default; settings override |
| Media disk cache entry count | 200,000 |
| Media disk cache async write queue | 64 pending writes, oldest-blocking dropped when full |

## Export Pipeline

Every time-based preset is a closed, versioned tuple covering at least:

- container and structural profile;
- video codec, profile/level, chroma sampling/location, sample depth/range, frame dimensions,
  scan/field mode, rational rate, pixel aspect, and alpha policy;
- input and encoded color identities plus exact signaling and HDR metadata policy;
- rate-control, GOP/random-access, entropy, quality, and hardware/software policy;
- audio codec, sample representation/rate, channel roles/layout, priming, loudness, and track
  assignment;
- start time, timecode, edit list, duration, track ordering, metadata allowlist, and forbidden
  volatile fields;
- provider qualification requirements; and
- reopen, independent-reader, semantic comparison, and external QC requirements.

The pipeline is:

1. Capture one immutable document snapshot, output request, provider generation, dependency
   revisions, and canonical destination intent.
2. Derive a machine-readable preservation report before rendering. Unsupported rate, color,
   alpha, channel, metadata, or provider requirements block staging; declared approximations require
   exact-digest approval under the output contract.
3. Render frames and audio under bounded back-pressure. Conversion into the encoder's planes is an
   explicit, versioned output operation; it is never delegated to an ambient provider default.
4. Send ordered immutable frame/audio products to the qualified encoder. Collect packets with exact
   timestamps and side data, then mux under the preset's closed ordering and metadata rules.
5. Finalize the container into a private staged artifact, flush, close every provider handle, and
   return typed execution provenance. A worker never receives the final publication path.
6. Reopen from disk. Parse the container and bitstream structure, enumerate tracks, decode samples,
   and compare timing, color, alpha, audio, metadata, frame count, and pixels against the approved
   request using the preset's exact or tolerance-based rules.
7. Run required independent or recipient QC and store its tool/version/profile identity and complete
   result. “Tool exited zero” alone is not a QC report.
8. Revalidate the staged artifact and target fingerprints, then atomically publish through the
   application-wide `StagedArtifactCoordinator`.

A lossy codec normally cannot reproduce process samples bit-for-bit. Its preset freezes the decoded
comparison domain, chroma reconstruction, color conversion, alpha rule, objective limits, structural
constraints, and required visual-QC ownership. Thresholds cannot be widened after observing a failed
provider without a reviewed preset version.

## Qualified Provider Model

### Implemented MEDIA-K1 Contract And Worker

Status: MEDIA-K1 established the synthetic provider and Linux process backend (2026-09-17).
MEDIA-3 adds the FFmpeg read provider described under "Video Read Implementation" below.
The larger qualification contract continues to govern which real-codec paths are admitted.

`bloom_media_provider` owns the Qt-free, codec-free contract, canonical identities, registry,
and protocol. It depends only on `bloom_core`. `bloom_media_worker_host`, also located under
`src/media/provider`, is the host facade over `bloom_platform_process` supervision and the
core-only `bloom_runtime` scheduler target. The evaluator is a separate target; neither the
scheduler nor the contract library depends on the host facade, so target dependencies remain
acyclic. The repository module allowlist permits that narrow scheduling edge. The desktop
must never link `bloom_media_fake_worker_provider` or `bloom_media_ffmpeg_worker_provider`.
MEDIA-3 adds video document records and UI projections above this codec-free contract.

The five records in `include/bloom/media/provider/contract.hpp` are frozen as follows:

- Each encoding begins with its exact C++ record name as a length-prefixed ASCII domain, then
  a little-endian `u16` schema version of 1. All subsequent fields appear in declaration order.
- Integers use their declared fixed width and little-endian order. Signed values use two's
  complement. Enums and booleans occupy one byte; true is 1 and false is 0. Digests occupy 32
  raw bytes. Strings contain a `u32` byte length followed by valid UTF-8 without NUL or implicit
  terminators. Vector counts are `u32`; elements follow in order. No ABI padding is encoded.
- Rationals contain signed 64-bit numerator and denominator, reduced with a positive
  denominator. A missing capability field is rejected; an inapplicable field must explicitly
  name its semantics, such as `none`. Fields are never wildcards.
- SHA-256 covers the entire canonical encoding. The independent C++ test reconstructs all
  five byte streams through literal fields and a separate hexadecimal encoder. Qualification
  and authority fields remain separate, including issuer, reference, product, scope, verified
  date and review date. An absent authority has six empty strings.
- Pipeline steps contain ordered capability/execution/evidence digests. A tolerance claim
  requires a nonzero immutable tolerance-profile digest; other classes require its absence.
  `NoDeterminismClaim` is restricted to the explicitly limited Preview path in this slice.
  Component registration never registers or qualifies a pipeline automatically.

The numeric v1 budgets below instantiate the resource categories described by this document.
Stricter qualified providers may reject requests within these ceilings. Expanding a ceiling requires
a reviewed profile/protocol change. The in-process still-image decoder is governed separately and
explicitly: its geometry is bounded only by the codec's representable window and integer overflow
checks, the encoded file is streamed with no whole-file cap, and decoded RGBA32F storage plus
bounded decode scratch must fit the caller's explicit `pixelBudget` (default 268435456 bytes)
before any large allocation. Codec/device constraints, not an absolute image ceiling, are the limit.

| Resource | v1 ceiling |
| --- | --- |
| UTF-8 string | 4096 bytes |
| Streams, declarations, pipeline steps, registry entries | 256 each |
| CPU planes | 4, with exact plane count/layout for each supported format |
| Dimension / pixels | codec-representable window and sample-count limits; no fixed image ceiling |
| CPU-plane storage including row padding | explicit caller `pixelBudget` for the in-process image decoder (default 256 MiB) |
| Wire frame excluding its length prefix | 513 MiB (one 8192 x 8192 16-bit 4:4:4:4 product) |
| Audio | 64 named channels, 65536 samples per channel, 384000 Hz |
| Worker address space / open descriptors | per stream: 1 GiB + twelve 16-bit 4:4:4:4 frames, 2 GiB floor (UHD), 4 GiB at 8K UHD, 16 GiB ceiling / 64; core dumps disabled |
| Default pool slots / call deadline | 2 / 5 seconds |
| Default cancellation message / SIGTERM grace | 50 ms / 100 ms |

Video CPU formats currently close to RGBA8, RGBA32F, YUV420P8 and YUVA444P16. Plane dimensions, stride,
byte count, chroma extents, aggregate allocation, and SHA-256 are revalidated by the host.
Probe streams preserve original H.273 color tags with -1 for absence. This does not resolve
color interpretation. Audio values use finite planar floats, explicit unique channel roles,
matching sample counts, and bounded rates; MEDIA-3 implements this audio transport and decode.

The wire envelope is `u32 body_length`, `u32 magic=0x314d4c42`, `u16 protocol=1`,
`u16 schema=2`, `u8 kind`, `u64 session`, `u64 sequence`, then payload. The fixed body header
is 25 bytes. Every response echoes the host's nonzero session nonce and next sequence.
Handshake, Call, Probe, Frame, Cancel, Shutdown, Ack and Failure use kind values 1 through 8.
Index and Audio use 9 and 10.
The host checks the complete expected execution identity, including provider/build, lock digest,
OS/architecture/SDK/driver/device, generation, software/hardware use, transport, synchronization,
resource profile, entitlement, availability and trust domain. The handshake also binds exact
capability/evidence declarations, ordered pipeline digests and transport modes. The host owns
qualification expectations; a worker cannot promote its own authority. A rejected frame never
advances protocol state. Transport 0 copies length-prefixed planes over pipes; transport 1
reserves shared-memory slabs and cannot be selected for execution in this version.

`MediaWorkerPool` submits only `BlockingIo` tasks with indeterminate activity, cancellation,
diagnostics and bounded process-slot admission. The scheduler bounds queued requests; terminal
task history and calls cancelled before execution retain no process slots. Each call owns a fresh
process and its pinned execution;
no automatic restart or provider substitution occurs. Products become visible only after a
validated reply, shutdown acknowledgement, and successful process exit. Pool destruction requests
shutdown without waiting on the UI thread; task-owned state lives until the scheduler has reaped
all children. Callers retain the scheduler's normal safe-shutdown obligation.

Linux uses `posix_spawn`, separate stdin/stdout pipes, and a startup gate on descriptor 3.
The trusted worker calls the bootstrap before provider work; the parent installs `RLIMIT_AS`,
`RLIMIT_NOFILE` and `RLIMIT_CORE` before releasing that gate. Other descriptors are closed,
stderr is discarded in favor of typed protocol diagnostics, and the environment contains only
`LC_ALL=C` and `TZ=UTC`. No loader override is inherited. This is resource/process containment,
not a filesystem/network sandbox or authorization to launch untrusted executables. Restricted
media-file access capabilities and additional sandbox policy belong to real-provider intake.
Poll-based pipe I/O shares a monotonic deadline. Cancellation sends a message, waits its grace,
sends SIGTERM, waits again, then SIGKILL and reaps. No global host signal handler or detached
watchdog thread is installed. macOS uses the same POSIX design without kernel rlimits. Windows
(ADR 0035) uses byte-mode named pipes with overlapped I/O under the same deadline and cancellation,
a gate pipe whose inherited handle the child reads from `BLOOM_PROCESS_GATE`, an explicit inherited
handle list (its pipe ends, the gate and `NUL` for stderr), and a job object with the
committed-memory limit, one active process and kill-on-close; stop is message, grace, closed stdin,
grace, `TerminateJobObject`. Windows refuses the descriptor-based source snapshot and streamed
staged output, so the host takes its path and chunk-copy routes there.

The fake build identity hashes the boundary/provider source generation and compiler configuration;
its dependency identity separately hashes the reviewed lock bytes at configure time.
The fake provider accepts only `synthetic:rgba8`, dimensions up to 64 by 64 and frame indices
through 100000. It probes one video stream at 24 fps and generates R=x, G=y, B=frame modulo 256,
A=255. These fixtures prove protocol/process behavior, not codec or real-file qualification.
`bloom.media.worker` covers independent probe/decode results, installed rlimits, process reaping,
kill -9 after request receipt, cooperative and forced cancellation, timeout, pool shutdown,
and hostile/truncated/oversized/replayed/version/identity/digest responses. Synthetic fixture
cases live under `tests/fixtures/media`; no binary media is checked in.

#### Worker Rpath Policy

Installed worker generations occupy `libexec/bloom/media/<provider-generation>/`, with private
shared dependencies under their own `lib/`. Linux worker install rpath is exactly `$ORIGIN/lib`;
CMake does not append link directories. The FFmpeg worker uses this same rpath in build and
install trees, avoiding empty loader-search entries; its private libraries and Bloom OpenH264
shim use `$ORIGIN`.
The synthetic test worker needs no codec library and is not installed. MEDIA-3 places the
reviewed shared FFmpeg closure in its worker generation's private directory. The desktop must
continue to pass `desktop-no-ffmpeg`; loader paths and codec-bearing worker targets must never
be propagated through `bloom_media_provider`.
macOS installs `bloom-media-worker` and its `lib/` dylibs (unmodified, `@rpath` install names,
worker rpath `@loader_path/lib`) and `bloom-media-videotoolbox-worker` inside
`Bloom.app/Contents/libexec/bloom/media/`; Windows installs the worker with its FFmpeg DLLs beside
it under `bin\media\ffmpeg-v1\`, never beside `Bloom.exe` (ADR 0035).

### Internal Boundary

Known in-tree code uses a small typed C++ provider interface inside `src/media`/`src/output` for
discovery, probe, index, decode, encode, mux, flush, and reopen operations. It is not a generic
multimedia object model. Providers register factories and immutable capability declarations; normal
frame-by-frame calls remain direct and typed within the owning worker adapter.

### Process Boundary

Container and codec parsing consumes untrusted bytes and has a material vulnerability history.
Release media providers therefore run in supervised `bloom-media-worker` processes unless an
accepted qualification explicitly permits a narrow in-process parser. This boundary also contains
commercial SDK ABI and license differences.

The worker protocol is versioned, length-delimited, endian-defined, bounded, and independent of Qt,
C++ ABI, provider structs, and host pointers. The handshake binds:

- protocol and schema versions;
- provider/build/dependency-lock identities;
- OS, architecture, SDK, driver, and hardware identities where relevant;
- exact capability tuples and Bloom qualification records;
- authorization/certification evidence as attributed, dated records; and
- supported CPU and external-surface transport modes.

The protocol begins as an internal compatibility boundary. It is not called a public codec plug-in
SDK until lifecycle, compatibility fixtures, signing/trust, packaging, update, and support promises
exist. Commercial or separately distributed providers may use the same boundary without making
their SDK types or source part of Bloom.

Each admitted worker process loads exactly one provider generation and one dependency/licensing
trust domain. A common launcher and protocol implementation may be reused, and one provider package
may internally combine a demuxer, codec, and muxer, but Bloom does not load every available provider
into one mega-worker. The executable/package identity, dependency namespace, sandbox permissions,
resource limits, and capability declaration are bound together by the handshake. Combining
individually qualified components inside one process still requires qualification of that exact
ordered pipeline.

### Closed Roles And Purposes

Version 1 closes operation roles to `Probe`, `DemuxIndex`, `VideoDecode`, `AudioDecode`,
`VideoEncode`, `AudioEncode`, `Mux`, `ReopenDecode`, `StructuralQc`, and `RecipientQc`. It closes
purposes to `Preview`, `Proxy`, `Conform`, `Export`, and `Delivery`. A provider declares every role
and purpose it implements; an unlisted value is unavailable. `VideoDecode` does not imply
`DemuxIndex`, `ReopenDecode`, or any purpose other than the one qualified.

Preview and Proxy may admit explicitly visible latency or quality tradeoffs. Conform requires
frame-accurate full-resolution ingest. Export proves Bloom's staged artifact contract. Delivery
adds a named recipient specification and required independent QC. These purposes are semantic
claims, not scheduler priorities.

### Capability, Execution, Evidence, And Pipeline Records

Provider selection and reporting use four immutable records rather than one overloaded support
flag:

- `MediaCapabilityKeyV1` describes one provider-neutral role and purpose plus the exact container
  mapping, codec/profile/level/tier, sample entry, bit depth, range, chroma and location, alpha,
  field mode, dimensions and rate limits, audio format/layout, color/HDR/timecode/metadata features,
  timing/reordering behavior, and CPU-plane or external-surface semantics;
- `ProviderExecutionKeyV1` identifies the provider/build/dependency lock, protocol, platform and
  architecture, software or exact hardware implementation, OS SDK/driver/device scope, memory
  transport, synchronization mode, resource-limit profile, and non-secret entitlement mode and
  availability state;
- `QualificationEvidenceV1` binds Bloom's fixture-set digest, qualification result, review date,
  optional review deadline, and any separately attributed authority record. Technical evidence and
  external authorization are different fields and neither is inferred from the other; and
- `PipelineQualificationV1` binds ordered capability, execution, and evidence identities to one
  purpose, preset or ingest profile, fixture-set digest, conversion versions, determinism class,
  reopen policy, and QC profile.

Selection matches every field that can change meaning or acceptance. A missing field is unavailable,
not a wildcard. Individually qualified demux, codec, conversion, mux, and reader components do not
compose into a qualified pipeline unless the exact ordered `PipelineQualificationV1` passed its own
fixtures. This prevents provider defaults or a compatible-looking sample entry from silently
changing a qualified claim.

The closed `MediaDeterminismV1` classes are:

- `ByteExact` — the complete admitted output bytes are reproducible for the frozen execution key;
- `DecodedSemanticExact` — bytes may differ, but the frozen reference decode domain compares
  exactly;
- `DecodedSemanticTolerance` — comparison uses one immutable versioned tolerance-profile digest;
  and
- `NoDeterminismClaim` — usable only for Development or an expressly limited Preview path.

Lossy encoding normally uses `DecodedSemanticTolerance`. Hardware availability, driver choice, or a
successful encode call cannot upgrade a determinism class. Tolerances are selected before execution
and cannot be widened after observing a failure.

### Qualification And Authority

Qualification is per tuple and per direction. A hardware decoder does not inherit a software
decoder's result. Decode qualification does not grant encode qualification. MOV qualification does
not grant MXF qualification. A missing field is unavailable, not a wildcard.

Bloom qualification and external authority are orthogonal:

- `Development`: callable but not eligible for artist promises;
- `PreviewQualified`: safe and fast enough for a declared visible preview path;
- `ConformIngestQualified`: frame/time/color/alpha/metadata ingest gates pass;
- `ExportQualified`: Bloom's exact staged export and reopen gates pass;
- `DeliveryQualified`: the exact recipient profile and required independent QC pass; and
- an optional attributed authority record names who authorized or certified precisely what. Bloom
  never manufactures that last claim.

No provider discussed in this working research is qualified merely by being listed. Vendor product
statements and platform API documentation establish candidates and questions, not Bloom capability
records. License, patent, trademark, redistribution, certification, and recipient acceptance remain
external gates even when technical fixtures pass.

### Provider Selection, Failure, And Substitution

An ingest or output attempt captures one provider generation and exact
`PipelineQualificationV1` before work starts. Once any product from that attempt can be published or
approved, Bloom never substitutes a provider, software/hardware path, profile, precision, range,
chroma, color transform, alpha rule, rate-control mode, metadata policy, or QC policy inside the
attempt. A crash, device loss, revoked entitlement, resource exhaustion, or failed verification
terminates that attempt without partial publication.

Preview may start a new visible generation with another `PreviewQualified` CPU or proxy path.
Conform, Export, and Delivery require a new analyzed attempt; output approval never transfers to a
different pipeline or execution generation. Resource pressure never silently resizes, changes
rate, drops streams, or lowers output quality.

When no Apple-authorized and Bloom-qualified pipeline matches a strict conventional ProRes preset,
selection returns typed `codec.prores.strict-provider-unavailable` with the missing direction,
container mapping, profile, platform, and authority scope. It does not select an FFmpeg-derived
implementation. A verified image-sequence plus BWF/BW64 handoff is a separately named preset and
request with its own analysis and approval, not a fallback execution of the ProRes attempt.

## Hardware Acceleration And Zero-Copy

VideoToolbox, Media Foundation/D3D11, VA-API, Intel VPL, NVIDIA Video Codec SDK, and AMD AMF expose
useful hardware paths on subsets of Bloom's platforms and devices. None is a universal capability,
and API availability does not prove support for a requested codec/profile/pixel format.
The current public capability lists for Windows Media Foundation/D3D12 Video, NVIDIA NVENC/NVDEC,
Intel VPL, AMD AMF, Vulkan Video, and libva do not supply a cross-platform ProRes implementation.
They are candidates for codecs they actually enumerate, not a way to close the ProRes gap.

Vulkan Video currently standardizes selected H.264, H.265, AV1, and VP9 operations, subject to
physical-device capability queries; it does not standardize ProRes. MoltenVK exposes Metal and
IOSurface interop through Metal-object/external-memory extensions but does not currently expose the
Vulkan Video queue. Therefore Bloom may share one provider-neutral lease and Vulkan compute
conversion contract, but video coding and native surface transport remain qualified per platform:
Win32 external memory and synchronization on Windows, file-descriptor/DMA-BUF mechanisms on Linux,
and Metal/IOSurface mechanisms on macOS. No native handle or API name enters project semantics.

The first correct path is:

```text
compressed packet -> qualified decoder -> bounded CPU planes
                  -> explicit color/chroma/alpha conversion
                  -> Bloom process image
```

An accelerated path may return a move-only runtime `ExternalMediaSurfaceLease`. Its public
descriptor names the provider/device generation, semantic plane format, color/range/chroma facts,
extent, memory class, synchronization contract, and lifetime token. Native handles remain inside a
paired media/render interop adapter. The surface never becomes project truth, crosses device
generations, or enters a portable cache identity by handle value.

Each zero-copy pairing—such as a D3D11 decode surface imported by a Vulkan backend—needs a concrete
ownership, synchronization, layout, device-match, cancellation, teardown, and fallback contract.
If any step would require an unqualified conversion or routine UI-thread wait, Bloom copies through
the qualified CPU path. Transfer cost participates in planning, so “zero-copy capable” is not a
promise that it is always faster.

Hardware encode is likewise an execution provider, not a preset. A delivery preset can require a
named qualified hardware implementation, allow any semantically qualified implementation, require
software reference output, or make hardware ineligible. Hardware output is always reopened and QC'd.

## Apple ProRes Policy

### Exact Family Boundaries

The conventional ProRes family contains ProRes 422 Proxy, 422 LT, 422, 422 HQ, 4444, and 4444 XQ.
Apple documents 10-bit 4:2:2 for the 422 family; 4444 and 4444 XQ support 4:4:4:4 sources, up to
12-bit image channels, and an optional mathematically lossless alpha channel up to 16 bits. A
profile without alpha cannot inherit 4444 alpha behavior.

ProRes RAW and ProRes RAW HQ encode camera-sensor RAW samples and require RAW processing such as
white balance, demosaic, and camera-specific interpretation. They are separate asset, decode, color,
cache, and export contracts—not extra conventional ProRes profiles. No public general-purpose
ProRes RAW encoder SDK was verified in this research; a decode or RAW-processing API must not be
presented as encode capability.

SMPTE RDD 36:2022 publishes ProRes bitstream syntax and a decoding process. SMPTE RDD 44:2022
defines the constrained mapping of ProRes pictures into frame-wrapped MXF. MOV, RDD 44 MXF, and IMF
applications are different container/delivery presets even when they carry a ProRes bitstream.

### Authorization Gap

Apple's current authorized-products page states that Apple licenses and certifies ProRes for
specific products and workflows, explicitly describes FFmpeg and derivative implementations as
unauthorized, and directs unlisted implementers to the ProRes Program Office. Therefore:

- an FFmpeg ProRes encoder or decoder may be useful for development, differential tests, or an
  explicitly non-delivery community workflow, but it cannot establish Bloom's strict ProRes ingest,
  export, authorization, or delivery claim. FFmpeg's option to write an Apple-like vendor tag is
  bitstream metadata, never authorization evidence, and Bloom must not use it to imply Apple
  provenance;
- on macOS, AVFoundation/Core Media expose conventional ProRes codec identifiers and VideoToolbox
  provides encode/decode services. Codec constants do not prove that an encoder exists on the
  current host, and a requested hardware path does not prove that hardware was selected; Bloom must
  enumerate the encoder and record the session's actual hardware-use property. This remains the
  preferred native implementation candidate, not proof that Bloom itself or every produced preset
  is Apple-authorized. Bloom must obtain the applicable Apple program answer and pass its own
  fixtures;
- Apple currently lists MainConcept's ProRes Decoder SDK, and MainConcept documents Windows,
  macOS, and Linux decoding plus RDD 36/RDD 44 support. It is a serious cross-platform ingest
  candidate, not an encode solution and not accepted until commercial terms and Bloom fixtures pass;
- Apple lists products with ProRes encoding such as nablet mediaEngine and Vidispine ProRes Encode,
  and nablet publicly advertises SDK/CLI/REST/container integration surfaces. The evidence reviewed
  still does not establish terms for a redistributable offline Bloom integration or transfer that
  product's certification to Bloom. OEM, profile/alpha, platform, deployment, authorization-scope,
  and pricing questions require direct vendor and Apple confirmation; and
- until an authorized and Bloom-qualified encode provider exists on Linux and Windows, strict
  ProRes delivery is `Unavailable` there. The honest cross-platform fallback is a verified image
  sequence plus BWF/BW64 audio and an explicit external delivery handoff. It is not labeled an
  equivalent ProRes export.

No `Apple ProRes`, authorized, certified, broadcast-approved, or studio-approved badge appears from
codec tag recognition or successful playback alone. The capability report carries a source URL or
contract reference, verification date, named product/workflow, direction, platforms, and expiry or
review date for every such claim.

## Licensing, Patents, And Distribution

Bloom's Apache-2.0 license covers original Bloom code only.

- FFmpeg is LGPL-2.1-or-later by default; enabling GPL parts changes the FFmpeg build to GPL, and an
  `--enable-nonfree` configuration is not redistributable under FFmpeg's terms. A community Bloom
  profile, if accepted, uses a reviewed minimal shared-library build with neither GPL nor nonfree
  parts, dynamically links it, ships exact corresponding source/build configuration/notices, and
  audits every enabled external library. Running it in another process does not erase distribution
  obligations.
- An open-source codec implementation does not grant codec patent, trademark, certification, or
  delivery-recipient rights. Each shipped encode/decode capability receives counsel review for the
  distribution territories and business model. Hardware vendor SDK documentation likewise may
  disclaim codec patent licenses.
- Apple frameworks are an OS-provider candidate governed by the Apple SDK and platform terms; Bloom
  does not redistribute them. ProRes program authorization, certification, and wordmark use remain
  separate questions for Apple.
- Commercial providers remain optional packages unless redistribution terms permit bundling. Their
  license, activation, telemetry/network behavior, offline availability, source/SBOM visibility,
  security response, and end-user deployment must qualify before integration.
- Dependency lock, prefix provenance, actual shipped-file inventory, SPDX SBOM, notices, source
  offer, and vulnerability review follow [`dependency-intake.md`](dependency-intake.md). A provider
  process is still part of Bloom's shipped distribution graph.

These are engineering gates, not legal advice. Counsel and the named licensors remain the authority.

## Untrusted-Media Containment

- Probe and decode workers start with no ambient project access, no network, a minimal environment,
  and only explicitly transferred source/staging capabilities.
- OS-specific sandboxes implement one semantic policy: read the admitted source, write only the
  private stage when authorized, allocate within caps, and use only admitted GPU/device services.
- Every request has checked byte, pixel, sample, stream, nesting, metadata, memory, output, CPU, and
  monotonic-time budgets. Integer arithmetic is checked before allocation or pointer movement.
- Workers are disposable. Timeout, crash, protocol violation, memory breach, or cancellation kills
  the request generation, publishes no partial product, and cannot poison a prior cache entry.
- Repeated crashes quarantine the source/provider tuple and produce one stable diagnostic rather
  than an automatic crash loop.
- Host validation treats worker strings, counts, offsets, handles, capabilities, and success claims
  as untrusted. IPC slabs are generation-scoped and never reused while either side may still access
  them.
- Staged outputs are reopened under no-follow/regular-file rules, rechecked against their lease, and
  validated independently of writer state before publication.
- Pinned dependency upgrades track upstream security advisories and rerun malformed corpora under
  sanitizers. FFmpeg's published vulnerability history is evidence for isolation, not a claim that
  another provider is safe by default.

## Deterministic Qualification And QC

Each provider profile pins source/build/configuration, platform SDK, compiler/ABI, enabled codecs,
hardware/driver scope, worker protocol, fixture-set digest, and all external qualification evidence.

Every staged media artifact produces one bounded immutable `MediaQcEvidenceV1`. It binds the
artifact SHA-256 and byte size; approved preset and preservation-report digests; ordered pipeline,
provider, dependency, conversion, and execution identities; determinism or tolerance-profile
identity; reopen-reader identity; structural, decoded-video, audio, timing, color/HDR, timecode, and
metadata results; complete-versus-sampled coverage; independent or same-provider reader status; any
recipient QC tool/profile/version and attributed authority evidence; and the terminal result. A
same-provider reopen is valuable verification but is never labeled independent QC. An exit status,
codec tag, or unstructured log cannot populate a passing record, and a partial record cannot be
published as success. When an Export or Delivery profile requires full coverage, sampled or missing
coverage blocks success rather than weakening the profile after execution.

Tests include:

- exact probe vectors for container/track order, unknown metadata, contradictory signaling, edit
  lists, negative starts, missing timestamps, variable frame rate, B-frame reorder, discontinuities,
  interlace, odd dimensions, non-square pixels, every admitted chroma/bit depth/range, alpha, HDR,
  timecode, audio layout, priming, and padding;
- sequential decode, cold random seek, repeated seek, reverse/scrub request sequences, reduced
  resolution, EOF/drain, cancellation, and source-change behavior;
- exact CPU-plane digests for lossless/reference fixtures and frozen numeric comparisons for lossy
  decoders, including range, chroma siting, alpha, negative/HDR, and round-trip color cases;
- output frame count, PTS/DTS/duration/edit mapping, track and atom/box/KLV order, random access,
  codec headers, profile/level, color/HDR signaling, timecode, audio layout/loudness, metadata
  allowlist, and absence of host paths/timestamps/usernames;
- decode-after-encode semantic comparison under the preset's exact rules, plus an independent
  implementation or recipient QC when the profile requires it;
- official conformance/reference streams where terms permit, independently generated fixtures,
  malformed/truncated/adversarial corpora, fuzzing, cross-provider differential diagnostics, and
  large-boundary arithmetic cases;
- worker crash, hang, protocol corruption, allocation failure, disk full, short write, cancellation
  at every phase, driver reset, device removal, stale generation, and shutdown; and
- Linux, macOS, and Windows performance envelopes for probe latency, seek latency, sustained decode,
  encode throughput, queue depth, memory, transfers, and cancellation. Performance qualification
  never relaxes semantic qualification.

For ProRes, RDD 36 reference material can help test the documented decoding process, but it cannot
by itself establish Apple product authorization or a recipient delivery acceptance. Those evidence
sets stay separately named.

## Per-Codec Research Dossiers

No codec enters the provider registry from a generic library feature list. Its checked-in dossier
must name the governing bitstream and container-mapping specifications; profile/level/tier and
sample-format matrix; alpha/auxiliary-plane behavior; timing, reorder, random-access, interlace, and
error-recovery rules; color/HDR and mastering metadata; audio delay/layout/loudness behavior where
applicable; reference/conformance streams; qualified software and hardware providers; license,
patent, trademark, and redistribution disposition; security history and limits; independent readers
or QC tools; and the exact Bloom ingest/export/delivery claims proposed.

The first dossier queue is:

| Family | Questions that block a Bloom claim |
| --- | --- |
| Conventional ProRes | six ordinary profiles, 4444/XQ alpha, RDD 36 essence, MOV versus RDD 44 MXF, provider authorization, independent decode, named recipient QC |
| DNxHD/DNxHR / VC-3 | SMPTE ST 2019 subset versus Avid-branded profiles, licensed SDK versus other implementation, alpha/profile behavior, MOV/MXF mapping, Avid interchange fixtures |
| AVC/H.264 and HEVC/H.265 | exact profile/level/tier, 4:2:0/4:2:2/4:4:4 and depth, reorder/random access, interlace, HDR static/dynamic signaling, hardware variance, patent/territory disposition, recipient preset |
| AV1 and VP9 | bitstream and container bindings, chroma/depth/HDR subset, hardware generation matrix, encoder quality/performance, conformance material, actual playback/delivery recipient |
| PCM BWF/RF64/BW64 | sample packing, size transition, time reference, UMID, loudness and XML chunks, channel roles, unknown-chunk preservation, exact round trip |
| AAC, Opus, and FLAC | profile/mapping, encoder delay and padding, timestamp and gapless behavior, channel mapping, loudness metadata, patents/licenses, container-specific interoperability |
| ProRes RAW and other camera RAW | authorized SDK and camera plug-in model, sensor metadata, demosaic/white balance/exposure semantics, color transform identity, cache invalidation, supported platforms, encode availability |
| JPEG 2000/JPEG XS and archival candidates such as FFV1 | exact production/archive profiles, container mapping, lossless claim, hardware/provider availability, patents/licenses, conformance corpus, recipient acceptance |

SMPTE ST 2019 publishes the VC-3 data format and decoding process, while Avid separately licenses
DNx SDK capabilities. AOMedia publishes AV1 specifications and a patent-license policy but notes that
its sample streams are not a comprehensive compliance suite. These distinctions follow the same
rule as ProRes: a public specification, a software implementation, a vendor license, and a delivery
acceptance are different evidence.

## Delivery Priorities

Priority is by professional workflow value and qualification risk, not the number of formats a
library advertises:

| Priority | Scope | Initial claim boundary |
| --- | --- | --- |
| P0 | OpenEXR/PNG and then DPX/TIFF/JPEG stills and sequences; PCM WAVE/BWF, then RF64/BW64 | VFX interchange, exact sequence timing, explicit color/alpha/channels, metadata preservation; closed subsets per format |
| P1 | MOV and MXF ingest; qualified conventional ProRes, DNxHD/DNxHR, AVC/H.264, HEVC/H.265, PCM and common production audio | ingest and conform are qualified separately per codec/profile; licensing and authorization may make some providers optional |
| P1 | proxy generation with a deliberately chosen intra-frame or broadly decodable profile | visible editorial-preview semantics; never a final-render substitute |
| P2 | MP4/MOV audience delivery using AVC/HEVC/AV1 and AAC or other selected audio | exact versioned platform/recipient presets, patent/license gate, hardware/software qualification, reopen and QC |
| P2 | strict conventional ProRes MOV/MXF export | only where an Apple-authorized and Bloom-qualified provider exists; image-sequence plus BWF/BW64 handoff elsewhere |
| P3 | ProRes RAW and other camera RAW, captions/subtitles, IMF/AS-11, DCP, immersive audio, live/network media | dedicated domain contracts; never inferred from general demux/decode support |

P0 implementation should follow the first proof's PNG/EXR output because it exercises VFX-native
sequence identity and audio preservation without committing Bloom to a consumer-delivery matrix.
The exact codecs in later priorities remain candidates until dependency, license, provider, and
fixture qualification is accepted.

## Coherent Implementation Batches

1. **Media contract kernel** — provider-neutral descriptors, capability/qualification records,
   registry rules, stable diagnostics, fake providers, and hostile descriptor tests.
2. **Isolated probe and index** — bounded worker protocol, secure source capability, probe/index
   tasks, source identity, cancellation, quarantine, and timeline mapping fixtures.
3. **CPU decode products** — immutable video planes and audio blocks, exact seek/drain rules,
   interpreted conversion into process frames, and cache identities.
4. **VFX sequence and audio foundation** — qualified still-sequence ingest plus PCM BWF/BW64,
   metadata/timecode preservation, waveforms, and deterministic proxy manifests.
5. **General media provider intake** — minimal dynamically linked LGPL FFmpeg candidate and any
   commercial decoder candidates, each behind the same worker and independently qualified.
6. **Time-based export kernel** — versioned preset/report/digest, bounded frame/audio back-pressure,
   encode/mux worker, reopen verification, QC evidence, and atomic publication.
7. **Hardware lane** — runtime capability probes, external-surface lease, one measured zero-copy
   pairing per target platform, CPU parity, driver-loss recovery, and hardware export qualification.
8. **ProRes qualification lane** — Apple program/vendor answers, macOS native provider fixtures,
   authorized cross-platform provider evaluation, MOV and RDD 44 MXF as separate presets, alpha and
   RAW kept separate, and recipient QC profiles.

No batch creates a broad “supports FFmpeg” claim. Each closes a small provider/profile matrix and
keeps unsupported tuples visible.

### Dependency-Free Contract Slices

Once the roadmap's first-proof gate admits media implementation, and before any codec or container
dependency is admitted, Bloom can land the following C++20, Qt-free `src/media` slices:

1. bounded immutable descriptor values, `MediaCapabilityKeyV1`, `ProviderExecutionKeyV1`,
   `QualificationEvidenceV1`, `PipelineQualificationV1`, `MediaDeterminismV1`, limits, validators,
   stable diagnostics, canonical encodings, and SHA-256 identities;
2. an immutable provider-generation registry with exact matching, deterministic selection,
   structured missing-capability reasons, and no wildcard or transitive qualification;
3. fake providers and hostile fixtures proving role/purpose separation, software/hardware
   independence, authority expiry, pipeline composition, and frozen substitution rules;
4. bounded versioned worker-message framing and state machines over an in-memory fake transport,
   covering cancellation, stale generations, malformed messages, crash, hang, and shutdown without
   promising a public plug-in SDK;
5. a canonical image-sequence manifest for ordered members, duplicate/gap diagnostics, rational
   time, interpretation, source fingerprints, and explicit gap policy; and
6. `MediaQcEvidenceV1` construction and validation, including rejection of partial, self-described
   independent, or mismatched artifact/preset evidence.

These slices create no codec-support claim and introduce no third-party binary. Real process launch,
sandbox adapters, demuxers, codecs, muxers, native GPU surfaces, and commercial SDKs follow the
dependency-intake and qualification gates. This sequence does not move media implementation ahead
of the current first-proof deferral.

## Decisions Still Requiring External Authority

Before strict ProRes work is scheduled, obtain written answers for:

1. Whether and under what terms Bloom may use Apple platform encoders/decoders and describe the
   resulting Bloom/macOS workflow as authorized, including wordmark and test requirements.
2. Whether Apple offers or approves an embeddable conventional ProRes encoder path for Bloom on
   Windows and Linux, and the product/workflow/platform scope of that approval.
3. Whether nablet, Vidispine, or another Apple-listed provider offers an offline, redistributable,
   local C/C++ or process SDK for all target platforms; exact encode/decode profiles, alpha, MOV/MXF,
   pricing, activation, support, and whether Bloom receives its own authorization listing.
4. MainConcept ProRes Decoder SDK commercial redistribution, offline activation, architectures,
   security/update policy, exact alpha/profile/output-plane behavior, and authorization inheritance.
5. Counsel's review of FFmpeg LGPL configuration and enabled transitives, ProRes and other codec
   patent/trademark exposure, OS SDK terms, commercial provider packaging, and target territories.
6. The first real studio/broadcaster/streamer delivery specifications and approved QC tools. Bloom
   qualifies named presets against real recipients rather than inventing a generic “broadcast” bar.

## Sources And Verification

Verified on 2026-08-25. These are primary specifications, project documentation, platform/vendor
documentation, and vendor product statements. Versioned candidates must be rechecked during intake.

### Verified Facts

- [Apple ProRes white paper, April 2022](https://www.apple.com/final-cut-pro/docs/Apple_ProRes.pdf)
  defines the conventional family and image/alpha depth behavior.
- [Apple ProRes RAW white paper, May 2023](https://www.apple.com/final-cut-pro/docs/Apple_ProRes_RAW.pdf)
  distinguishes sensor RAW, its two compression levels, and the downstream RAW processing pipeline.
- [Apple authorized ProRes products](https://support.apple.com/en-us/118584) states that licensing
  and certification are product/workflow-specific, names FFmpeg derivatives as unauthorized, lists
  current products, and provides the ProRes Program Office contact.
- [Apple AVFoundation ProRes codec identifiers](https://developer.apple.com/documentation/avfoundation/avvideocodectype/prores4444)
  and [Core Media video codec constants](https://developer.apple.com/documentation/coremedia/video-codec-constants)
  expose conventional and RAW ProRes type identifiers on Apple platforms;
  [VideoToolbox](https://developer.apple.com/documentation/videotoolbox) exposes low-level hardware
  encode/decode services whose actual capabilities still require runtime queries. Its
  [encoder-list keys](https://developer.apple.com/documentation/videotoolbox/video-encoder-list-keys)
  and [actual hardware-use property](https://developer.apple.com/documentation/videotoolbox/kvtcompressionpropertykey_usinghardwareacceleratedvideoencoder)
  keep availability separate from hardware selection.
- [SMPTE RDD 36:2022](https://pub.smpte.org/doc/rdd36/20220909-pub/rdd36-2022.pdf) specifies the
  ProRes bitstream syntax and decoding process;
  [SMPTE RDD 44:2022](https://pub.smpte.org/pub/rdd44/rdd44-2022.pdf) specifies its constrained,
  frame-wrapped MXF mapping. An RDD is a disclosure document, not evidence that Bloom is licensed or
  certified by Apple.
- [MainConcept ProRes Decoder SDK](https://www.mainconcept.com/prores) describes Apple-approved
  cross-platform decode for MOV/RDD-36 and MXF/RDD-44; Apple's authorized-products page lists that
  decoder SDK. No public MainConcept ProRes encoder SDK was verified in this review.
- [nablet mediaEngine release notes](https://support.nablet.com/hc/en-us/articles/24011027768340-mediaEngine-v3-0-Release-Notes)
  claim Apple-certified ProRes encoding, and Apple lists mediaEngine. Public evidence reviewed here
  does not answer Bloom OEM/redistribution or offline-local SDK terms.
- [FFmpeg codec documentation](https://ffmpeg.org/ffmpeg-codecs.html#ProRes) documents its ProRes
  encoders and profile options. [FFmpeg licensing](https://ffmpeg.org/legal.html) states LGPL-2.1+
  default terms, GPL/nonfree configuration consequences, dynamic-linking guidance, and separate
  patent uncertainty. [FFmpeg security](https://www.ffmpeg.org/security.html) publishes fixed
  vulnerabilities; [FATE](https://www.ffmpeg.org/fate.html) is its regression framework.
- [FFmpeg demuxing API](https://ffmpeg.org/doxygen/7.0/group__lavf__decoding.html) and
  [codec send/receive API](https://www.ffmpeg.org/doxygen/8.0/avcodec_8h.html) provide candidate
  implementation primitives; they do not define Bloom's frame-accuracy or preservation semantics.
- [SMPTE ST 2019-1:2016](https://pub.smpte.org/pub/st2019-1/st2019-1-2016.pdf) specifies the VC-3
  data format and decoding process, while [Avid's developer program](https://developer.avid.com/)
  treats commercial DNxHD/DNxHR codec capability as a separately licensed SDK option.
- [AOMedia's AV1 specification index](https://aomedia.org/specifications/av1/) publishes the
  bitstream and ISOBMFF binding, while its [product/test guidance](https://aomedia.org/products/)
  says its own sample streams are not a comprehensive compliance set.
- [QuickTime File Format atoms](https://developer.apple.com/documentation/quicktime-file-format/atoms),
  [SMPTE ST 377-1:2019 MXF](https://pub.smpte.org/latest/st377-1/st377-1-2019.pdf),
  [EBU Tech 3285 BWF](https://tech.ebu.ch/publications/tech3285), and
  [ITU-R BS.2088-2 BW64](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2088-2-202511-I%21%21TOC-HTM-E.htm)
  are the starting container/audio contracts for their declared subsets.
- [SMPTE ST 2086:2018](https://pub.smpte.org/pub/st2086/st2086-2018.pdf) defines mastering-display
  color-volume metadata; [SMPTE ST 2094-1:2016](https://pub.smpte.org/pub/st2094-1/st2094-1-2016.pdf)
  defines the common dynamic-metadata framework; and
  [CTA-861.3-A](https://www.cta.tech/standards/cta-8613-a/) defines HDR static-metadata extensions
  for the interface mappings in its scope. Each delivery container and bitstream still needs its own
  declared carriage rules.
- [ITU-T H.273](https://www.itu.int/rec/T-REC-H.273) defines coding-independent video signal code
  points. [SMPTE's timecode overview](https://www.smpte.org/blog/understanding-standards-time-code)
  identifies the ST 12 family and its rate/drop-frame scope.
- Hardware candidates expose different surfaces and support matrices:
  [Apple VideoToolbox](https://developer.apple.com/documentation/videotoolbox),
  [Microsoft D3D11/Media Foundation decode](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation),
  [VA-API/libva](https://github.com/intel/libva),
  [Intel VPL](https://www.intel.com/content/www/us/en/developer/tools/vpl/overview.html),
  [NVIDIA Video Codec SDK](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/index.html),
  and [AMD AMF](https://gpuopen.com/advanced-media-framework/). AMD explicitly states that AMF does
  not sublicense codec-standard IP rights, reinforcing the separate legal gate.
- [Vulkan Video](https://docs.vulkan.org/spec/latest/chapters/videocoding.html) defines the current
  codec-operation framework; [Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html)
  defines platform handle and synchronization families. The current
  [MoltenVK runtime feature list](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
  includes Metal external-memory/object interop but not the Vulkan Video queue, while
  [Core Video's Metal texture cache](https://developer.apple.com/documentation/CoreVideo/cvmetaltexturecache-q3j)
  exposes the native Apple image-buffer-to-Metal candidate path. All remain runtime-qualified
  implementation mechanisms rather than portable media semantics.
- The currently reviewed
  [Windows Media Foundation format list](https://learn.microsoft.com/en-us/windows/win32/medfound/supported-media-formats-in-media-foundation),
  [D3D12 Video codec enum](https://learn.microsoft.com/en-us/windows/win32/api/d3d12video/ne-d3d12video-d3d12_video_encoder_codec),
  [NVIDIA Video Codec SDK](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/ffmpeg-with-nvidia-gpu/index.html),
  [Intel VPL hardware reference](https://www.intel.com/content/www/us/en/docs/onevpl/developer-reference-media-intel-hardware/1-1/overview.html),
  [AMD AMF encode API](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_Video_Encode_API.md),
  [Vulkan Video specification](https://docs.vulkan.org/spec/latest/chapters/videocoding.html), and
  [libva API](https://github.com/intel/libva/blob/master/va/va.h) do not enumerate ProRes as a
  common accelerated codec. This negative result is dated and must be rechecked as those APIs evolve.

### Architecture Recommendations, Not External Facts

The provider protocol, isolation policy, qualification vocabulary, CPU reference contract,
zero-copy lease, staged verification pipeline, delivery priorities, and implementation batches in
this document are Bloom proposals derived from the verified constraints. They are not claims made
by Apple, FFmpeg, SMPTE, EBU, ITU, or any SDK vendor.

## Related Contracts

- [`frame-output.md`](frame-output.md)
- [`color-management.md`](color-management.md)
- [`task-system.md`](task-system.md)
- [`gpu-backend.md`](gpu-backend.md)
- [`module-system.md`](module-system.md)
- [`dependency-intake.md`](dependency-intake.md)
- [`platform-support.md`](platform-support.md)
- [`../standards/strategy.md`](../standards/strategy.md)
- [ADR 0020](../decisions/0020-qualified-media-codec-providers.md)

### Video Read Implementation (MEDIA-3)

Linux uses FFmpeg 8.1.2 exclusively inside `bloom-media-worker`. Only
`apps/bloom-media-worker/ffmpeg_provider.cpp` includes FFmpeg headers. The desktop links the
provider-neutral protocol and client; it loads no libav library. The worker installs under
`libexec/bloom/media/ffmpeg-v1`, with its shared closure in `lib/` and relative loader paths.
macOS and Windows currently return typed `Unavailable`; their native providers remain pending.

Protocol 1, schema 2 adds bounded stream metadata, keyframe indices and planar float audio blocks.
Probe retains codec/profile identifiers, exact rational timebase and rate, dimensions, original
pixel-format spelling, H.273 color tags, channel layout, sample rate, duration and timecode.
`DemuxIndex` records stream IDs and exact key-packet PTS/DTS. Software video verifies constant
packet-presentation cadence, then seeks backward to a keyframe and decodes forward until the
exact requested CFR presentation timestamp. A retained decoder keeps its presentation position:
a later request whose nearest preceding key packet is at or before that position decodes forward
without seeking (for example every second frame of a 50p source in a 25p composition), and only a
backward request or one past a later key packet seeks. A missing exact timestamp, unsupported layout or
unavailable rate is a typed refusal.

Footage timing and layout (issue #367), all applied by the worker as source interpretation so the
rest of Bloom sees display geometry:

- Variable frame rate. When packet presentation times do not follow the nominal cadence, frame N
  means nominal slot N: the frame presented at `start + N / rate` (the last packet at or before
  it). Probe reports the number of nominal slots the duration covers as the frame count, so
  composition time maps to source time by timestamp rather than by packet count. Duplicate or
  missing presentation timestamps remain typed refusals.
- Seeking always lands at the decode time of the key packet presented at or before the target, so
  B-frame and open-GOP streams (leading pictures before a CRA/I picture) decode the target from the
  previous GOP rather than refusing it.
- Display orientation. A display matrix with a right-angle rotation and/or horizontal mirror is
  applied to the decoded planes after full-resolution chroma reconstruction; probe reports the
  displayed width and height. Non-right angles are not applied.
- Sample aspect ratio. A non-square SAR (container value first, then the bitstream's) is resampled
  horizontally to square pixels with the same bit-exact bilinear `sws` pass; probe reports the
  square-pixel width.
- Interlace. Interlaced frames are deinterlaced with FFmpeg's `bwdif` filter (libavfilter, enabled
  in the intake with `buffer` and `buffersink`), field-aware and honoring each frame's field order
  (`parity=auto`, one output frame per frame). The retained decoder feeds it the previous decoded
  frame, the requested frame and the next one, which it keeps as a lookahead for the next forward
  request; at a seek point or the stream end bwdif uses only the neighbors that exist. Hardware
  frames that are interlaced fall back to the software decoder.
- RGB-coded streams (the intake's TIFF-in-MOV) decode to one packed RGBA16 plane with their
  identity matrix tag and are interpreted only through a config input space. The TIFF decoder runs
  without `AV_EF_EXPLODE`, which otherwise rejects descriptive tags its own encoder writes.

Document 1.25 records each video stream's field order and timecode type (drop-frame or non-drop),
shown in the Assets tooltip and in the Video node's Properties, and the Video node can choose which
of the asset's audio streams it plays (Properties, Audio Stream). Preview and export decode and key
audio buffers by asset and stream.

Audio requests name a starting sample and exact count; decoding from
the beginning preserves codec priming and refuses timestamp gaps or overlaps. A retained worker
keeps a separate audio reader and its decode position: a block that starts where the previous
block ended continues the same pass, including samples already decoded past that end, so N
sequential blocks decode the stream once. Any other start reopens the reader and decodes from the
beginning again. A retained worker is relaunched after five minutes or 65,536 calls. Audio sample zero
uses the first video presentation timestamp, including for containers with a nonzero origin.

All four read roles run behind the existing process supervisor, cancellation, bounded queue and
watchdog. Limits include 1 TiB source files (4 TiB per export), 256 streams, 16,777,216 index
entries (over 77 hours at 60 fps), 16,384 per image dimension, 67,108,864 pixels (8192 x 8192: 8K
full aperture and 16K x 4K panoramas), 512 MiB of CPU planes, and 65,536 audio samples per block.
These are protocol and representation ceilings, not memory policy: frames are admitted by the
caller's pixel budget, and each worker's address space is sized for its stream
(`provider::workerAddressSpaceBytes`). A retained worker launched for a probe or a smaller stream
is relaunched with the larger limit before it decodes a bigger one; export workers are sized from
the export extent. An 8K UHD ProRes fixture probes, decodes, renders through the evaluator and
exports with reopen QC.

The decoder no longer reads every packet before the first frame (issue #352). A container that
declares a constant cadence (its sample count equals its duration in frame periods) and carries an
in-memory demuxer index (MOV/MP4) is addressed provisionally: frame N is start + N / rate, random
access uses the index's keyframes (all-intra streams seek straight to the frame), and a seek by
decode time that lands after the target on a reordered stream is retried earlier with a doubling
distance. Any decode that contradicts the claim (a missing presentation or an early end) falls
back to the whole-file packet scan, which also remains the path for containers without an index
and for streams whose metadata does not declare a constant cadence. On a 1,200-frame fixture the
worker reads about 6% of the file for four random-access frames.
The preview audio buffer also obeys the existing audio sample budget. Malformed input, changed
content, oversize products, worker crashes and timeouts remain typed failures. A failed worker
request cannot publish a partial product; a later request starts a fresh worker.

Software decode declares `DecodedSemanticExact` for the intake's admitted codec paths. It is
built with FFmpeg's x86 SIMD and is threaded: long-GOP codecs use frame threads and intra-only
codecs (ProRes, DNxHD, MJPEG, TIFF) use slice threads so one random-access frame never waits on a
pipeline of following frames. The thread count is the smaller of the machine's cores, eight, and
what fits four in-flight 16-bit 4:4:4 frames per thread into half of the worker's address space
(read from its own limit; two threads at 8K UHD). glibc malloc arenas are limited to four for the same reason. Frame
and slice threading are bit-exact for these decoders, so threading does not change the
determinism claim.

Preview hardware decode (issue #366) is a separate, decode-only worker generation started with
`--vaapi-decode` and identified by its own execution key: `Hardware`, device
`vaapi-render-node-discovered`, H.264/HEVC `VideoDecode` only, `PreviewQualified`, and
`NoDeterminismClaim`. The worker discovers `/dev/dri/renderD*` nodes in numeric order instead of
assuming one; a node whose driver cannot decode the stream makes FFmpeg return software frames,
which the worker treats as a miss and moves to the next node. The first hardware frame of every
worker generation is decoded again by the software reference on an independent reader of the same
immutable input and must match byte for byte (the documented tolerance is zero); downloaded
limited-range NV12 is de-interleaved into the same YUV420P8 product as software, other surfaces
use the same 16-bit conversion. Any typed hardware refusal or parity failure retires hardware for
that source and decodes the software reference; the frame's `FrameDecodeProvenance` then reads
`software (fallback: <reason>)`. Only retained (interactive preview) revisions may request
hardware. Revalidated requests — final, still, sequence, video and headless rendering — always
decode in software, decoded-cache keys and GPU raw-upload identities separate hardware frames from
software ones. The route is preferred automatically only on hosts with at most four hardware
threads, where it measured faster than threaded software decode; `BLOOM_VIDEO_HARDWARE_DECODE=1`
prefers it on any host with a render node and `0` disables it. The older export-side
VA-API execution key keeps its Development/Incomplete decode declarations, which are not
selectable. Hardware provides acceleration only; it never changes project semantics.
Each successful video source selection, on the CPU and GPU routes alike, publishes a
`VideoSourceReport` (asset digest, decode route, assumed-color note) on the video source context;
`OperationCache::videoSourceReports()` reads the 32 most recent. The window status bar's video cell
shows, for the active composition's Video sources, the latest route ("Video: software", "vaapi" or
"software (fallback: <reason>)") followed by any assumed-color note, with one line per source in
its tooltip. The report is status beside evaluation, never an evaluation diagnostic, so frames
that carry it stay cacheable in RAM preview.
FFmpeg ProRes evidence explicitly records `apple_authorized=false` and no delivery qualification.
Assets expose this exact note:

> Decoded by FFmpeg; not an Apple-authorized ProRes implementation

`VideoDecodeSession` serializes each asset's requests through one retained `MediaWorkerPool`
worker per route. Identical frame requests share one decode; other concurrent requests wait,
cancellably, for the decoder rather than receiving `Busy`, and the 30 second watchdog bounds each
worker exchange rather than the queue. The evaluator admits retained sessions by memory: each is
charged a conservative worker estimate (64 MiB plus 24 frames of 16-bit 4:4:4 planes, capped at
the stream's worker address space) against a session budget of a quarter of physical memory, keeping
at least two so two sources can alternate and at most the eight files the source-snapshot quota
can retain. Least-recently-used sessions beyond that are retired. `DecodedVideoCache` has a
separate LRU byte budget and keys frames by content digest, stream, frame index,
interpretation, color identity and decode route. Its target is the `playback/decoded-video-cache-bytes`
setting, or automatically one sixteenth of physical memory between 256 MiB and 4 GiB. It is a
reusable pool on the process memory ledger, so an observed OS limit constrains it with the other
reusable caches. A zero budget disables retention.

Measured on 2026-10-23 with locally generated files (not committed), 200 frames and 40 seeded
random scrubs through `bloom_media_video_decode_benchmark` on a 16-thread x86-64 Linux host with
an AMD integrated GPU for VA-API. Mean milliseconds per frame request, before → after, software
route:

| Fixture | Cold first frame | Playback (+1) | 50p in 25p (+2) | Random scrub |
| --- | --- | --- | --- | --- |
| H.264 1080p50, GOP 100, B-frames | 65.6 → 33.8 | 48.3 → 16.9 | 230.4 → 17.7 | 255.6 → 51.7 |
| H.264 2160p25, GOP 50, B-frames | 218.8 → 77.2 | 179.9 → 49.6 | 584.4 → 51.7 | 572.0 → 144.5 |
| HEVC Main10 1080p50, closed GOP 100 | 246.9 → 46.0 | 269.4 → 31.9 | 636.9 → 32.7 | 548.6 → 109.1 |
| HEVC Main 1080p50, closed GOP 100 | 60.4 → 28.0 | 46.0 → 17.2 | 264.9 → 18.3 | 272.7 → 87.6 |
| ProRes 422 HQ 1080p25 (intra) | 226.9 → 36.1 | 327.6 → 32.4 | 326.2 → 32.6 | 264.6 → 32.4 |

Before, closed-GOP HEVC failed one scrub in 40 and open-GOP HEVC four of 100 stride requests,
because MOV seeks by decode time landed on a keyframe presented after the target; after, none
fail. VA-API on the same host was slower than threaded software (H.264 1080p playback 28.1,
scrub 122.5; 2160p playback 81.2, scrub 285.6). Restricted to four threads it matched or beat
software (2160p playback 81.4 vs 98.4), and to two threads its scrub was 260.9 vs 415.3, which is
the basis of the automatic preference. `--parity` decoded 200 frames of each H.264 and HEVC
fixture through both routes with zero byte mismatches. On this AMD driver, hardware HEVC after a
random seek returned a frame presented after the target; that is a typed refusal and the session
fell back to software as designed.

Plane digests are computed once in the worker and verified once when the host decodes the
message, hashing the planes of large products concurrently. Later consumers of the immutable
product (the decoded cache and color conversion) check structure and budget only, so a 16-bit
4:4:4 1080p frame is no longer hashed seven times. File content is rehashed before a cache hit, so edits cannot reuse stale decoded pixels.
Thumbnail and waveform/audio buffers reuse the existing bounded preview storage after the same
content check; scrubbing does not decode the complete audio clip again.
No operation-cache or memory-ledger implementation changes are required.

Color conversion preserves stream tags when a decoder omits them from its frame. The provider
copies left-sited YUV420P8 directly; any other layout or chroma siting is expanded to bounded
YUVA444P16 by bit-exact bilinear chroma reconstruction at the frame's H.273 chroma location
(unspecified 4:2:0 siting is the H.273 default, left). The host reconstructs YUV420P8 chroma with
the same left-sited bilinear filter rather than nearest neighbor. `src/media/video` performs the
matrix/range step and `src/color` applies the resolved OCIO input-id-to-working-space transform on
straight RGB before alpha is restored; the GPU route uploads the same host-prepared pre-OCIO RGB
and runs that transform as a GPU OCIO effect.

Issue #365 interpretation rules:

- Untagged SDR video is not refused (owner decision, 2026-10-24). Automatic resolution fills each
  missing tag by the FFmpeg/NLE convention and leaves present tags unchanged: pictures 1280 pixels
  wide or taller than 576 lines are Rec.709 (primaries 1, matrix 1); smaller pictures are Rec.601,
  BT.470BG (5) at 25 or 50 fps and SMPTE 170M (6) otherwise, with the BT.709-curve transfer
  (SMPTE 170M, 6). A missing transfer on HD is BT.709 (1), a missing range is limited (video), and
  a missing matrix on a BT.2020-primaries or PQ/HLG stream is BT.2020 non-constant luminance.
- Ordinary SDR video, tagged or assumed (a Rec.709 or Rec.601 matrix with the BT.709/SMPTE 170M
  transfer), resolves in Auto to the config's sRGB-texture space
  ([ADR 0032](../decisions/0032-sdr-video-colour-default.md)): the YUV conversion applies the
  stream's matrix (Rec.601 for SD by the size convention) and range, and the resulting R'G'B' is
  interpreted with the sRGB display transfer, as players and Bloom's 8-bit stills are shown. Before
  document 1.28 these streams resolved to the config's Rec.709 video space, which in Bloom Neutral
  was scene-linear, so the encoded signal was shown pale. The Rec.709 camera inverse OETF
  (`Rec.709 Camera (Bloom)`), Rec.2100 PQ/HLG and log spaces stay explicit Input Color choices.
  The assumption is shown as the note "Color tags missing — assumed Rec.709 with the sRGB
  transfer. Change in Assets → Input Color" (Rec.601 for SD; "assumed Rec.2020" without the
  transfer clause for a wide-gamut stream) in the asset's color note, an Info mark on its Assets
  row, and "Auto (assumed: …)" in the input color display. The filled tags and which of them were
  assumed (`assumed-colour-v2:<filled>:p,t,m,r`; v2 since the SDR default changed) are part of the
  video transform and cache identity and of the thumbnail key. Streams the assumption cannot make SDR
  Rec.709/601 (Rec.2020 without PQ/HLG, BT.2020 constant luminance) keep their named refusal.
- An explicit input color space is the artist's interpretation and overrides missing tags. The
  space supplies primaries and transfer; the stream supplies its YUV matrix and range. When those
  are untagged too, the matrix follows the chosen space's gamut family (BT.2020 non-constant
  luminance for a Rec.2020/Rec.2100 space, otherwise the size convention's BT.709 or BT.601) and
  the range is video (limited). An explicit choice carries no assumption note.
- Supported YUV matrices are BT.709, BT.601 (470BG/SMPTE 170M) and BT.2020 non-constant luminance.
  BT.2020 constant luminance is refused by name.
- PQ and HLG streams resolve automatically to the config's Rec.2100 PQ or HLG color space,
  searched among active and inactive spaces because configs usually hide these display-referred
  spaces from pickers. The pinned ACES 1.3 CG config provides `Rec.2100-PQ - Display`; Bloom
  Neutral has no PQ space, so PQ there is refused with a diagnostic that names the space. Neither
  pinned config defines HLG, so HLG resolves to the synthesized scene-light `Rec.2100-HLG (Bloom)`
  space ([color management](color-management.md#synthesized-rec2100-hlg-input-space)). Other
  Rec.2020 streams need an explicit space.
- On the unmanaged legacy path (no config space), Rec.2020, PQ and HLG stay typed `Unavailable`,
  including under a legacy transfer override. Raw and linear compatibility overrides keep their
  transfer-only behavior.

Generated untagged (64x48, the SD case), BT.2020/PQ and BT.2020/HLG H.264 fixtures and an untagged
1280x720 ProRes 422 fixture (the HD case) check tag plumbing and the assumption. GPU/CPU
input-to-working parity at the unchanged 2e-6 gate is tested for tagged Rec.709 H.264, a PQ-tagged
stream, an untagged stream interpreted as Rec.2100 PQ, and a 16,384-sample still sweep across the
PQ code range including negative and above-one values, plus the HLG fixture through the synthesized
space under both pinned configs.

OCIO implements the Rec.2100 PQ input as an inverse half-domain 1D LUT (65,536 entries). Its
generated GLSL folds the half index and the interpolation fraction into one binary32 coordinate
of up to 2^16, which keeps only 8 to 10 fraction bits; on the steep PQ segment that alone measured
2.19e-6 relative at 52.37 linear. Bloom's OCIO GPU adapter therefore rewrites every half-domain
lookup to take the half index from the float's exponent and top ten mantissa bits and the fraction
from the remaining thirteen, both exact, and to fetch the two neighboring entries directly. The
PQ cases then match the CPU oracle exactly.

Tests generate 48-frame H.264 MP4 and ProRes 422 MOV fixtures with burned-in frame numbers and
48 kHz PCM, using the intake libraries in a worker-only fixture generator. An additional ProRes 4444
fixture checks half-opacity alpha. Qualification evidence hashes the generator source; the
execution key separately binds the dependency lock and provider build. The H.264 fixture uses
baseline I_PCM macroblocks because the intake intentionally contains no H.264 encoder or FFmpeg
CLI. The MOV muxer writes its MP4-compatible `isom` sample entry. Generated provenance describes
the synthetic pixels, audio samples and tool version; no binary fixture is checked in.


## Time-based Export (MEDIA-4)

Implemented on Linux, 2026-09-17. The existing supervisor's explicit typed `Unavailable` backend
is the supported macOS/Windows fallback. No in-process encoder or FFmpeg header is added to the
host. Strict delivery and Apple authorization remain gated by ADR 0020.

| Preset or worker capability | Implemented export | Preservation and qualification |
| --- | --- | --- |
| `ProResMovV1` | ProRes KS Proxy, LT, 422, HQ, 4444, 4444 XQ; optional PCM | Preview workflow; straight alpha for 4444 variants only; no Apple authorization or delivery claim |
| `DnxhrMxfV1` | DNxHR LB, SQ, HQ, HQX, 444; optional PCM | Intra-frame lossy video; dimensions, rate and audio layout must satisfy the muxer |
| `PcmWavV1` | Signed little-endian PCM 16/24; BWF description when supplied | Byte-exact pinned worker; RF64 auto promotion; BW64 remains pending |
| `TiffRgba16SrgbV1` | Single frames and numbered sequences | Exact decoded RGBA16 after the declared OCIO display/view conversion and quantization |
| Worker-only capabilities | DNxHD; AAC; MOV, MXF, Matroska, WAV mux | Closed stream layouts; unsupported profiles or timing return typed failures |
| `H264MovV1` | H.264 High MOV review encode via Cisco OpenH264 2.6.0 when verified; optional AAC or PCM; 30 Mbps default | 8-bit 4:2:0, one-second GOP, Rec.709 limited range; lossy `DecodedSemanticTolerance`; Review deliverable — not for archival |
| H.264 hardware encode | H.264 High MOV via VA-API when the worker creates an encode context | Hardware execution key; no determinism claim; skipped with a typed reason when no `/dev/dri/renderD*` exists |
| H.264/HEVC software encode | H.264 software is conditional; HEVC software remains unavailable | `H.264 encoder not installed` until the verified Cisco binary is present; HEVC remains typed unavailable |

The ProRes dialog, approval text, MOV comment and `MediaQcEvidenceV1` carry:

> Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation

`apple_authorized=false` and `delivery_qualified=false` accompany that evidence. Preview export
success does not establish an authorized pipeline. H.264 review evidence names `openh264 2.6.0`
plus its verified digest, or `vaapi <driver>`. No output is presented as archival or delivery
qualified.

OpenH264 policy is host-owned and Qt-free. The first install requires explicit consent to Cisco's
binary license and stores `media/openh264-consent=2.6.0` in the per-user `Bloom.conf` beside the
`openh264` directory (a Qt-free file, not the application's QSettings store); the entry is written
once, through a temporary file, however often Bloom installs, and duplicate `[media]` groups left by
earlier appends collapse to one. It never fetches at startup. The host supervises system `curl` for at most 60 seconds and 16 MiB, then `bunzip2`,
verifies both archive and library SHA-256 values from the dependency lock, and atomically publishes
`libopenh264.so.8` and `BINARY_LICENSE.txt` below the per-user `Kinetik/Bloom/openh264/2.6.0`
directory. A locate action applies the same verification: it hashes the chosen file while copying it
into a staging file beside the install (the artist's own file is never moved, and a copy across
file systems works), publishes the staging file only when its digest matches, and stops between
chunks when cancelled, leaving no staging file.

Which H.264 encoder a platform uses is the export provider's answer, `output::h264EncoderSupportV1()`,
a pure platform query that opens no file: VideoToolbox inside the macOS AVFoundation provider
(nothing to install or consent to), Cisco OpenH264 or VA-API hardware on Linux x86-64, VA-API
hardware only on Linux on other processors (Cisco publishes no binary there), and none yet on
Windows, where the H.264 preset reports the worker as unavailable. The Export panel offers
install, consent and Locate (the H.264 encoder dialog) only when the answer is OpenH264. Everything that touches the disk --
`verifyH264RuntimeV1()` (the library's SHA-256), `vaapiRenderNodeAvailableV1()` (the
`/dev/dri/renderD*` probe) and `locateH264RuntimeV1()` -- runs as a `BlockingIo` task through
`ui::H264EncoderService`, which publishes the result to the dialog; Locate shows a busy bar and
Cancel inside the dialog while it runs. The worker receives the directory and
expected digest, prepends that verified directory to its private loader directory before FFmpeg
use, and rejects a mismatch. Every worker and fixture-generator build also contains a Bloom-owned
`libopenh264.so.8` API shim in the private `lib/` directory. The shim contains no codec code and
keeps FFmpeg loadable when the Cisco binary is absent; it reports the typed `H.264 encoder not
installed` state for software encode. Decode never needs the Cisco binary, and the shim is used
for decode and fixture generation until a verified Cisco directory is available. The private
superbuild stub is not a runtime path and no Cisco binary is bundled.

`host::SequenceExportRunnerV1` compiles one immutable snapshot. `output::CompositionOutputStreamV1`
owns the codec clients, source-audio decoding and offline mix; host owns approvals and publication.
The runner consumes SCRIPT-0's
`FrameRangeRunnerV1::timeForFrame` mapping, then runs the existing per-frame analysis attempt and
exact-digest approval on the authoring thread, several frames ahead of one ordered encoder
([frames in flight](frame-output.md#frames-in-flight)). One encoder slot provides back-pressure:
the export ledger admits prepared pixels, transport copies and retained source audio before
allocation. The worker retains only first/last frame references, codec buffers and bounded private
file products. Audio uses the production `AudioEngine` mixer through its offline null-device pull,
in blocks of at most 512 samples, stopping at each rational video-frame boundary. The final sample
count is the floor of range duration times the chosen source sample rate. Nested clip time mappings,
levels, mute and solo use the normal audio graph.

The worker closes its private artifact and decodes it again with the same provider. QC checks every
video timestamp, frame count, duration, codec, dimensions and stream layout; first/last decoded
pixel digests and error measurements; and every PCM sample against the quantized mix. It makes no
independent-reader claim. A bounded chunk transfer writes a private coordinator lease, closes it,
reopens it and verifies the whole-file digest before atomic publication. Cancellation, worker
failure, target changes and supersession cannot publish a partial movie. Existing destinations
survive failures. PNG/EXR/TIFF sequences retain one atomic publication per frame; cancellation
preserves complete frames already published.

### Immutable tolerance profiles and records

Lossy video uses `DecodedSemanticTolerance`. Its exact UTF-8 profile is:

```text
bloom.video-rgba16-srgb.v2;first-last;reference=declared-sampling;absolute-max=22938;mean-ceil=1967;alpha-max=64;timing=exact;stream-layout=exact;pcm=exact
```

SHA-256: `15e8c88900528492844693ec44fe2261d5a713ec96b4498c1b2ec65c611290d3`.
The maximum and mean thresholds use unsigned 16-bit sample units. Alpha is checked separately
against the reference alpha; TIFF requires exact equality.

The comparison models the codec's own chroma sampling (`reference=declared-sampling`). The
reopened stream declares its sample format, matrix, range and chroma siting. Bloom takes the
RGBA16 reference the encoder received through that sample format with the encoder's own conversion,
using the declared matrix, range and siting. It then reconstructs the reference and the decoded
frame to RGBA16 with one siting-aware conversion and compares them. 4:2:0 and 4:2:2 chroma
decimation is therefore not counted as codec error, so a small saturated feature passes. The
luma, alpha and every chroma sample the codec kept are still compared, and the bounds are
unchanged. A real encoding error still fails: corrupted or drifting samples, a sample layout that
decimates chroma differently from the encoder's input, or a declared matrix, range or siting that
differs from what the encoder did. Unspecified siting means co-sited left (the H.273 default, and
what Bloom's reader assumes). An unspecified matrix means BT.709. A Y'CbCr stream that declares a
non-Y'CbCr matrix is refused. The encoder converts with exactly the matrix, range and siting it
declares: subsampled chroma is written co-sited left and tagged, and 4:4:4 (ProRes 4444, DNxHR
444) has no siting. Version 1 compared against the unsampled reference. It failed a 10x10
saturated red square in ProRes 4:2:2 because the edge pixels that share a chroma sample with
black reconstruct with about 40% of their red. Version 1's SHA-256 was
`822d817ff9130218c44a4bd91de7327b349722681bb15f534894259138be3c56`. These are acceptance bounds, not predicted errors. An export outside
the bound fails closed. AAC uses the exact profile
`bloom.aac-f32.v1;sample-count=exact;priming=trim;absolute-max=0.25;rms=0.05`; video with AAC binds
the SHA-256 of the two profile strings concatenated in video-then-audio order. PCM has no tolerance
digest and uses `ByteExact`; TIFF uses `DecodedSemanticExact`.
AAC profile SHA-256: `2c4c461185d7d9ba7741afefbc076060b7e55cb9b75e39d15a934eea133c03d8`.
Combined video/AAC SHA-256: `88e3e40e56e68e465d0250f2e45f26b26959f8ce5ab1f8f723be88bceefd8d7e`.

Wire protocol version 3 adds encode begin, frame/audio acknowledgements, finish/QC and bounded
artifact chunks. `EncodeSettingsV1` canonical encoding orders container, codec, profile, audio codec,
dimensions, rational rate, frame/sample counts, sample rate, channels, BWF description, determinism
and tolerance digest after its versioned domain; version 2 (#369) adds the color tags and the bit
rate. Execution byte limits are excluded from semantic identity. The 256×128, 48-frame HQ/PCM
fixture record (video tolerance profile v2) is 166 bytes and independently derives
`7e8bea8770830d7257eba98ee80d465a1d0106bbc3e06665f8d171d99b1baa2f`.

Media analysis binds the frozen preset byte (4 = ProRes, 5 = DNxHR, 6 = PCM, 7 = H.264 review), settings digest and
all eleven ordered preservation facets. COLOR-5 binds the chosen display/view identity and look
effect count. Its HQ/PCM fixture advances from 579 to 759 bytes and independently derives
`1253f309292cb3999ba7f1f81b46b38e3260d59c64dd37bac17a5b70dfb47b9d`.
The neutral display record remains 224 bytes, SHA-256
`b473d0289aa343bc89b1465c4779c557c687e3ee724d5aae337d9829f8e949b1`.
The [COLOR-5 identity oracle](color5-identity-oracle.md) constructs these records independently.
The expanded QC oracle fixture is 527 bytes. The QC record appends artifact size, frame/sample counts, rational duration, first/last/audio and
approval digests, tolerance, determinism, authority booleans, implementation note and measured
errors. Independent oracles pin field order. The runner returns this immutable evidence with the
publication outcome. Default no-look EXR identities remain unchanged; explicit output transforms and look policies
are specified in [Frame Output](frame-output.md).

The pinned 48-frame, 24 fps motion fixture reopens both ProRes HQ MOV and DNxHR HQ MXF with
2/1 duration and 96,000 stereo PCM samples. Every decoded sample equals the independently
constructed source after mixing. Both motion exports measure maximum/mean RGB error 24/24 in
unsigned 16-bit units. The separate worker pattern measures 28/24 for ProRes HQ and 243/76 for
DNxHR HQ. TIFF decodes exactly; WAV matches an independent RIFF/PCM byte oracle. Tests also
cover all admitted profiles, AAC acceptance and out-of-tolerance rejection, BWF metadata,
H.264/HEVC refusal, cancellation, worker crash/retry, abandoned-client cleanup and sequence
byte parity for PNG, EXR and TIFF.


### Review color and look evidence (COLOR-5)

The named **Review H.264 (Rec.709, look on)** deliverable evaluates the captured composition with
`bypassLookNodes=false`. It unpremultiplies RGB, applies the exact config's
`Rec.1886 Rec.709 - Display` / `ACES 1.0 - SDR Video` OCIO `DisplayViewTransform`, clamps and
quantizes the display values to RGBA16, then sends the existing worker settings. The 30 Mbps
default, H.264 High profile and Rec.709 tags remain unchanged. PNG/TIFF and raw movie presets use
the config's default display/view instead of a fixed sRGB transfer function. No Viewer state is
consulted. Unsupported config/display/view combinations fail before encoding.

Analysis binds the processor identity, actual display/view pair, config revision and
`look: baked (N look-tagged effects)`; approval and QC retain that description. The existing
versioned worker tolerance-profile name retains `srgb` for compatibility: its numerical limits
compare the supplied display-referred samples and do not select the color transform. The COLOR-5
2× gain LUT fixture decodes the first review frame through the worker and independently converts
its tagged Rec.709 YUV to display RGB. Maximum/mean errors are 252/133 in 16-bit units, within
22938/1967. The same fixture's look-bypassed AP0 handoff has zero error against direct OCIO output.
Worker and external-LUT unavailability on macOS/Windows remain explicit supported fallbacks.

## Export Queue Media (#369)

Issue #369 ships with the export queue ([ADR 0033](../decisions/0033-export-queue.md), section 16):

- **MP4.** The encode worker writes H.264 and HEVC into MP4 (`H264Mp4V1`, `HevcMp4V1`) as well as
  MOV. MP4 movies carry AAC audio; MOV and MXF carry PCM.
- **HEVC** is VA-API only (`hevc_vaapi`, Main and Main 10). There is no software HEVC encoder:
  without a VA-API render node that opens HEVC encode, HEVC presets report
  `HardwareEncoderMissing`.
- **VA-API render-node discovery** is part of the encoder probe. The worker's `--probe-vaapi` mode
  opens the H.264 and HEVC encode entry points on every render node and prints the answer;
  `output::probeVaapiEncodeV1()` reads it, so no driver code loads into the application. Encoding
  uses the probed node.
- **Color tags** come from the output color intent: `output::outputColourTagsV1()` maps the
  resolved display to H.273 primaries, transfer and matrix (sRGB displays get the sRGB transfer,
  Rec.1886/Rec.709 BT.709, P3 and Rec.2100 their own code points, unknown displays unspecified), and
  the matrix selects the RGB to YUV coefficients. `EncodeSettingsV1` is version 2 for this; the
  reopen reads the tags and `<container>/<codec>` back into `EncodeQcV1`/`MediaQcEvidenceV1` for the
  ColourTags and Container checks.
- **Multithreaded encode.** The worker uses an automatic thread count for video. Lossy codecs
  already declare `DecodedSemanticTolerance`, so threading can change bytes but not the decoded
  tolerance; PCM stays `ByteExact` and TIFF single-threaded `DecodedSemanticExact`.
- **Limits from the destination.** The encoded artifact's byte limit is the destination's sampled
  free space minus the safety margin (2%, at least 64 MiB), within the provider ceiling, and the
  audio channel cap follows the container (WAV and MOV 64, MXF 16, AAC 8) instead of a fixed 8.
- **DPX** sequences are part of the 1.29 vocabulary and the catalogue but are reported unavailable
  with a typed reason in this build.
