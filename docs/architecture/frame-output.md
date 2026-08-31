# Frame Output And Publication

Status: accepted

Implementation status: the allocation-free Process Pixel Stream and Process-Frame Semantic
Identity version 1 codecs, cancellable owning semantic-identity preparer, closed facet-descriptor
grammar and validator, preset-specific owning OutputAnalysis analyzer/report, streaming analysis
digest, preset-specific bound-analysis products, the module-private Output Semantic Identity version
1 streaming serializer/preparer, portable golden vectors, and the flat OpenEXR adapter with its
semantic reopen verifier and verifier-product issuance are implemented. The PNG adapter and
end-to-end publication remain pending.

Updated: 2026-08-31

## Purpose

Bloom's first delivery surface renders one immutable composition frame to a deterministic PNG or a
flat OpenEXR. OpenImageIO (OIIO) and OpenEXR are private implementation libraries; Bloom owns the
request, preservation analysis, cancellation, diagnostics, verification, and filesystem
publication contracts.

Writing bytes is not proof that an export preserved its requested meaning. OIIO documents that an
output plugin may coerce unsupported data types, channel names, or metadata, and some formats cannot
represent Bloom's process image. Every staged output is therefore reopened and verified before it
can replace the destination.

## Input Boundary

Frame output consumes `ProcessFrame`, not a Viewer screenshot and not a display buffer
bundled into evaluation. The process frame owns finite premultiplied `lin_rec709_scene` `RGBA32F`,
signed data/display windows, pixel aspect, composition/time/output identity, document revision,
primitive-semantics versions, and process cache identity. Its color identity is exactly
`lin_rec709_scene`: scene-referred linear-light Rec.709 primaries with a D65 white point. A config
alias, role, display name, or approximate transform does not reinterpret that v1 process frame.

The two initial presets are closed, versioned contracts:

- `PngRgba8SrgbV1`
- `FlatExrRgba32fLinRec709SceneV1`

Their typed identity derives every portable string and version; callers never provide the fields
independently and enum ordinals are never serialized:

| Typed preset | Serialized preset ID | Version | Output pixel-semantics profile | Expected OCIO revision | Display identity |
| --- | --- | ---: | --- | --- | --- |
| `PngRgba8SrgbV1` | exact text `PngRgba8SrgbV1` | `1` | `bloom.output.png-rgba8-srgb.semantic.v1` | exactly 32 bytes | required |
| `FlatExrRgba32fLinRec709SceneV1` | exact text `FlatExrRgba32fLinRec709SceneV1` | `1` | `bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1` | zero bytes | zero bytes |

A mismatched preset ID, version, or profile tuple is invalid. For PNG, the separate expected OCIO
revision must equal both the revision embedded in the canonical `DisplayProcessorIdentity` and the
64 lowercase hexadecimal digits in the target external-dependency descriptor. The EXR preset
rejects a nonempty OCIO revision or display identity.

A preset version participates in analysis, semantic output identity, verification, and
reproducibility.
Adding half-float EXR, arbitrary channels, tiled or multipart EXR, non-square PNG, sequences, or
video requires another declared preset rather than changing version 1 silently.

### Process-Frame Semantic Identity Version 1

Output approval binds an evaluated product, not an address-bearing runtime cache key.
`ProcessFrameSemanticIdentity` therefore has a portable, closed codec. `u8`, `u16`, `u32`, and
`u64` are unsigned big-endian; `i64` is two's-complement big-endian. `text` is `u32(byteCount)`
followed by strict Unicode 15.1 NFC UTF-8 with no NUL; `bytes32` is exactly 32 bytes with no length
prefix. Project, composition, and output-node IDs are their checked unsigned 64-bit stable/raw
values; a platform `size_t` is never serialized.

First compute the process-pixel digest by streaming exact rows in increasing process Y, then
increasing X, with components in `R`, `G`, `B`, `A` order. Each component is its IEEE-754 binary32
bit pattern serialized as `u32`; no numeric conversion, NaN canonicalization, or host-endian memory
copy occurs:

```text
SHA-256(
  ASCII "BloomProcessPixelStream\0" || u16(1) ||
  u64(pixelCount) || pixelCount * (u32(Rbits) || u32(Gbits) || u32(Bbits) || u32(Abits))
)
```

The canonical semantic identity bytes are exactly:

```text
ASCII "BloomProcessFrameSemanticIdentity\0"
u16(1)
u64(project ID)
u64(composition ID)
u64(source document revision)
i64(normalized time numerator)
i64(positive normalized time denominator)
u64(output node ID)
u8(resolution kind)                    # 1 = composition format, 2 = proxy
if resolution kind is 2:
  u32(requested proxy width)
  u32(requested proxy height)
i64(data-window origin X)
i64(data-window origin Y)
u32(data-window width)
u32(data-window height)
i64(display-window origin X)
i64(display-window origin Y)
u32(display-window width)
u32(display-window height)
u32(pixel-aspect numerator)
u32(pixel-aspect denominator)
text(process Color Interop ID)         # exactly "lin_rec709_scene" in version 1
u8(pixel packing)                      # 1 = packed RGBA binary32
u8(alpha association)                  # 1 = premultiplied
u8(evaluation quality)                 # 1 = reference
text(process pixel-semantics profile ID)
u32(compiled-plan semantics version)
u32(animation-sampling semantics version)
u32(evaluator semantics version)
u32(image-primitive semantics version)
bytes32(process-pixel digest)
```

The version 1 process pixel-semantics profile is exactly
`bloom.process.rgba32f.semantic.v2`. CPU, GPU, compiler, target, and dependency builds may use that
profile only when conformance proves the exact same process bits. Their provider, target triple,
compiler, dependency-lock digest, and qualified-prefix digest form separate
`ProcessExecutionProvenance`, retained for diagnostics and local cache partitioning but excluded
from the portable semantic identity. Request generation, cancellation, priority, byte budgets,
task owner, pointer identity, and cache residency enter neither record. Unless semantic-identity
preparation and pixel-stream verification succeed for the captured frame, no output analysis is
approvable.

The version 1 public application API does not expose synchronous process-pixel hashing or a public
canonical-identity writer. A CPU worker stage receives one
`std::shared_ptr<const ProcessFrame>` and is the only producer of a private-constructible,
immutable `ProcessFrameSemanticIdentityV1` handle. That product retains the exact process frame,
the canonical identity bytes derived from it, and the process-pixel digest as one inseparable
lifetime. Its frame and canonical-byte views are available only from a retained lvalue handle;
neither may be detached and paired with another frame.

The preparer performs checked descriptor, layout, dimension, pixel-count, and byte preflight before
reading any pixel or allocating variable-size storage. It then streams pixels in bounded chunks,
checking cancellation between chunks and reporting monotonic pixel progress, and publishes the
identity handle only after the complete digest and canonical bytes have been produced. Cancellation,
allocation failure, a resource-limit failure, or any invariant failure publishes no partial
identity. The immutable handle is the version 1 reuse unit: an analysis and its approved export may
share it, but version 1 has no global identity cache, shared in-flight wait, or scheduler-level
deduplication. Scheduler coalescing may supersede obsolete attempts; it must not make independent
consumers share cancellation or wait on one another.

## Preservation And Loss Report

Before a file task is admitted, the output adapter produces a deterministic `OutputAnalysis` with
a typed report. Each facet has one preservation state:

| State | Meaning |
| --- | --- |
| `Exact` | The preset-defined target preserves the source without numeric or semantic loss, or realizes a target-only requirement exactly |
| `Equivalent` | Standardized representation differs but has the same declared meaning |
| `Approximated` | A bounded, declared conversion changes values or representation |
| `Omitted` | Source information is intentionally not written |
| `ExternalReference` | Meaning depends on a named external standard/configuration |
| `Missing` | Required information or dependency is unavailable |
| `Unsupported` | The preset cannot represent or convert the source under its contract |

The report covers at least pixels, precision, color, alpha association, channel names/roles, data
window, display window, pixel aspect, compression, metadata, and external dependencies. Every
non-exact facet carries a stable code, source/target values, and whether the preset permits it.

Analysis exposes a typed report before it exposes an approvable product. A deterministic report may
describe `Missing` or `Unsupported` inputs and remain useful to Jobs, scripting, headless clients,
and the UI without having an approval digest. `OutputAnalysisDigest` becomes available only when a
validated frame-bound process identity exists and, for PNG, a matching expected OCIO revision and
validated canonical `DisplayProcessorIdentity` exist. Approval requires both that digest and all
eleven derived permission bits. Bloom never invents an empty PNG display identity or a placeholder
process identity merely to hash a failure report.

### Pre-Approval Output Analysis Attempt

The application output service owns one bounded output-analysis-attempt task group for a captured
document revision, output request, preset, and target. Before asking an artist or headless policy to
approve anything, its dependent worker stages perform, in order:

1. cheap blocking-I/O target preflight, including canonical target identity and no-follow observed
   destination state;
2. CPU evaluation or exact immutable `ProcessFrame` reuse under the closed output limits;
3. cancellable CPU preparation of the frame-bound `ProcessFrameSemanticIdentityV1`;
4. for PNG, bounded color/config resolution plus preparation or exact reuse of the qualified
   display processor and its canonical `DisplayProcessorIdentity`; EXR has no display product;
5. creation and self-validation of the immutable owning report; and
6. streaming computation of `OutputAnalysisDigest` from that report and the retained identity
   products.

The application controller submits each dependent stage only after consuming its predecessor's
typed result; a worker never waits on another task or on the UI. A failed or unavailable stage may
still produce the truthful nonapprovable report defined below, but it never produces a placeholder
identity or approval digest. A completed approvable `OutputAnalysisAttempt` is an immutable owning
product that retains the target-preflight result, the frame-bound process-identity handle, the
qualified PNG display-processor handle and identity when applicable, the owning report, and its
exact digest.

The digest implementation consumes the process-identity product and derives source descriptors
from that product's retained frame; it accepts neither a second process frame nor arbitrary
canonical identity bytes. Recomputing pixel hashes or substituting an equivalent-looking frame or
processor at approval or export is forbidden. These ownership rules do not change the frozen
version 1 digest bytes.

The output service retains and charges the completed attempt while the artist decides; a panel only
observes it and may issue approve, dismiss, or supersede intent. Headless policy uses the same owned
product. Dismissal or supersession cancels unfinished work and releases the completed attempt and
its reservations when no admitted export retains them. A disposable panel never owns the attempt,
its task group, its retained frame, or its resource reservation.

### Analyzer And Owning Report Contract

Version 1 has two public preset-specific analyzer entry points:
`analyzePngRgba8SrgbV1(PngRgba8SrgbAnalysisInputV1)` and
`analyzeFlatExrRgba32fLinRec709SceneV1(FlatExrRgba32fLinRec709SceneAnalysisInputV1)`. There is no
public entry point that accepts a preset enum plus a union of optional fields. The PNG input contains
its expected 32-byte OCIO revision as a required value and its typed color-resolution result; the
EXR input contains neither field. Absence of the PNG expected revision is therefore an
input-construction failure, not a report facet. This shape prevents a caller from constructing PNG
without its structural dependencies or attaching PNG-only state to EXR.

Both inputs carry one typed process source. Its `Ready` arm owns or retains the immutable process
frame from which the source descriptor is derived. Its `Missing` arm still contains a validated
`Rgba32fImageDescriptor`, but states that the immutable pixel payload or semantic identity is not
available. The source descriptor is never optional and is never invented from target defaults.
Process `Missing` changes only the Pixels facet to `Missing`, `process-frame.missing`; windows,
pixel aspect, precision, color, alpha, channels, compression, metadata, and dependencies continue
to derive independently from the known source descriptor and their own typed inputs.

Analyzer inputs use only these closed states:

| Input | Version 1 states | Derivation |
| --- | --- | --- |
| process | `Ready`, `Missing` | `Missing` affects only Pixels as specified above |
| adapter | `Qualified`, `Unavailable` | `Unavailable` produces external `adapter.unavailable`; capability states `Development` and `Unqualified` are mapped to this input state before analysis |
| compression | `Available`, `Unavailable` | `Unavailable` changes only Compression to `compression.unavailable` |
| other output dependency | `Available`, `Missing` | `Missing` contributes external `dependency.missing` |
| PNG color resolution | `Ready`, `Missing`, `Changed`, `Invalid`, `MissingResource`, `UnsupportedVersion` | non-ready states map respectively to `ocio.missing`, `ocio.changed`, `ocio.invalid`, `ocio.resource-missing`, and `ocio.version-unsupported` on Color and contribute external `dependency.missing` |

An enum value outside these sets, an invalid source descriptor, or another structurally impossible
combination fails analyzer construction. A resolved PNG configuration whose helper, processor, or
execution provider cannot run is an adapter-execution failure: Color keeps its `Ready` nominal
tuple while External Dependencies uses `adapter.unavailable`. Configuration-resolution failures
retain their exact `ocio.*` Color code and contribute `dependency.missing`; they are not rewritten
as adapter failures.

The analyzer generates a non-default-constructible, move-only `OutputAnalysisReportV1`. It owns an
immutable fixed set of eleven semantic assessments and bounded descriptor storage; each source or
target descriptor is at most 1024 ASCII bytes and the twenty-two descriptors together contain at
most `22528` bytes. It exposes a borrowed report view only from `const&`; the rvalue view overload is
deleted. It also exposes its derived permission mask and approvability by value. There is no public
mutable assessment access, general report builder, or constructor that accepts caller-chosen
state/code/permission tuples. Long-lived UI, Jobs, Python, and headless consumers retain exactly
`std::shared_ptr<const OutputAnalysisReportV1>`.

Localized explanations and suggested actions are mapped from preset, facet, and stable code outside
the semantic report. They are neither retained by `OutputAnalysisReportV1` nor serialized into its
digest. Every generated owning report validates its own borrowed view with the closed validator
before it is returned or shared. A nonapprovable but valid report is a successful analysis attempt;
allocation failure, invalid input, or a generated-report invariant failure are typed analyzer
failures and never publish a partial report.

Facet derivation is independent except for the declared external-dependency aggregation. Its exact
precedence is `resource.limit-exceeded` over `adapter.unavailable` over `dependency.missing` over the
nominal PNG `png.ocio-external-reference` or EXR Exact tuple. `dependency.missing` includes a
non-`Ready` PNG color resolution and an explicitly missing other output dependency. Window and
pixel-aspect failures remain independently truthful; in particular, `window.out-of-range` and
`resource.limit-exceeded` may coexist. The generic `*.unsupported` codes remain accepted by the
version 1 validator for the frozen wire vocabulary, but these preset-specific analyzers cannot emit
them until a future typed source or target contract represents the corresponding unsupported case.

Analyzer tests exhaust both nominal presets; every closed input state and invalid enum; Ready and
Missing process sources with a retained descriptor; exact-limit and over-limit dimensions and pixel
counts; PNG color-code mapping; adapter, compression, and other-dependency isolation; every external
precedence edge; coexistence of window and resource failures; allocation and internal-invariant
failure without partial publication; move/view/shared-ownership lifetimes; self-validation; and the
unchanged digest golden vectors.

Canonical ordering and serialization produce an `OutputAnalysisDigest`. A caller cannot reduce this
to `acceptLoss = true`. Publication requires an immutable request that binds the exact digest the
artist or headless policy approved. If source identity, preset, config, output pixel-semantics
profile, display-processor identity, or report changes, approval no longer matches and analysis runs
again. A target build-provenance change alone does not alter portable approval when both builds are
qualified for the same semantics profile.

`OutputAnalysisDigest` serialization version 1 is closed. `u16`, `u32`, and `u64` are unsigned
big-endian. `text` is `u32(byteCount)` followed by strict Unicode 15.1 NFC UTF-8 with no NUL.
`bytes` is
`u32(byteCount)` followed by exact bytes. The digest is SHA-256 over fields in this exact order:

```text
ASCII "BloomOutputAnalysisDigest\0"
u16(1)
bytes(versioned canonical process-frame identity)
text(preset ID)
u32(preset version)
text(output pixel-semantics profile ID)
bytes(expected OCIO revision: 32 bytes, or zero bytes when not required)
bytes(versioned canonical DisplayProcessorIdentity, or zero bytes for process EXR)
u16(11)
11 facet records in fixed facet-ID order
```

Facet IDs are `1` pixels, `2` precision, `3` color, `4` alpha association, `5` channel names/roles,
`6` data window, `7` display window, `8` pixel aspect, `9` compression, `10` metadata, and `11`
external dependencies. A facet record is `u8(facetId)`, `u8(state)`, `u8(presetPermits)`, then
`text(stableCode)`, `text(canonicalSourceDescriptor)`, and `text(canonicalTargetDescriptor)`.
State values in table order above are `1` through `7`; `presetPermits` is exactly `0` or `1`.
Localized artist explanations and suggested actions are mapped outside the semantic report and are
excluded from the approval digest, so changing UI language cannot change semantic approval.

An `Exact` facet uses the empty stable-code string; every other state requires a non-empty stable
code from this closed version 1 vocabulary:

| Facet | Permitted non-empty stable codes |
| --- | --- |
| pixels | `png.display-transform-clamp-quantize`, `process-frame.missing`, `pixels.unsupported` |
| precision | `png.float32-to-uint8`, `precision.unsupported` |
| color | `png.lin-rec709-scene-to-srgb`, `ocio.missing`, `ocio.changed`, `ocio.invalid`, `ocio.resource-missing`, `ocio.version-unsupported`, `color.unsupported` |
| alpha association | `png.premultiplied-to-straight`, `alpha.unsupported` |
| channel names/roles | `channels.unsupported` |
| data window | `png.origin-window-required`, `window.out-of-range` |
| display window | `png.equal-window-required`, `window.out-of-range` |
| pixel aspect | `png.square-pixel-required`, `exr.par-rounded-binary32`, `pixel-aspect.unsupported` |
| compression | `compression.unavailable`, `compression.unsupported` |
| metadata | `metadata.unsupported` |
| external dependencies | `png.ocio-external-reference`, `dependency.missing`, `adapter.unavailable`, `resource.limit-exceeded` |

Codes are valid only for their listed facet; another code or a future code requires a new analysis
serialization version. Every code is the exact non-localized ASCII spelling shown. State and
permission are derived from preset and code; callers cannot choose them:

| Code set | Required state | Valid preset | `presetPermits` |
| --- | --- | --- | ---: |
| empty code | `Exact` | both | `1` |
| `png.display-transform-clamp-quantize`, `png.float32-to-uint8`, `png.lin-rec709-scene-to-srgb`, `png.premultiplied-to-straight` | `Approximated` | PNG | `1` |
| `exr.par-rounded-binary32` | `Approximated` | EXR | `1` |
| `png.ocio-external-reference` | `ExternalReference` | PNG | `1` |
| `process-frame.missing`, `compression.unavailable`, `dependency.missing`, `adapter.unavailable`, `resource.limit-exceeded` | `Missing` | both | `0` |
| `ocio.missing`, `ocio.changed`, `ocio.invalid`, `ocio.resource-missing`, `ocio.version-unsupported` | `Missing` | PNG | `0` |
| `pixels.unsupported`, `precision.unsupported`, `color.unsupported`, `alpha.unsupported`, `channels.unsupported`, `window.out-of-range`, `pixel-aspect.unsupported`, `compression.unsupported`, `metadata.unsupported` | `Unsupported` | both | `0` |
| `png.origin-window-required`, `png.equal-window-required`, `png.square-pixel-required` | `Unsupported` | PNG | `0` |

The five `ocio.*` codes map one-to-one from the corresponding typed color-resolution failures.
An unknown code/state/preset/permission tuple is structurally invalid, not merely non-approvable.
`Equivalent` and `Omitted` have no valid tuple in version 1; the process frame owns no optional
artist/application metadata to omit. Approval is forbidden when any permission bit is zero,
regardless of caller policy; a headless policy can approve only the exact digest of a report whose
eleven derived permission bits are all `1`.

Source and target descriptors use one canonical ASCII grammar. A descriptor is empty only when the
preset's facet schema declares that side absent. Otherwise it is semicolon-separated `key=value`
fields whose keys are strictly increasing ASCII byte strings matching `[a-z][a-z0-9-]*`; missing,
unknown, duplicate, or out-of-order keys are invalid. Values have exactly one tag:

```text
b:0 | b:1
i:<minimal signed decimal>
u:<minimal unsigned decimal>
r:<normalized signed numerator>/<positive denominator>
f32:<8 lowercase hexadecimal IEEE-754 bits>
f64:<16 lowercase hexadecimal IEEE-754 bits>
id:<ASCII [A-Za-z0-9._:/-]+>
utf8:<lowercase hexadecimal bytes of strict Unicode 15.1 NFC UTF-8>
```

Minimal signed decimal is exactly `0` or `-?[1-9][0-9]*`; minimal unsigned decimal is `0` or
`[1-9][0-9]*`. A rational is reduced, has a positive denominator, and represents zero only as
`0/1`. Hexadecimal carries no `0x` prefix.

Each preset version closes the required descriptor keys for each facet: pixels use `height`,
`packing`, `sample-type`, `width`; precision uses `component-type`; color uses `color-id`; alpha
uses `association`, `zero-alpha`; channels use `count` followed by zero-based `name-N` and `role-N`;
either window uses `height`, `origin-x`, `origin-y`, `width`; pixel aspect uses `denominator`,
`numerator` for the source and `value` (`f32`) for an EXR target; compression uses `method`;
metadata uses `profile`; external dependencies use `kind` and `revision`. Optional information is
represented by an explicit `id:none`, not an undeclared field. List indices are minimal decimal and
contiguous. The qualified dependency profile pins Unicode normalization fixtures.

The descriptor schema on each side is exact:

| Facet | Source for both presets | PNG target | EXR target |
| --- | --- | --- | --- |
| pixels | `Pixels` | `Pixels` | `Pixels` |
| precision | `Precision` | `Precision` | `Precision` |
| color | `Color` | `Color` | `Color` |
| alpha association | `AlphaAssociation` | `AlphaAssociation` | `AlphaAssociation` |
| channels | `Channels` | `Channels` | `Channels` |
| data window | `Window` | `Window` | `Window` |
| display window | `Window` | `Window` | `Window` |
| pixel aspect | `PixelAspectRational` | `PixelAspectRational` | `PixelAspectBinary32` |
| compression | `Absent` | `Compression` | `Compression` |
| metadata | `Metadata` | `Metadata` | `Metadata` |
| external dependencies | `ExternalDependencies` | `ExternalDependencies` | `ExternalDependencies` |

The following spellings close version 1. `W` and `H` are the process data-window extent;
`DX/DY/DW/DH` and `SX/SY/SW/SH` are the signed origins and unsigned extents of the process data and
display windows; `N/D` is the positive reduced process pixel aspect. Substitution emits the tagged
minimal decimal form, not the placeholder letters. Source descriptors in facet order are exactly:

```text
height=u:H;packing=id:rgba;sample-type=id:binary32;width=u:W
component-type=id:binary32
color-id=id:lin_rec709_scene
association=id:premultiplied;zero-alpha=id:canonical-zero
count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha
height=u:DH;origin-x=i:DX;origin-y=i:DY;width=u:DW
height=u:SH;origin-x=i:SX;origin-y=i:SY;width=u:SW
denominator=u:D;numerator=u:N
""
profile=id:none
kind=id:none;revision=id:none
```

The `""` notation marks the `Absent` compression source; the quote characters are not serialized
and the descriptor has zero bytes. `canonical-zero` means exact positive-zero RGB whenever alpha is
exact zero. The channel descriptor expresses semantic `R,G,B,A` order; EXR's physical lexical
`A,B,G,R` header order is a file-profile rule and does not reorder this descriptor.
`profile=id:none` describes optional artist/application metadata; mandatory PNG signaling and EXR
header/color attributes remain part of their color and output-profile contracts.
`kind=id:none` means no unresolved output-time dependency, not an absence of evaluation lineage.
`process-frame.missing` means the requested, validated process image descriptor remains known but
its immutable pixel payload or semantic identity is unavailable; it never licenses invented source
dimensions or descriptors.

PNG target descriptors in the same facet order are exactly:

```text
height=u:H;packing=id:rgba;sample-type=id:uint8;width=u:W
component-type=id:uint8
color-id=id:srgb_rec709_display
association=id:straight;zero-alpha=id:canonical-zero
count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha
height=u:H;origin-x=i:0;origin-y=i:0;width=u:W
height=u:H;origin-x=i:0;origin-y=i:0;width=u:W
denominator=u:1;numerator=u:1
method=id:deflate-level-6-filter-none
profile=id:none
kind=id:ocio;revision=id:R
```

The PNG target always describes its implicit zero-origin window using the process data extent,
including in a report that the preset does not permit. `deflate-level-6-filter-none` names the exact
level-6, default-strategy, filter-zero policy below. `R` is substituted by exactly 64 lowercase
hexadecimal digits of the expected OCIO revision.

EXR target descriptors in the same facet order are exactly:

```text
height=u:H;packing=id:rgba;sample-type=id:binary32;width=u:W
component-type=id:binary32
color-id=id:lin_rec709_scene
association=id:premultiplied;zero-alpha=id:canonical-zero
count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha
height=u:DH;origin-x=i:DX;origin-y=i:DY;width=u:DW
height=u:SH;origin-x=i:SX;origin-y=i:SY;width=u:SW
value=f32:BITS
method=id:zip
profile=id:none
kind=id:none;revision=id:none
```

`BITS` is substituted by the eight lowercase hexadecimal digits of the rounded binary32 pixel
aspect. EXR pixel aspect is `Exact` only when the exact rational value of that binary32 equals
`N/D`; otherwise it is permitted `Approximated`. Descriptor and digest intake is bounded: each
source or target descriptor is at most 1024 ASCII bytes, a version 1 process identity is exactly
249 or 257 bytes, and the complete canonical analysis-digest preimage is at most 4 MiB
(`4194304` bytes). A longer descriptor returns the distinct `DescriptorTooLong` validation result.
All size arithmetic is checked before allocation or caller-buffer mutation. Digesting streams the
components and never requires allocating the complete preimage. Its API accepts the retained
frame-bound `ProcessFrameSemanticIdentityV1` product and the validated typed display-identity
product, never a separate process frame or arbitrary caller-provided identity byte spans.

Nominal analysis is derived exactly as follows:

| Facet | PNG | EXR |
| --- | --- | --- |
| pixels | `Approximated`, `png.display-transform-clamp-quantize` | `Exact` |
| precision | `Approximated`, `png.float32-to-uint8` | `Exact` |
| color | `Approximated`, `png.lin-rec709-scene-to-srgb` when the processor is ready | `Exact` |
| alpha association | `Approximated`, `png.premultiplied-to-straight` | `Exact` |
| channels | `Exact` | `Exact` |
| data window | `Unsupported`, `window.out-of-range` if `W` or `H` exceeds `2147483647`; otherwise `Exact` iff source equals the PNG target, else `Unsupported`, `png.origin-window-required` | `Exact` iff the inclusive bounds fit signed 32-bit; otherwise `Unsupported`, `window.out-of-range` |
| display window | same PNG extent limit; otherwise `Exact` iff source equals the PNG target, else `Unsupported`, `png.equal-window-required` | same EXR bounds rule as data window |
| pixel aspect | `Exact` iff `1/1`; otherwise `Unsupported`, `png.square-pixel-required` | `Exact` or permitted `Approximated`, `exr.par-rounded-binary32` |
| compression | `Exact` | `Exact` |
| metadata | `Exact` | `Exact` |
| external dependencies | `ExternalReference`, `png.ocio-external-reference` | `Exact` |

Report version 1 also derives the export hard-limit result from its canonical descriptors. The
limit is exceeded iff any source or target pixel width or height, or any source or target data- or
display-window extent, exceeds `32768`, or checked multiplication of the source pixel width and
height exceeds `67108864` (`2^26`). Values exactly at either limit do not exceed it. The pixel
relationship rules above require the target extent and the source data-window extent to match the
source pixels, but validation still checks every named descriptor side directly and performs the
pixel-count multiplication with overflow detection.

When that derived condition is true, the external-dependencies facet must be `Missing`,
`resource.limit-exceeded`; its nominal PNG external-reference or EXR exact tuple and every other
unavailable-dependency code are invalid. When the condition is false, `resource.limit-exceeded` is
invalid; the nominal tuple or another otherwise-valid unavailable-dependency code remains valid.
This relationship is independent of format window representability. A report can therefore require
`window.out-of-range` on a data/display facet and `resource.limit-exceeded` on external dependencies
at the same time. Neither code substitutes for or suppresses the other.

`png.equal-window-required` means specifically that the source display window differs from the PNG
implicit target `(0,0,W,H)`. Thus equal source data/display windows at a nonzero origin do not
silently pass.

The process-frame and display-processor identity byte records carry their own leading serialization
version. An adapter that cannot provide those closed canonical bytes cannot produce an approvable
analysis. No JSON object order, locale, host-path spelling, pointer, timestamp, or
implementation-defined enum representation enters this digest.

The output pixel-semantics profile is exactly `bloom.output.png-rgba8-srgb.semantic.v1` for PNG and
`bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1` for EXR. Target-specific writer/reader
versions, compiler and target triple, dependency-lock digest, qualified-prefix digest, and build
options form separate `OutputExecutionProvenance`. They remain in capability reports, diagnostics,
and local cache partitioning, but not in the portable analysis digest. A build may advertise one of
the shared profile IDs only after cross-platform semantic fixtures pass; otherwise it needs a
distinct profile ID or remains unqualified.

The same typed report powers the render dialog, Jobs surface, logs, scripting, and headless JSON.

## PNG Preset Version 1

`PngRgba8SrgbV1` is deliberately narrow:

- one non-interlaced RGBA image whose width and height are each in `1..2147483647`, with unsigned
  8-bit channels in `R`, `G`, `B`, `A` order;
- straight/unassociated alpha;
- a qualified OCIO display/output processor whose output resolves to the Color Interop ID
  `srgb_rec709_display`;
- origin `(0, 0)`, identical data and display windows, and square pixel aspect;
- deterministic clamp and byte packing after the qualified transform;
- no dithering in version 1;
- DEFLATE level `6`, default zlib strategy, and PNG row-filter type `0` (`None`) for every row; and
- no timestamps, host paths, usernames, application-session data, or other volatile metadata.

Bloom explicitly unpremultiplies process RGB, transforms RGB only, preserves alpha, clamps at the
declared display boundary, and packs the final straight RGBA bytes. The OIIO writer receives already
prepared bytes and the `oiio:UnassociatedAlpha=1` control so it must not unpremultiply a second time.
`oiio:ColorSpace=srgb_rec709_display` requests the standardized output intent from the plugin, but
Bloom's chunk verifier—not the writer attribute—decides whether the file conforms.

The OCIO processor's binary32 RGB outputs and the unchanged binary32 alpha must be finite. Each
component is clamped to `[0, 1]`, converted exactly to binary64, multiplied by `255`, then quantized
as `floor(value + 0.5)`. An exact halfway case therefore selects the higher unsigned code. Signed
zero maps to `0`; no dithering, host rounding mode, fused contraction, or library transfer function
participates. This packing rule applies after the qualified processor has produced
`srgb_rec709_display`; Bloom does not apply a second transfer curve.

The v1 PNG structure and signaling profile is exact. After the eight-byte PNG signature, chunks
appear only in this order: one `IHDR`, one `sRGB`, one or more contiguous `IDAT`, then one `IEND`.
`IHDR` declares bit
depth 8, color type 6 (RGBA), compression method 0, filter method 0, and interlace method 0. `sRGB`
has length 1 and rendering intent byte 0 (perceptual). Every chunk length and CRC is checked.
The concatenated `IDAT` payload is one zlib stream with compression method 8 and 32 KiB window; after
decompression every scanline begins with filter byte `0`. The exact compressed bytes and `IDAT`
chunk boundaries remain execution-provenance evidence, not portable semantic identity.

Every other chunk is forbidden in v1, including `PLTE`, `gAMA`, `cHRM`, `iCCP`, `cICP`, `sBIT`,
`bKGD`, `hIST`, `tRNS`, `pHYs`, `sPLT`, `eXIf`, `tIME`, `tEXt`, `zTXt`, and `iTXt`. In particular,
Bloom does not emit redundant gamma/chromaticity or a competing ICC/CICP declaration beside the
required `sRGB` chunk. If the pinned OIIO/libpng build cannot emit this exact profile and satisfy the
fixed compression/filter policy, `PngRgba8SrgbV1` remains `Development` or `Unavailable`;
it must not be reported as `Qualified` and no production publication may use that preset.

Negative and HDR process RGB, Float32 precision, non-square pixel aspect, and distinct windows are
not silently discarded. The report marks the pixel/color/precision conversions `Approximated`; a
non-square aspect or differing window is `Unsupported` for this preset. Missing or unqualified OCIO
configuration is `Missing` and blocks staging.

Verification scans the raw chunk structure, reopens the staged PNG with the pinned reader, checks
dimensions, channels, type, straight-alpha interpretation, and the exact signaling profile, decodes
all pixels, and compares exact RGBA8 bytes to the prepared output. Writer-only control attributes
are not expected to round-trip as file metadata; their semantic effect is what verification proves.

## Flat OpenEXR Preset Version 1

`FlatExrRgba32fLinRec709SceneV1` writes:

- one flat, single-part, scanline image;
- channels named exactly `R`, `G`, `B`, `A`, each Float32 with sampling `1 x 1`;
- canonical premultiplied process samples without a display transform, clamp, quantization, or
  straight-alpha conversion;
- exact signed data and display windows after checked conversion to the OpenEXR coordinate domain;
- increasing-Y scanline order and lossless ZIP compression;
- the pixel-aspect rational rounded once to the OpenEXR Float32 attribute; and
- `colorInteropID=lin_rec709_scene` plus the exact Rec.709/D65 chromaticity bits defined below.

The version-field value is OpenEXR version `2` with every feature flag clear: regular single-part
scanline storage, short names, non-deep, non-tiled, non-multipart. The header contains exactly these
attributes and types; any additional attribute, including `name`, `type`, `version`, `chunkCount`,
time code, owner, comments, software, capture date, or host data, fails preset verification:

| Attribute | OpenEXR type | Required version 1 value |
| --- | --- | --- |
| `channels` | `chlist` | entries in lexical order `A`, `B`, `G`, `R`; each `FLOAT`, `pLinear=0`, x/y sampling `1/1` |
| `compression` | `compression` | `ZIP_COMPRESSION` |
| `dataWindow` | `box2i` | checked inclusive signed-32 bounds corresponding exactly to the process data window |
| `displayWindow` | `box2i` | checked inclusive signed-32 bounds corresponding exactly to the process display window |
| `lineOrder` | `lineOrder` | `INCREASING_Y` |
| `pixelAspectRatio` | `float` | checked binary32 conversion of the positive source rational |
| `screenWindowCenter` | `v2f` | `(0, 0)`, bits `00000000 00000000` |
| `screenWindowWidth` | `float` | `1`, bits `3f800000` |
| `chromaticities` | `chromaticities` | values below, in OpenEXR red/green/blue/white order |
| `colorInteropID` | `string` | exact UTF-8 bytes `lin_rec709_scene` |

The `chromaticities` binary32 bit patterns are exact: red `(3f23d70a, 3ea8f5c3)`, green
`(3e99999a, 3f19999a)`, blue `(3e19999a, 3d75c28f)`, and D65 white
`(3ea01a37, 3ea872b0)`. They are the round-to-nearest, ties-to-even binary32 encodings of Rec.709
`(0.64, 0.33)`, `(0.30, 0.60)`, `(0.15, 0.06)` and D65 `(0.3127, 0.3290)`; implementations write
the bits, not locale-parsed decimal text.

The pixel-aspect rational is converted once to binary32 using IEEE-754 round-to-nearest,
ties-to-even, with gradual underflow and preserved signed zero. NaN, infinity, a non-positive
result, or finite overflow fails before staging. The same conversion rule governs any declared
binary64-to-binary32 output boundary; it must not inherit ambient rounding or flush subnormals.
Process samples are already binary32 and are copied bit-for-bit rather than numerically converted.

Version 1 additionally bounds every data- and display-window coordinate to a magnitude strictly
below `1073741823` (`INT32_MAX/2`) — the validated coordinate ceiling the qualified OpenEXR
encoder itself enforces. A window outside that checked ceiling fails typed before staging rather
than surfacing a library error; the full signed-32 wording above is therefore bounded by this
explicit version 1 ceiling.

The report marks process pixels, alpha, channels, and windows `Exact`. The pixel-aspect facet is
`Exact` only when the stored Float32 round-trips to the source rational; otherwise it is
`Approximated` and records both values. Float32 samples are compared bit-for-bit after reopen,
including signed zero and finite negative/HDR values where the process contract preserves them.

Version 1 does not claim arbitrary channels, subsampling, half precision, tiled, multipart, deep,
display-referred, or ACES-container output. Unsupported source channels or semantics remain visible
in the report and are never folded into RGBA implicitly.

Verification reopens the staged EXR and checks part count/type, channel names/types/sampling,
data/display windows, pixel aspect, compression, line order, `colorInteropID`, chromaticities, and
the exact header allowlist before streaming and comparing all channel samples.

## Determinism And Portable Output Identity

Bloom's portable output-determinism claim is semantic, not an assertion that every conforming
compression library emits identical artifact bytes. Under the same immutable process frame,
approved `OutputAnalysisDigest`, preset/version, output pixel-semantics profile, and applicable OCIO
config and processor identities, reopening the staged artifact must produce the same declared pixel
samples and metadata:

- PNG compares the exact prepared RGBA8 sample stream, dimensions, channel/alpha interpretation,
  and required/forbidden chunk semantics;
- EXR compares exact Float32 channel bits plus the versioned channel, window, pixel-aspect,
  compression-mode, line-order, color, and permitted metadata records.

The portable `OutputSemanticIdentity` has one closed streaming serializer. `u8`, `u16`, `u32`, and
`u64` are unsigned big-endian; `i32` and `i64` are two's-complement big-endian; `text` and `bytes`
have the definitions above. It is SHA-256 over these exact bytes:

```text
ASCII "BloomOutputSemanticIdentity\0"
u16(1)
bytes32(approved OutputAnalysisDigest)
bytes(versioned ProcessFrameSemanticIdentity)
text(preset ID)
u32(preset version)
text(output pixel-semantics profile ID)
bytes(versioned DisplayProcessorIdentity, or zero bytes for process EXR)
u8(reopened semantic payload kind)    # 1 = PNG RGBA8, 2 = flat EXR RGBA32F
the exact kind-specific payload below
```

PNG kind `1` appends:

```text
u32(width)
u32(height)
u8(1)                                # packed RGBA8
u8(1)                                # straight alpha
text("srgb_rec709_display")
u8(0)                                # PNG sRGB rendering-intent byte
text("png.ihdr-srgb-idat-iend.v1")  # verified required/forbidden chunk profile
u64(checked width * height * 4)
exact reopened RGBA8 bytes in increasing row Y, increasing X, R/G/B/A order
```

EXR kind `2` appends:

```text
i32(data xMin) || i32(data yMin) || i32(data xMax) || i32(data yMax)
i32(display xMin) || i32(display yMin) || i32(display xMax) || i32(display yMax)
u32(pixelAspectRatio binary32 bits)
u8(1)                                # ZIP_COMPRESSION
u8(1)                                # INCREASING_Y
text("lin_rec709_scene")
8 * u32(chromaticities bits in red/green/blue/white x/y order)
text("exr.singlepart-scanline-rgba32f.v1")
u64(pixelCount)
for increasing Y, then increasing X:
  u32(Rbits) || u32(Gbits) || u32(Bbits) || u32(Abits)
```

The symbolic `u8` values are Bloom serializer values, not casts of library enums. The serializer
explicitly excludes compressed `IDAT` or EXR block bytes, zlib implementation details, physical
chunk offsets, writer/reader execution provenance, attribute storage order, and the complete-file
artifact SHA-256. Reopen verification must first prove the exact preset profile; it then feeds the
decoded semantic values into this serializer. The artifact digest remains useful for transfer and
corruption checks, but it is not cross-build render identity.

A qualified dependency profile may additionally assert byte-for-byte reproducibility for its exact
encoder versions, build flags, and platform matrix. That stronger profile-specific claim is test
evidence, not part of portable preset semantics and cannot be inferred merely because semantic
verification passed.

## Immutable Export Request

An approved `FrameExportRequest` captures:

- one completed approvable `OutputAnalysisAttempt`, retaining its captured document snapshot and
  revision, project/composition/output/time identity, exact frame-bound process identity and frame,
  owning report, preset/profile, adapter provenance, canonical target preflight, and qualified PNG
  display-processor handle and identity when applicable;
- the exact `OutputAnalysisDigest` approved by the artist or headless policy, which must byte-equal
  the retained attempt's digest;
- publication-intent ID, overwrite policy, resource/time limits, and any destination option that
  does not alter the retained canonical target identity; and
- a task owner/group used for progress, cancellation, and shutdown.

Task generation, cancellation state, byte budgets, destination path, and overwrite policy do not
enter either pixel cache identity. They remain part of job/publication identity. A process-cache
key excludes display/view, looks, monitor/output intent, packing, and display processor. A
display/output cache key begins with the exact process-frame identity and adds the complete prepared
processor, display intent, packing, and output-preparation revisions. The request is immutable after
admission and exposes no frame, processor, report, identity, or target-substitution mutator. Export
uses the retained frame and processor products directly; it cannot reevaluate the captured snapshot
or resolve a replacement identity. A newer document revision creates a different analysis attempt
and job rather than changing the frame under an active writer.

## Non-Blocking Execution

The pre-approval `OutputAnalysisAttempt` graph above performs target access, evaluation, semantic
identity hashing, color resolution, and digesting entirely through bounded worker stages. The UI or
headless caller receives the completed typed attempt and approves only its exact digest. Approval
itself performs no evaluation, hashing, color work, or filesystem access: it assigns a
publication-intent ID and constructs the immutable `FrameExportRequest` from the retained attempt.

One approved foreground export job has this explicit dependency graph:

1. CPU output preflight validates the retained attempt/request binding and checked aggregate
   resources without reevaluating the composition or rehashing the process frame.
2. For PNG, dependent output preparation applies the retained qualified built-in processor on CPU
   in bounded chunks or drives bounded helper slabs for the retained external-config processor,
   then produces one immutable prepared display/output frame; EXR exposes rows from the retained
   process frame directly.
3. Blocking-I/O publication asks the shared `StagedArtifactCoordinator` for a
   `StagedArtifactLease`, revalidates the retained target preflight, writes, reopens, verifies,
   hashes, then enters its short atomic-publication section.

The application controller submits a dependent stage only after consuming its predecessor's typed
successful result through the task mailbox. A worker never waits on another task, future, worker
thread, processor build, or the UI event loop. Attempt and export progress are monotonic within each
stage and use the ordered stage vocabulary `Resolving`, `Evaluating`, `Identifying`,
`ColorPreparing`, `Analyzing`, `PreparingOutput`, `Writing`, `Verifying`, and `Publishing`. EXR
skips `ColorPreparing`; no active worker is implied while an approvable attempt awaits a decision.

Attempt resource admission computes and reserves the checked retained bytes needed through the
approval decision, including the process frame, semantic-identity product, report, target state,
and PNG processor product. Approved-job admission transactionally expands that reservation to the
checked peak across retained attempt products, prepared display/output pixels, encoder scratch, and
verification chunks; it neither double-charges shared retained storage nor grants display mapping a
hidden second allowance. Variable-size encoded output also has an explicit cap. An insufficient or
overflowing reservation rejects the stage before allocation or file creation. Cancellation before
identity completion publishes no partial identity; cancellation before publication publishes no
artifact.

Version 1 export limits are closed; a request may lower but not raise them:

| Resource | Hard limit |
| --- | ---: |
| width or height | `32768` pixels |
| checked pixel count | `67108864` (`2^26`) |
| retained process-pixel bytes | 1 GiB (`1073741824`) |
| retained prepared PNG bytes | 256 MiB (`268435456`) |
| one encoder or verifier streaming chunk | 16 MiB (`16777216`) |
| one export job's aggregate Bloom-host resident allowance | 2 GiB (`2147483648`) |
| all concurrent export jobs' aggregate Bloom-host resident allowance | 4 GiB (`4294967296`) |
| staged or pre-existing artifact bytes read/written/hashed | 16 GiB (`17179869184`) |
| one export's total monotonic elapsed time | 24 hours (`86400` seconds) |
| permitted no-progress interval in a writer, reader, or filesystem stage | 120 seconds |

The host allowance includes retained inputs, prepared pixels, adapter state charged at its qualified
worst case, encoder scratch, verification decode state, hashes, canonical metadata, and queued I/O
buffers. The supervised OCIO helper has the separate hard process ceiling and per-slab deadlines in
the color contract; its sealed shared-memory slabs are also charged to the export job's host
allowance. Admission reserves from the service allowance before work starts and releases on every
terminal path. Checked arithmetic overflow, an adapter whose worst-case memory cannot be
conservatively charged, inability to stream within the chunk cap, artifact growth beyond the cap,
deadline expiry, or no-progress expiry fails with a typed resource diagnostic and publishes
nothing. A target unable to enforce these limits cannot report the preset `Qualified`.

Cancellation is checked between operations, scanlines/chunks, write calls, and verification chunks.
Cancellation before publication removes the private staging artifact and leaves the destination
unchanged. No partial image or unverified file is reported as successful.

## Atomic Publication

Project saves and frame exports reuse one `src/platform::StagedArtifactCoordinator` and its
move-only `StagedArtifactLease`; format adapters cannot implement private variants. Target preflight
runs on the blocking-I/O executor and produces an `ArtifactTargetKey` from a canonical opened parent
directory identity plus the normalized final filename under native platform comparison semantics.
Relative, case, and Save As aliases therefore converge when the platform considers them the same
target. The parent handle is pinned for the lease lifetime. Final-component inspection and all
staging opens are no-follow; a symlink, reparse point, directory, device, or other non-regular
existing target is rejected rather than traversed.

The application assigns a monotonic `PublicationIntentId` when the artist or headless caller accepts
a save or export and registers the newest generation for its canonical key. Before entering the
publication section, a save or export that is no longer the newest same-target intent returns
`Superseded`; completion order can never let an older accepted export overwrite a newer one. This is
the same per-target generation policy for both operation kinds, not an output-only lock.

The request records an expected external-file fingerprint: either `Absent`, or the existing regular
file's stable identity, byte size, high-resolution modification time, and SHA-256 of its complete
bytes. Bloom rechecks that fingerprint immediately before publication. A mismatch fails with
`ExternalModificationConflict`; overwrite approval never authorizes silently replacing an
externally changed target. Size and time are fast rejection evidence, while the complete digest is
the content check.

With the lease held, the `StagedArtifactCoordinator` performs this sequence on the blocking-I/O
executor:

1. create through the pinned parent handle a securely named, exclusive, no-follow private staging
   regular file in the target directory;
2. write incrementally under explicit size and cancellation limits;
3. close the encoder and file;
4. reopen the same staged-file identity without following links and semantically verify it;
5. compute the artifact SHA-256 and flush staged file contents;
6. revalidate the parent and target identities, enter a short non-cancellable publication section,
   and atomically replace or create the target according to the immutable overwrite policy; and
7. flush the parent directory where the platform can provide that durability guarantee.

The final result distinguishes:

- `Published` — replacement and requested durability completed;
- `PublishedWithDurabilityWarning` — the new target is visible, but a post-replacement flush could
  not prove the requested durability; and
- `Superseded` — a newer same-target Bloom publication intent won before publication;
- `ExternalModificationConflict` — the target no longer matches the expected external state; and
- `CancelledBeforePublication` or `FailedBeforePublication` — the previous target, if any, remains
  intact.

After atomic replacement Bloom never claims that the old target remains. A cancellation or error
arriving after step 6 reports the actual published state. Stale staging cleanup is bounded,
destination-scoped, and never guesses at user files.

## Capability Boundary

`src/output` owns presets, analysis, adapter diagnostics, verification, and publication orchestration.
OIIO/OpenEXR types are private. The application-wide `PublicationCoordinator` owns monotonic intent
IDs, per-target ordering, and supersession across saves and exports. `src/platform` owns the shared
`StagedArtifactCoordinator`, canonical target identity, `StagedArtifactLease`, no-follow
same-directory staging, external-conflict detection, atomic replace, and durability primitives with
parity tests on Linux, macOS, and Windows. `src/runtime` owns
immutable process evaluation and task composition. UI and scripting issue requests and consume
typed reports; they do not write image files directly.

An `OutputCapabilityReport` names each preset/version, portable pixel-semantics profile, separate
writer/reader execution provenance and availability, maximum checked dimensions/bytes, supported
metadata features, and qualification state (`Unavailable`, `Development`, or `Qualified`).
File-extension recognition alone is not a capability claim.

## Deterministic Fixtures And Gates

Shared fixtures cover:

- exact transparent empty stack, opaque/translucent solids, negative/HDR RGB, alpha endpoints,
  signed zero, odd dimensions, hostile signed windows, square and non-square PAR;
- exact process-frame, display-processor, analysis, and output-semantic identity bytes, including
  every serializer boundary and malformed/non-canonical rejection; private-only process-identity
  construction, exact retained-frame lifetime, cancellation before and during chunked hashing,
  monotonic `Identifying` progress, and no partial identity publication;
- the closed facet-code vocabulary, descriptor grammar, and rejection of approval when any
  `presetPermits` bit is zero;
- PNG exact packed bytes and tie cases, exact required chunk order/content, row filter bytes,
  rejection of every forbidden ancillary chunk, straight alpha, missing OCIO, preset
  dequalification when the pinned writer diverges, and forbidden window/PAR cases;
- EXR exact Float32 channel bits, exact header allowlist/types, signed windows, rational-PAR
  approximation and tie cases, compression, line order, and Rec.709/D65 attribute bits;
- deliberate writer coercion or metadata omission detected by reopen verification;
- insufficient budgets, disk-full/short-write, permission failures, cancellation at each stage,
  canonical-path aliases, symlink/reparse rejection, external destination mutation, ordered
  same-target intents, overwrite denial, replace failure, and post-replacement flush warning;
- attempt dismissal and supersession release every reservation, approval accepts only the exact
  retained digest, and approved export cannot substitute a frame/processor, recompute semantic
  identity, or reevaluate the document snapshot;
- exact-at-limit, one-over, arithmetic-overflow, allocation-failure, no-progress, helper, job, and
  service budget cases;
- identical report digests and reopened semantic outputs under the same portable pixel-semantics
  and OCIO identities on Linux, macOS, and Windows while execution provenance differs;
  profile-specific fixtures may additionally require identical artifact bytes; and
- task/thread sentinels proving no evaluation, color processing, encoding, verification, hashing,
  or filesystem publication runs on the UI thread.

The first artist-visible checkpoint is a Jobs entry that renders one frame, presents the full
preservation report before approval, progresses without blocking interaction, and opens only a
verified atomically published PNG or EXR.

This document is an accepted implementation contract. Output adapters, preset conversion,
staged-format verification, the publication coordinator, and qualified PNG/OpenEXR dependency
profiles remain pending Batches 5–7 implementation.

Primary references:

- [OpenImageIO 3.1.16 image output](https://openimageio.readthedocs.io/en/v3.1.16.0/imageoutput.html)
- [OpenImageIO 3.1.16 PNG behavior](https://openimageio.readthedocs.io/en/v3.1.16.0/builtinplugins.html#png)
- [OpenEXR technical introduction](https://openexr.com/en/latest/TechnicalIntroduction.html)
- [OpenEXR standard attributes](https://openexr.com/en/latest/StandardAttributes.html)
- [Color-management contract](color-management.md)
- [Task-system contract](task-system.md)
- [Dependency-intake contract](dependency-intake.md)
