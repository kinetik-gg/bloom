# ADR 0020: Qualified And Isolated Media Codec Providers

Status: proposed

Date: 2026-08-25

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
- Qualify ingest, preview, export, and named delivery profiles independently. Hardware and software
  implementations also qualify independently. Missing tuple fields are unavailable, not wildcards.
- Run general-purpose container and codec providers in supervised, resource-limited
  `bloom-media-worker` processes. Use a versioned bounded provider-neutral protocol and host-side
  revalidation. The same boundary may contain optional commercial providers without exposing their
  SDK types or ABI through Bloom.
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
- Make every time-based export follow: immutable snapshot and preset; preservation report and exact
  approval; bounded render/convert/encode/mux; private staging; close and reopen; structural,
  decoded-semantic, metadata, and required independent QC; then atomic publication. An encoder never
  writes the final target directly.
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
  is unavailable there. Offer a verified image-sequence plus BWF/BW64 audio handoff as an explicitly
  different fallback, never as equivalent ProRes output.
- Keep conventional ProRes 422 Proxy/LT/422/HQ/4444/4444 XQ, optional 4444/4444 XQ alpha, and ProRes
  RAW/RAW HQ as distinct capability/preset families. MOV, RDD 44 MXF, and IMF mappings also qualify
  separately.
- Require counsel review for each shipped FFmpeg configuration, codec patent/trademark exposure,
  OS SDK use, commercial provider agreement, territory, and distribution model. Technical
  conformance does not answer those legal questions.

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

## Acceptance Requirements

This ADR can become accepted when:

1. provider-neutral probe, stream, frame/audio, capability, provenance, and qualification schemas
   have closed version 1 vocabularies and resource limits;
2. the worker protocol proves crash/hang/cancellation/shutdown containment on all three targets;
3. one fake and one real provider pass deterministic probe, seek, decode, malformed-input, and
   source-change fixtures without provider types crossing the boundary;
4. one time-based export proves bounded back-pressure, preservation approval, encode/mux, close,
   reopen/semantic verification, QC evidence, and atomic publication;
5. FFmpeg's exact proposed configuration and distribution package pass dependency, LGPL, patent,
   security, and SBOM review, or FFmpeg is removed from the decision;
6. the Apple and vendor questions listed in the architecture document have written dispositions
   before any authorized/strict ProRes capability is advertised; and
7. cross-platform capability UI and headless reports distinguish unavailable, preview, conform,
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
- [FFmpeg ProRes encoders](https://www.ffmpeg.org/ffmpeg-codecs.html#ProRes)
- [FFmpeg license and legal considerations](https://ffmpeg.org/legal.html)
- [MainConcept ProRes Decoder SDK](https://www.mainconcept.com/prores)
- [nablet mediaEngine](https://nablet.com/media-engine)
- [nablet mediaEngine 3.0 ProRes release claim](https://support.nablet.com/hc/en-us/articles/24011027768340-mediaEngine-v3-0-Release-Notes)
- [Microsoft Media Foundation supported formats](https://learn.microsoft.com/en-us/windows/win32/medfound/supported-media-formats-in-media-foundation)
- [Vulkan Video coding](https://docs.vulkan.org/spec/latest/chapters/videocoding.html)
