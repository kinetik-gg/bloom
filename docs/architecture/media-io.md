# Professional Media I/O And Codec Providers

Status: working research

Updated: 2026-08-25

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

### 5. Proxies, Thumbnails, And Waveforms

Proxies are derived assets with a manifest binding source content, selected streams, interpretation,
time mapping, conversion version, codec/container preset, provider provenance, and semantic fixture
profile. A proxy is used only when that manifest matches. The UI always indicates proxy or reduced-
resolution playback; final render never consumes a proxy unless an explicit offline-media policy
allows it and records the consequence.

Thumbnail and waveform generation share the same interpreted source and bounded task contracts.
They do not run independent “quick” probes that can disagree with the Asset Registry.

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
