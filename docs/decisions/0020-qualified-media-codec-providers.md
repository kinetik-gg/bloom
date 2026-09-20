# ADR 0020: Qualified And Isolated Media Codec Providers

Status: proposed

Date: 2026-08-25

## Accepted MEDIA-K1 amendment: contract and supervised worker (2026-09-17)

Owner decisions dated 2026-09-16 admit the general provider worker now. Linux's first real
provider will be FFmpeg in `bloom-media-worker`; MEDIA-3 owns that adapter. The existing
FFmpeg 8.1.2 dependency intake remains worker-only. This lane introduces no codec headers,
real media parser, or new in-process codec exception. Native macOS and Windows providers and
process backends follow later. Their current supported fallback is typed `Unavailable`.

FFmpeg ProRes is permitted as a preview-workflow candidate with explicit
`apple_authorized=false` and `delivery_qualified=false`. It cannot satisfy strict ProRes
or recipient delivery requests. This records the owner's product decision; it adds no vendor
authorization or distribution qualification.

Acceptance requirements #1–#3 are delivered for the MEDIA-K1 kernel and Linux worker scope:

1. `src/media/provider` implements the five versioned capability, execution, qualification,
   pipeline, and QC records, stable role/purpose/determinism values, canonical SHA-256 identities,
   bounded probe/stream/CPU-plane/planar-audio values, and an independent encoding oracle.
2. The exact registry requires separate passing pipeline evidence, separates technical evidence
   from attributed authority, and pins execution generations in attempt handles. Missing fields,
   missing roles/purposes, revocations, and absent authority yield typed unavailability.
3. The bounded pipe protocol and Linux supervisor prove handshake identity checks, copied-plane
   validation, replay rejection, crash/watchdog/cancellation containment, resource limits,
   bounded admission, and orderly shutdown through the synthetic worker. Other targets compile
   an explicit unsupported process backend. The original all-three-target containment requirement
   remains a qualification gate when those backends arrive; Linux evidence is not portable proof.

The containing ADR remains **proposed** for broad real-codec/export/delivery qualification.
MEDIA-3 supplies the Linux read slice and MEDIA-4 supplies the export evidence below.
Cross-platform qualification, external GPU leases, shared memory and strict delivery remain
future work. Existing
accepted v0 image/audio exceptions stay narrowly scoped; no further exceptions are admitted.
The implemented wire format, limits, target boundaries, and worker rpath policy are specified in
[`media-io.md`](../architecture/media-io.md#implemented-media-k1-contract-and-worker).

## MEDIA-4 export evidence (2026-09-17)

Owner decisions dated 2026-09-16 permit FFmpeg ProRes read and write for preview workflows with
the explicit note "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation".
The note appears in export and approval dialogs, the MOV metadata and the returned QC record.
Both authority booleans remain false. H.264/HEVC software encoding is excluded from intake and
returns typed `Unavailable`; no hardware determinism or delivery qualification is inferred.

Requirement #5 now has a Linux implementation: `host::SequenceExportRunnerV1` compiles once,
uses SCRIPT-0's exact rational frame mapping, approves every frame, admits a one-frame queue
through the ledger, mixes timeline audio in bounded blocks, and uses the isolated encoder/muxer.
Close/reopen QC checks frame/sample counts, timing, stream layout, first/last pixel tolerances and
every PCM sample. Staged transfer and reopen hashing precede shared-coordinator atomic publication.
The 48-frame motion/audio fixture, TIFF round trip, independent WAV byte oracle, cancellation,
worker-crash/retry and identity oracles exercise this boundary.

For #8, the provider handshake reports the ordered encode/mux/reopen roles and pinned execution
identity. The Export Composition dialog exposes preset/profile, range, destination, source-rate
audio, explicit ProRes limitations and provider-unavailable reasons. Headless callers receive the
same typed settings, analysis and `MediaQcEvidenceV1`; SCRIPT-0 remains the owner of CLI surfaces.
This is partial #8 evidence: macOS/Windows still use the explicit unavailable process backend,
and no platform or recipient qualification is borrowed from Linux results. The ADR remains proposed
for its broader acceptance conditions.

The implemented export ladder, canonical records and immutable tolerance profiles are owned by
[media-io.md](../architecture/media-io.md#time-based-export-media-4).

## H.264 review encode intake (2026-09-18)

The review deliverable admits H.264 MOV at approximately 30 Mbps using Cisco OpenH264 2.6.0 for
software encoding and VA-API hardware encoding when an encode context is available. Software
capability is declared only after the host verifies Cisco's runtime binary digest; the worker gets
the verified directory and digest and refuses a mismatch. The software tuple is High profile,
8-bit 4:2:0, one-second GOP, CBR/VBR target, and Rec.709 primaries/transfer/matrix with limited
range. H.264 review output is `DecodedSemanticTolerance` with its tolerance-profile digest and is
explicitly not archival or delivery-qualified.

OpenH264's source is built only as a private FFmpeg link-time stub. Cisco's redistributed binary is
not bundled, and installation requires an explicit licence-consent record, a supervised bounded
download, decompression, digest verification, and atomic publication. VA-API is a separate
hardware execution key with no determinism claim; its tuple is advertised only when the worker
initialises the hardware path. The export dialog exposes a typed “H.264 encoder not installed”
reason, one-click install/locate controls, and a VA-API alternative.

## Accepted v0 amendment: WAV and MP3 preview audio (2026-09-15)

Building on the narrow v0 in-process image exception introduced by MEDIA-1, AUDIO-1 admits a
similarly bounded, engine-only audio exception for local WAV and MP3 preview. The pinned private
`dr_wav` and `minimp3` adapters use a 64 MiB file-size cap and a 48,000,000 interleaved-sample
budget, validate limits before decoder entry and storage allocation, and reject wrong-magic,
malformed, truncated, and partial inputs without publishing a buffer. The pinned `miniaudio`
adapter is device output only; it receives Bloom-owned samples and does not parse media files.

This is an accepted resource-boundary disposition, not process isolation or a general provider
qualification. The security records under `dependencies/licenses/{dr_wav,minimp3,miniaudio}/`
record that bounded in-process parsing cannot guarantee containment of a memory-corruption defect,
and that decode calls are not interruptible. The exception covers no video, encoder, broad codec,
project-schema, graph, timeline, or UI behavior; AUDIO-2 owns those connections. The engine's
device callback consumes a lock-free bounded queue, and its frame-written count is the audio master
clock while playing; the synchronized transport follows that clock rather than an independent
wall-clock timer.
## Accepted v0 amendment: PNG and JPEG images (2026-09-15)

The narrow v0 image pipeline may parse user PNG/JPEG files in process through the pinned,
private stb_image adapter. This exception does not accept the proposed broad provider design.
There is no fixed whole-file, per-axis or total-pixel ceiling. Geometry is bounded by the codec's
representable window and integer overflow checks; the encoded file is streamed (no whole-file cap);
parser temporary memory is thread-local and scoped to the request; and decoded RGBA32F storage plus
bounded decode scratch is admitted against the caller's explicit `pixelBudget` (default 268435456
bytes) before any large allocation. Scans are bounded to 100000 entries and a 100000-frame span.
Hostile fixtures exercise malformed/truncated files and overflowing/oversized windows.
These limits bound resources but do not isolate memory corruption. A bounded decoder call is
not interruptible; cancellation is checked before/after it and within conversion and scans.
All I/O, hashing, scanning, decode and proxy work executes off the UI thread with diagnostics,
activity and safe task shutdown. Audio, video, arbitrary codecs, general provider workers,
metadata colour-profile interpretation and HDR import stay outside this exception.

PNG8/PNG16 and JPEG use display-referred sRGB, inverted through the existing Bloom Neutral
`srgb_rec709_display` transform. Straight alpha is premultiplied after conversion. Per-node
Auto/sRGB/Linear/Raw interpretation is explicit. Numbered image sequences hold gaps with a
warning and use composition-rate Hold/Loop/PingPong playback.

## Context

Bloom needs professional still, image-sequence, video, and audio ingest and export on Linux, macOS,
and Windows. A broad media library can expose many demuxers and codecs, but that does not establish
frame-accurate seeking, preservation of time/color/alpha/audio metadata, safe handling of untrusted
media, a recipient-compliant deliverable, or permission to make a vendor certification claim.

The Apple ProRes family makes the distinction concrete. SMPTE RDD 36 discloses the bitstream syntax
and decoding process, while RDD 44 defines a constrained MXF mapping. Apple separately licenses and
certifies named products and workflows, and its current authorized-products page explicitly calls
FFmpeg and derivative ProRes implementations unauthorized. MainConcept publicly offers an
Apple-approved cross-platform decoder SDK, but no comparable embeddable cross-platform encoder SDK
has been verified. nablet mediaEngine is an Apple-listed encoding product and a serious commercial
candidate, but Bloom embedding, redistribution, exact profile support, and certification transfer
are unanswered.

One universal FFmpeg path would therefore be convenient but would overstate strict ProRes delivery.
Three unrelated platform media implementations would also leak platform behavior into project
semantics and multiply validation logic. In-process codec parsing would put a large untrusted-input
surface inside the application process.

## Proposed Decision

- Bloom owns provider-neutral media descriptors, interpretation, rational-time mapping, decoded
  products, output presets, preservation analysis, verification, QC evidence, diagnostics, and
  atomic publication. A codec or platform framework remains a private implementation provider.
- Register providers by immutable, exact capability tuples. Direction, container profile, codec
  profile/level, chroma, depth, range, alpha, field mode, rate/dimension limits, audio layout,
  color/HDR/timecode/metadata support, software/hardware implementation, surface transport,
  platform, provider/build identity, and fixture digest are never inferred from a codec name.
- Close version 1 operation roles to Probe, DemuxIndex, VideoDecode, AudioDecode, VideoEncode,
  AudioEncode, Mux, ReopenDecode, StructuralQc, and RecipientQc. Close purposes to Preview, Proxy,
  Conform, Export, and Delivery. An unlisted role or purpose is unavailable, never implied by
  another role.
- Represent selection and claims with separate immutable `MediaCapabilityKeyV1`,
  `ProviderExecutionKeyV1`, `QualificationEvidenceV1`, and `PipelineQualificationV1` records.
  Component qualification is not transitive: the exact ordered demux/decode/convert or
  convert/encode/mux/reopen/QC pipeline must pass its own fixtures.
- Qualify ingest, preview, export, and named delivery profiles independently. Hardware and software
  implementations also qualify independently. Missing tuple fields are unavailable, not wildcards.
- Run general-purpose container and codec providers in supervised, resource-limited
  `bloom-media-worker` processes. Use a versioned bounded provider-neutral protocol and host-side
  revalidation. The same boundary may contain optional commercial providers without exposing their
  SDK types or ABI through Bloom.
- Admit exactly one provider generation and dependency/licensing trust domain per worker process.
  Bloom may reuse a common launcher and protocol, but it does not load every provider into one
  mega-worker. Package identity, dependencies, sandbox permissions, limits, and capabilities are
  bound by the worker handshake.
- Keep the worker protocol internal initially. Do not promise a stable native codec plug-in SDK
  until signing/trust, packaging, lifecycle, compatibility fixtures, and external support policy are
  accepted.
- Establish bounded CPU planes and audio blocks as the first portable reference products. Admit
  runtime-only external GPU surface leases only through a separately qualified media/render interop
  pairing with explicit ownership, synchronization, device generation, semantic format, and CPU
  fallback.
- Treat hardware decode and encode as acceleration providers, not authoring or preset semantics.
  Runtime capability and actual selected hardware implementation are both recorded. Final output
  never silently changes provider, precision, color, alpha, rate control, or quality.
- Record one closed determinism class per qualified pipeline: byte-exact, decoded-semantic exact,
  decoded-semantic under an immutable tolerance-profile digest, or no determinism claim. The last is
  limited to Development or an expressly limited Preview path. Lossy or hardware output never
  acquires a stronger class merely because encoding succeeded.
- Make every time-based export follow: immutable snapshot and preset; preservation report and exact
  approval; bounded render/convert/encode/mux; private staging; close and reopen; structural,
  decoded-semantic, metadata, and required independent QC; then atomic publication. An encoder never
  writes the final target directly.
- Produce bounded immutable `MediaQcEvidenceV1` for every staged artifact, binding its digest and
  size, approval and preset, ordered execution chain, comparison profile, reopen reader, complete
  check results and coverage, independent-reader status, external QC identity, and terminal result.
  Same-provider reopen is verification but not independent QC; an exit status alone cannot pass.
- Capture the provider generation and exact pipeline before an attempt begins. Provider,
  software/hardware path, precision, chroma/range, color, alpha, rate control, metadata, and QC
  policy are never substituted mid-attempt. Preview may start a new visible qualified generation;
  conform or output requires a new analyzed attempt and output approval does not transfer.
- Use a minimal, dynamically linked, LGPL-compatible FFmpeg build only if its exact dependency graph
  passes intake. Do not enable GPL or `nonfree` components in an Apache-2.0 community package. The
  worker boundary does not remove FFmpeg source, notice, reverse-engineering, relinking, patent, or
  transitive-license obligations.
- Never use FFmpeg or a derivative implementation as evidence for Apple-authorized or strict
  delivery-qualified ProRes. Keep any such provider explicitly `apple_authorized=false` and
  `delivery_qualified=false`.
- Prefer the Apple-native AVFoundation/Core Media/VideoToolbox provider on macOS, but require runtime
  encoder/decoder discovery, actual hardware-use evidence, Bloom conformance fixtures, and written
  Apple confirmation before describing Bloom or an output preset as authorized.
- Evaluate MainConcept for authorized cross-platform ProRes ingest. Evaluate nablet mediaEngine and
  other Apple-listed providers for Windows/Linux encode only after written vendor and Apple answers
  establish local/OEM integration, profiles and alpha, MOV/MXF behavior, offline deployment,
  redistribution, support, and Bloom's authorization scope.
- Until an Apple-authorized and Bloom-qualified encoder exists on a platform, strict ProRes delivery
  returns typed `codec.prores.strict-provider-unavailable` there and never selects an
  FFmpeg-derived implementation. Offer a verified image-sequence plus BWF/BW64 audio handoff as an
  explicitly different analyzed preset, never as equivalent ProRes output or a substituted
  execution of the failed request.
- Keep conventional ProRes 422 Proxy/LT/422/HQ/4444/4444 XQ, optional 4444/4444 XQ alpha, and ProRes
  RAW/RAW HQ as distinct capability/preset families. MOV, RDD 44 MXF, and IMF mappings also qualify
  separately.
- Require counsel review for each shipped FFmpeg configuration, codec patent/trademark exposure,
  OS SDK use, commercial provider agreement, territory, and distribution model. Technical
  conformance does not answer those legal questions.
- Once the roadmap's first-proof gate admits media implementation and before provider intake, land
  only the dependency-free Qt-free `src/media` contract kernel: bounded descriptors and the four
  identity/evidence records; determinism and QC evidence values; exact provider-generation
  selection; fake providers and hostile fixtures; bounded worker framing over a fake transport; and
  a canonical image-sequence manifest. These foundations make no real codec-support claim and do
  not move media work ahead of the accepted deferral.

The detailed descriptors, pipelines, qualification matrix, test strategy, priorities, and vendor
questions live in [`../architecture/media-io.md`](../architecture/media-io.md).

## Consequences

- Bloom can add or replace codec implementations without changing durable media or render
  semantics.
- A provider crash, hang, dependency conflict, or malformed source can be contained without taking
  down the document/UI process.
- Commercial and native OS providers can coexist with a community implementation and share one
  artist-facing capability/report model.
- Import, export, authorization, and recipient delivery claims remain precise. Bloom will sometimes
  expose an honest unavailable capability where another application exposes a file that may fail
  later QC.
- Initial CPU copies cost throughput and memory. Zero-copy paths are added deliberately after
  correctness and measured transfer costs, rather than by leaking native surfaces into the model.
- Vulkan Video is a candidate for the operations and devices it enumerates, not a universal media
  backend or a ProRes solution. Windows, Linux, and macOS retain different native surface transports
  behind the same qualified external-surface lease and CPU reference behavior.
- Byte reproducibility, decoded-semantic conformance, Bloom technical qualification, external
  authorization, redistribution permission, and recipient acceptance remain visibly separate
  claims.
- Release engineering must build, sandbox, package, inventory, update, and test provider processes
  and their complete dependency graphs.
- Strict ProRes parity may remain asymmetric until Apple or an authorized vendor supplies workable
  terms across all targets. The fallback preserves high-quality work but does not erase that product
  gap.
- Broad video support remains a sequence of closed qualification profiles, not a one-time FFmpeg
  integration task.

## Rejected Alternatives

### Treat FFmpeg As The Media Architecture

FFmpeg is a valuable implementation candidate, but its types and defaults do not define Bloom's
time, color, alpha, metadata, security, or delivery contracts. Apple explicitly rejects deriving
authorized ProRes status from FFmpeg implementations.

### Use Only Native Platform Frameworks

This would create different codec coverage and subtle semantics across operating systems, make
headless conformance harder to compare, and still would not solve authorized ProRes encoding on
Windows and Linux.

### Load Every Provider In Process

This simplifies buffer sharing but couples Bloom's availability and address space to complex
untrusted parsers and independently versioned commercial SDKs. Narrow in-process exceptions may be
qualified later; they are not the default.

### Promise A Stable C++ Codec Plug-in ABI Now

C++ standard-library, compiler, SDK, and provider ABI differences would create an accidental public
compatibility promise before the lifecycle and fixture suite exist. A bounded process protocol is a
safer internal seam and can become public only deliberately.

### Call A File Valid When The Encoder Or Muxer Succeeds

Writer success does not prove stream timing, decoded pixels, alpha, color signaling, audio layout,
container structure, recipient policy, or disk durability. Reopen verification and preset-specific
QC remain mandatory.

### Requirement #6 evidence (2026-09-17)

The dependency-intake lane records FFmpeg 8.1.2 as a Linux worker-only candidate, not as an
accepted provider. The lock binds the official `ffmpeg-8.1.2.tar.xz` archive at
`sha256:464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`, its detached
signature, the same archive as corresponding source, `LGPL-2.1-or-later`, shared linkage, and the
exact non-GPL/nonfree configure allow-list. `dependencies/licenses/ffmpeg/` contains the license,
provenance, review, and security records; the offline dependency checker enforces the archive
metadata, schema 1.1 fields, and recipe/lock argument equality; and the isolated superbuild
verified the shared-library SONAME set with VA-API enabled. The review explicitly excludes
software H.264/HEVC encoding and treats FFmpeg ProRes as non-authorized preview output. Full
patent/counsel disposition, shipped-file SPDX SBOM, and final distribution-package review remain
gates for this ADR and are not implied by this intake evidence.

The amendment also proves the runtime-fetched-library boundary: a Bloom-owned six-symbol
`libopenh264.so.8` shim is installed beside every worker, including tests-disabled builds, and has
no codec implementation. Decode and fixture generation run with the shim and therefore do not
require Cisco's binary; only H.264 software encode gets the verified Cisco directory prepended.

## Acceptance Requirements

This ADR can become accepted when:

1. provider-neutral probe, stream, frame/audio, capability, execution, evidence, pipeline, QC, and
   provenance schemas have closed version 1 roles, purposes, determinism classes, vocabularies, and
   resource limits;
2. the exact-match registry and fake providers prove non-transitive qualification, typed
   unavailability, authority separation, and no mid-attempt substitution;
3. the worker protocol proves one-provider-trust-domain isolation plus
   crash/hang/cancellation/shutdown containment on all three targets;
4. one fake and one real provider pass deterministic probe, seek, decode, malformed-input, and
   source-change fixtures without provider types crossing the boundary;
5. one time-based export proves bounded back-pressure, preservation approval, encode/mux, close,
   reopen/semantic verification, QC evidence, and atomic publication;
6. FFmpeg's exact proposed configuration and distribution package pass dependency, LGPL, patent,
   security, and SBOM review, or FFmpeg is removed from the decision;
7. the Apple and vendor questions listed in the architecture document have written dispositions
   before any authorized/strict ProRes capability is advertised; and
8. cross-platform capability UI and headless reports distinguish unavailable, preview, conform,
   export, delivery, and attributed external authorization without ambiguity.

## Primary References

- [Professional media I/O architecture](../architecture/media-io.md)
- [Apple ProRes white paper](https://www.apple.com/final-cut-pro/docs/Apple_ProRes.pdf)
- [Apple ProRes RAW white paper](https://www.apple.com/final-cut-pro/docs/Apple_ProRes_RAW.pdf)
- [Apple authorized ProRes products](https://support.apple.com/en-us/118584)
- [Apple AVFoundation codec types](https://developer.apple.com/documentation/AVFoundation/AVVideoCodecType)
- [VideoToolbox encoder-list keys](https://developer.apple.com/documentation/videotoolbox/video-encoder-list-keys)
- [VideoToolbox actual hardware-encoder property](https://developer.apple.com/documentation/videotoolbox/kvtcompressionpropertykey_usinghardwareacceleratedvideoencoder)
- [SMPTE RDD 36:2022](https://pub.smpte.org/doc/rdd36/20220909-pub/rdd36-2022.pdf)
- [SMPTE RDD 44:2022](https://pub.smpte.org/pub/rdd44/rdd44-2022.pdf)
- [SMPTE ST 2086:2018](https://pub.smpte.org/pub/st2086/st2086-2018.pdf)
- [SMPTE ST 2094-1:2016](https://pub.smpte.org/pub/st2094-1/st2094-1-2016.pdf)
- [CTA-861.3-A HDR static metadata](https://www.cta.tech/standards/cta-8613-a/)
- [FFmpeg ProRes encoders](https://www.ffmpeg.org/ffmpeg-codecs.html#ProRes)
- [FFmpeg license and legal considerations](https://ffmpeg.org/legal.html)
- [MainConcept ProRes Decoder SDK](https://www.mainconcept.com/prores)
- [nablet mediaEngine](https://nablet.com/media-engine)
- [nablet mediaEngine 3.0 ProRes release claim](https://support.nablet.com/hc/en-us/articles/24011027768340-mediaEngine-v3-0-Release-Notes)
- [Microsoft Media Foundation supported formats](https://learn.microsoft.com/en-us/windows/win32/medfound/supported-media-formats-in-media-foundation)
- [Vulkan Video coding](https://docs.vulkan.org/spec/latest/chapters/videocoding.html)
- [Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html)
- [MoltenVK runtime feature list](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
- [Core Video Metal texture cache](https://developer.apple.com/documentation/CoreVideo/cvmetaltexturecache-q3j)
