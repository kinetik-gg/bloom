# Professional Media I/O And Codec Providers

Status: working research

Updated: 2026-09-17

## v0 implemented boundary

The accepted v0 amendment in ADR 0020 admits the image pipeline under `src/media/image`:
a private, pinned stb_image PNG/JPEG decoder and a bounded OpenEXR reader compiled in process with
hard input and allocation limits. PNG16 is included. Images and numbered sequences become asset
records; authoring source nodes reference stable asset IDs. Broad time-based media remains the
working research below.
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
single-part scanline files and single-level tiled files with RGB, RGBA, Y, or YA channels, all
half or all float, within the image limits above. It honors the data window, retains the display
window offset in the Bloom image descriptor, defaults alpha to associated/premultiplied, and maps
the optional chromaticities attribute to the linear color tag (or records the absent-metadata
scene-linear assumption in the probe). Deep, multipart, multi-level tiled, unknown-channel,
subsampled, mixed-type, malformed, and truncated files return typed diagnostics and never become
project truth. EXR decode runs on a worker and uses the same disk-cache key and thumbnail path as
PNG/JPEG.

TIFF is content-probed by classic and BigTIFF magic, but its read and write operations are provider
operations. `ImageProvider` carries explicit Decode and Encode callbacks matching the worker image
product; no global registry is consulted. Until MEDIA-3 supplies the callback, TIFF returns
`ProviderMissing` and the `TiffRgba16SrgbV1` export preset is listed as unavailable with that reason.
The output contract is fixed as RGBA16, sRGB, straight alpha; enabling it later requires the
worker provider to satisfy that contract and its reopen/verification path.

| Bound | v0 value |
| --- | --- |
| Encoded file | 64 MiB |
| Either dimension | 16,384 pixels |
| Pixel count | 16,777,216 |
| Decoder allocator live bytes | 256 MiB per thread |
| Float image storage | 256 MiB per image, also subject to request budget |
| Directory entries and sequence span | 100,000 each |
| Decoded image LRU | 1 GiB per evaluator cache |
| UI thumbnail cache | 512 RGBA8 images, at most 64 × 64 (8 MiB) |

These are separate bounds, not an aggregate process-memory guarantee. Cancellation is checked
between file chunks, before and after stb, and during conversion/proxy rows; an in-progress stb
call is cooperatively cancellable only after it returns. Import and relink preparation and
thumbnail work use the blocking-I/O task executor, with Jobs progress, diagnostics and Cancel.
Publication checks the originating project/revision and discards work after project replacement.
The evaluator decodes only on worker execution; UI paint reads cached thumbnails.

The same C++ decoder, filesystem paths and Qt dialogs/drop events serve Linux, macOS and Windows.
Imports across different filesystem roots are refused explicitly; put the media on the project's
root before importing. The project-relative locator is preferred. If it is unavailable, the
absolute file-URI hint supports local recovery, including the first Save As of an unsaved project;
content-digest verification still applies. Relink records a fresh relative locator for portable
packaging after such a move. Media is referenced, never embedded or silently copied.

This slice does not add movie containers, EXIF orientation, ICC interpretation, metadata
round-tripping, external codec processes, or a general proxy service. Files are interpreted by
the closed sRGB/override rule in `color-management.md`, not by embedded profiles. The broader
research below does not supersede this implemented boundary.

## FFmpeg dependency intake (2026-09-17)

The Linux candidate is FFmpeg 8.1.2, selected from the maintained 8.1 release line for this
intake. It is a dependency-only result: the future `bloom-media-worker` may consume its shared
libraries, while the desktop `bloom` target must not link or load `libav*`. No worker or codec
integration is part of this intake.

The lock records the official `ffmpeg-8.1.2.tar.xz` archive, detached-signature evidence, exact
configure arguments, and a corresponding-source obligation. The recipe is shared-only,
library-only, LGPL-2.1-or-later, disables GPL/nonfree/version-3/network/autodetection, and has no
external codec or crypto libraries. Its closed preview allow-list covers MOV, Matroska, MXF, MPEG-TS,
WAV, MP3, and AAC demuxing; MOV, Matroska, MXF, and WAV muxing; H.264/HEVC, ProRes, DNxHD, MJPEG,
TIFF, AAC, MP3, and explicit PCM-family decoding; ProRes KS, DNxHD, PCM-family, AAC, and TIFF
encoding; the required parsers/bitstream filters; and the `file` protocol. VA-API H.264/HEVC
hardware acceleration is enabled when `libva` is available and can be disabled for a host without
the development files.

FFmpeg ProRes is a non-authorized preview/workflow capability only. It is not evidence of Apple
authorization, certification, patent clearance, or delivery qualification. The Linux worker's
eventual private runtime may use `$ORIGIN/../lib` rpath resolution for its own bundled shared
libraries; the desktop binary receives no FFmpeg rpath. Source, license, security, and review
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

The engine-only v0 slice implements bounded local preview for WAV and MP3. It is the companion audio
paragraph to the narrow in-process image exception introduced by MEDIA-1; it does not accept the
general worker/provider design as complete and it does not add audio to project schema, the graph,
or the timeline. AUDIO-2 owns those document and UI connections.

### Formats And Limits

- WAV is identified from RIFF/RF64 plus WAVE content and decoded through the pinned `dr_wav` header.
  PCM and IEEE float WAV encodings supported by that adapter are converted to Bloom-owned planar
  float32 samples.
- MP3 is identified from an ID3 tag or MPEG frame sync and decoded through the pinned `minimp3`
  core and extended headers with float output. No MP3 encoder or broad codec claim is made.
- `probeAudio(path)` returns the container, sample rate, channel count, frame count, and exact
  `core::RationalTime` duration. `decodeAudio(path, limits)` returns an `AudioBuffer` with one
  float32 plane per channel and rejects partial reads.
- The default file-size cap is 64 MiB and the decoded sample budget is 48,000,000 interleaved
  samples. Limits are checked before decoder entry and before decoded storage allocation; wrong
  magic, truncated input, malformed headers, invalid formats, and budget violations return typed
  errors without publishing a partial buffer.
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

Audio assets are imported by the same atomic worker transaction as images. `probeAudio` supplies the
persisted rate, channel count, frame count and exact duration; `decodeAudio` and `waveformSummary`
run only on the blocking-I/O worker. The UI publishes a bounded waveform summary for Assets, the
timeline bar and the audio source node card. Painting reads the immutable summary and never opens or
decodes a file.

The UI/audio boundary maps a compiled clip's stable asset ID to a published planar `AudioBuffer`
before calling `AudioEngine::replaceClips`. Audio asset records, source parameters, layer flags and
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
QSettings override (an absolute path; anything else falls back to the platform default). No
in-app settings dialog exists yet for the RAM preview or operation-cache byte budgets either
(`preview_frame_cache.hpp`'s own `ramPreviewByteBudgetFromSettings()`/
`operationCacheByteBudgetFromSettings()`); the disk cache's `media/disk-cache-enabled`,
`media/disk-cache-budget-bytes` and `media/disk-cache-directory` keys follow that same
QSettings-only precedent (`bloom::ui::media_disk_cache_settings`) rather than adding a first
Preferences surface for one feature.

**Cache key.** Content-addressed and restart-stable, deliberately narrower than the evaluator's
in-process operation-cache key: asset content digest, member/frame, interpretation (color space
and alpha association), the Bloom Neutral config digest, and a decoder identity/version string
(`bloom::media::cache::kImageDecoderIdentity`) bumped whenever the decode or Bloom-Neutral-
conversion pipeline could change decoded pixels for the same source bytes -- a decoder upgrade
therefore invalidates old entries by construction. The key omits the resolved path/relink hint and
availability that the memory-cache key includes, so a relink of identical content still hits the
disk entry.

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
same condition that already excludes such a request from the evaluator's memory operation cache
(`request.bypassOperationCache` / `plan->bypassOperationCache()`) also passes a null disk cache
into `evaluateImageSource()`, so an override is never read from or written to disk either.

**Controls.** `media/disk-cache-enabled` (default on), `media/disk-cache-budget-bytes`, and
`media/disk-cache-directory` in QSettings; "Clear Media Cache…" in the Composition menu asks for
confirmation, then clears every entry and resets statistics. The window status bar's own disk-
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

Status: implemented for the synthetic provider and Linux process backend (2026-09-17).
The following sections remain the larger qualification contract; the implementation slice is
explicitly limited to the values and operations described here. No real codec is advertised.

`bloom_media_provider` owns the Qt-free, codec-free contract, canonical identities, registry,
and protocol. It depends only on `bloom_core`. `bloom_media_worker_host`, also located under
`src/media/provider`, is the host facade over `bloom_platform_process` supervision and the
core-only `bloom_runtime` scheduler target. The evaluator is a separate target; neither the
scheduler nor the contract library depends on the host facade, so target dependencies remain
acyclic. The repository module allowlist permits that narrow scheduling edge. The desktop
must never link `bloom_media_fake_worker_provider`, the worker-private provider target that
will later acquire codec adapters. No UI or durable document schema is involved.

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

The numeric v1 budgets below instantiate the resource categories described by this document;
previous image limits are retained where applicable. Stricter qualified providers may reject
requests within these ceilings. Expanding a ceiling requires a reviewed profile/protocol change.

| Resource | v1 ceiling |
| --- | --- |
| UTF-8 string | 4096 bytes |
| Streams, declarations, pipeline steps, registry entries | 256 each |
| CPU planes | 4, with exact plane count/layout for each supported format |
| Dimension / pixels | 16384 per dimension / 16777216 pixels |
| CPU-plane storage including row padding | 256 MiB per product |
| Wire frame excluding its length prefix | 257 MiB |
| Audio | 64 named channels, 65536 samples per channel, 384000 Hz |
| Default worker address space / open descriptors | 2 GiB / 64; core dumps disabled |
| Default pool slots / call deadline | 2 / 5 seconds |
| Default cancellation message / SIGTERM grace | 50 ms / 100 ms |

Video CPU formats currently close to RGBA8, RGBA32F, and YUV420P8. Plane dimensions, stride,
byte count, chroma extents, aggregate allocation, and SHA-256 are revalidated by the host.
Probe streams preserve original H.273 color tags with -1 for absence. This does not resolve
color interpretation. Audio values use finite planar floats, explicit unique channel roles,
matching sample counts, and bounded rates; audio transport/decoding is not implemented yet.

The wire envelope is `u32 body_length`, `u32 magic=0x314d4c42`, `u16 protocol=1`,
`u16 schema=1`, `u8 kind`, `u64 session`, `u64 sequence`, then payload. The fixed body header
is 25 bytes. Every response echoes the host's nonzero session nonce and next sequence.
Handshake, Call, Probe, Frame, Cancel, Shutdown, Ack and Failure use kind values 1 through 8.
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
watchdog thread is installed. macOS/Windows return typed process `Unavailable`.

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
CMake does not append link directories. The synthetic worker uses the same explicit rpath in build and install trees, avoiding empty
loader-search entries.
The current fake worker needs no codec library. MEDIA-3 must place the reviewed shared FFmpeg
closure in its worker generation's private directory and validate the installed binary and
transitive library rpaths. The desktop must continue to pass `desktop-no-ffmpeg`; loader paths
and codec-bearing worker targets must never be propagated through `bloom_media_provider`.
macOS and Windows packaging policies will be qualified with their process/provider backends.

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
