# Color Management

Status: accepted

Implementation status: version 1 durable value validation, the domain-separated content revision
primitives, the canonical display-processor identity, the built-in registry with concrete
Bloom Neutral and ACES 1.3 CG built-in resolution, in-process CPU display processing, project and
composition working-space selection, config-managed asset input transforms, and the checked
alpha/pixel flow are implemented and qualified on Linux. The separable
blend modes under "Blend modes" below are implemented in the CPU reference compositing kernel and
qualified by per-mode goldens. The
supervised helper, the archive and loose locator kinds, staged-graph integration, and the
processor cache remain pending.

Updated: 2026-09-18

## Version 1.20 Decisions — Asset Input Transforms

Document schema 1.20 gives every image and video asset a durable `inputColorSpaceId` beside the
legacy integer interpretation. An empty id is Auto. A non-empty id is resolved only in the exact
OCIO configuration and revision selected by the project; it must name a non-data colour space.
Image and Video source nodes retain their integer `colorSpace` rows (`0` inherits the asset, and
the legacy `sRGB`/`Linear`/`Raw` values remain for compatibility) and add a non-animatable string
`inputColorSpaceId` row. An empty node id inherits the asset's id or automatic interpretation.

The deterministic 1.19 → 1.20 migration retains the legacy enum and adds the equivalent config id:
`Auto` becomes empty, `Srgb` becomes the selected config's sRGB-texture id,
`Linear` becomes the working-space id, and `Raw` remains empty and data/no-conversion. The
configuration is resolved before the migration result is accepted; if it is unavailable, the
durable id is not guessed.

`color::CpuColorSpaceProcessor` is the general CPU-only input path. It builds an OCIO
`ColorSpaceTransform` from a non-data source id to the effective working-space id, preserves alpha
and premultiplication order, and records the config content revision in the processor identity.
Missing ids, data spaces, invalid working spaces, non-invertible transforms, unavailable CPU
processors, hostile-resource failures, and an incompatible floating-point environment are typed
failures. The existing display-inverse `CpuInputProcessor` remains the colour-picker boundary.

### Automatic asset rules

Auto resolution is format-specific and never uses an ambient `$OCIO` or a decoder default:

| Source | Automatic input id / result |
| --- | --- |
| 8/16-bit PNG, JPEG, TIFF | Config sRGB-texture space: `color_picking`, `texture_paint`, or the config's named sRGB-texture id; Bloom Neutral uses `srgb_rec709_display` |
| EXR with AP0 chromaticities | `ACES2065-1` |
| EXR with AP1 chromaticities | `ACEScg` |
| EXR with Rec.709/D65 chromaticities | `lin_rec709_scene` or the config's Rec.709 camera/video mapping |
| EXR with other chromaticities | Refuse with a typed reason; choose an explicit config id |
| EXR without chromaticities | Working space, with a visible warning |
| Video: Rec.709 primaries/matrix + BT.709 transfer | Config Rec.709 camera/video space |
| Video: sRGB transfer | Config sRGB-texture space |
| Video: linear transfer | Working space |
| Video: Rec.2020, HLG, or PQ tags | Typed refusal; an explicit config id is required and the pixel path must be qualified for that mapping |

ARRI LogC3 and RED Log3G10 are not container-tagged automatic cases. The Assets and Properties
pickers list every non-data colour space, grouped by the config's family, so an artist can choose
those spaces explicitly for camera footage. The resolved id and config revision are shown in the
Assets tooltip and source row; video tooltips also retain the numeric H.273 container tags.

The read path is probe/metadata resolution → bounded decode to straight RGB `RGBA32F` → OCIO
input transform → unchanged alpha association/premultiplication into the working space. Decoded
image and video-frame keys include source/member identity, resolved input id, effective working id,
and OCIO config revision. Changing any of those values re-decodes thumbnails, proxies, and frames.
The CPU AP0 EXR read is pinned against the OCIO reference processor to an absolute channel delta of
`1e-5`; Neutral sRGB PNG defaults retain the existing byte-identical goldens.

## Current Version 1.19 Decisions — Working Colour Space

Document schema 1.19 revises the earlier fixed-process-space decision. `ColorSettings.processColorSpaceId`
is the project's working colour space: it must resolve, in the selected OCIO configuration, to a
non-data scene-linear colour space. The durable identity is the colour-space id together with the
selected configuration's content revision. A composition may carry an optional
`workingColorSpaceId`; absent means inherit the project value. An override is accepted only when the
same selected configuration exposes that exact scene-linear colour space. A failed or unavailable
resolution is typed and fail-closed; Bloom never silently falls back to an ambient OCIO config.

New projects use Bloom Neutral v1 and `lin_rec709_scene`. Project Settings → Colour can select that
default or OCIO 2.5's built-in ACES 1.3 CG configuration
`ocio://cg-config-v1.0.0_aces-v1.3_ocio-v2.1`; its revision is derived from OCIO's serialized
content. The working-space picker is populated from the selected config, with the `scene_linear`
role preselected. External `config.ocio` and `.ocioz` references remain explicit durable locators;
`$OCIO` is only a suggested picker path and is never an evaluation fallback.

Authored Solid, Text, and Shape colours are numeric values in the effective working space. The colour
picker edits sRGB-encoded values and converts through the selected config's `sRGB - Texture` colour
space. Changing the working space re-interprets existing numbers; “convert existing colours” is the
opt-in undoable path for preserving appearance.

The process/display split remains: process evaluation and blending use the effective working space,
flat EXR records its primaries in `chromaticities`, and display-referred PNG/viewer output applies the
working-space-to-display transform. Projects that remain on `lin_rec709_scene` retain the previous
process-frame and reference-display golden bytes.

### Historical Version 1 Decision

The original version 1 contract fixed every process image to `lin_rec709_scene` and required every
qualified config to expose that exact interop id. That text is retained below as design history; the
1.19 decision above is authoritative for current documents.

## Purpose

Bloom uses OpenColorIO (OCIO) for production color configuration and transforms while keeping
project identity, image ownership, task execution, and error behavior in Bloom-owned types. Color
management is an explicit pipeline boundary; it is not ambient process state and it is not hidden
inside image storage.

The historical version 1 process interpretation was closed: a canonical process image was finite premultiplied
`RGBA32F` in scene-referred, linear-light Rec.709 primaries with a D65 white point. Its Color
Interop Forum identifier is exactly `lin_rec709_scene`. The identifier is the durable semantic
identity; a display, view, look, monitor, or output choice does not change it.

Every qualified OCIO configuration then had to resolve exactly one non-data color space whose declared
Color Interop ID is `lin_rec709_scene`. Matching a display name, role, alias, or approximate
chromaticities is not a substitute. A config without that exact unambiguous mapping is `Invalid`
for Bloom v1, even if it could produce a visually plausible result.

New version 1 projects use the immutable built-in configuration **Bloom Neutral v1**, identified by
the exact URI `bloom://ocio/neutral-v1/config.ocio`. It must expose both the process Color Interop ID
`lin_rec709_scene` and the display/output Color Interop ID `srgb_rec709_display`. Its expected
payload digest comes from Bloom's qualified dependency/build profile; it is never discovered from
the artist's environment. Any content change requires a new built-in URI. Existing projects retain
their exact locator and expected digest rather than being migrated to a newer built-in implicitly.

## Process And Display Separation

The first production path is:

```text
immutable document snapshot
        |
        v
compiled plan -> CPU/GPU process evaluation -> ProcessFrame
                                             |          |
                                             |          +--> flat OpenEXR output
                                             v
                              qualified OCIO display processor
                                             |
                                             v
                                  PreparedDisplayFrame
                                             |
                                             +--> Viewer
                                             +--> display-referred PNG output
```

`ProcessFrame` owns only the process image, process cache identity, document revision, and
composition/time/output identity. Evaluation diagnostics remain on the typed result, not the
successful frame. It does not own a display buffer or display/view selection.
`PreparedDisplayFrame` owns the immutable packed display result and the complete display processor
identity; the live unqualified reference path uses the narrower `ReferenceDisplayFrame` with the
same ownership split. `PreparedPreviewFrame` combines a desired preview-request identity with a
prepared display frame for stale-result rejection.

This separation is normative. A process-cache key includes the compiled plan, exact time, output,
resolution, process quality, evaluator/provider and primitive revisions, and only color
configuration or context inputs used by process operations. It excludes display, view, looks,
monitor, display packing, and display-processor identity. A display-cache key starts with the exact
process-frame identity and adds the complete prepared display-processor and packing identity.
Process and display caches have separate bounded budgets and never share a combined key.

Changing only a display, view, look, monitor, or display packing invalidates only display results.
Changing a config revision also invalidates process results when an import or explicit process
color operation used that config; the fixed v1 Solid-to-process path does not. A
`PreparedPreviewFrame` is a publication envelope over those two products, not a third pixel cache.

### Viewer display/view and look selection

At config resolution Bloom enumerates the active `(display, view, colourSpaceId)` pairs once. The
public list is bounded to 64 displays and 32 views per display; a larger config is refused with a
typed diagnostic rather than being partially exposed to the UI. Each entry marks the config's
default pair. Bloom Neutral's `srgb_rec709_display / srgb_rec709_display` pair remains the default,
so its existing display-processor identity and packed goldens are unchanged. The ACES CG list
includes the `Rec.1886 Rec.709 - Display / ACES 1.0 - SDR Video` review pair and its sRGB display
counterpart.

The Viewer footer stores the selected pair under a revision-scoped, per-viewer QSettings key and
includes it in `PreviewRequestIdentity`, the RAM-preview key, and display-preparation identities.
Changing it therefore re-prepares display pixels without invalidating the working-space process
cache. The colour picker's inverse remains pinned to the config's sRGB authoring pair; it is not
changed by the review display selection.

The footer's Look toggle is a viewer-only request setting. When off, viewer evaluation sets
`EvaluationRequest::bypassLookNodes`; RAM preview and the one-pixel probe inherit that setting.
Export requests retain the normal `false` value and always include active look-tagged effects.
The toggle is unavailable when the composition snapshot has no look-tagged OCIO effect.

Probe values expose un-premultiplied working-space RGBA from the process frame, the encoded RGBA8
actually painted, and the float display-stage value before packing. The status-bar cell identifies
the working-space id and selected display in its tooltip; Ctrl-click cycles the display value
between 8-bit and float forms.

## Durable OCIO Configuration Identity

Project color settings own:

- `colorSettings.processColorSpaceId`, the effective project working-space id in document schema
  1.19 (historical 1.18 projects retain `lin_rec709_scene`); and
- `colorSettings.ocioConfig`, a Bloom-owned `OcioConfigReference`.

`OcioConfigReference` contains:

- a locator kind and locator value;
- `expectedRevision`, a versioned SHA-256 content revision;
- a portability classification;
- a sorted, explicit set of OCIO context variables whose values affect project interpretation; and
- a schema version for Bloom's interpretation of the record.

The accepted locator kinds are:

| Kind | Durable value | Qualification rule |
| --- | --- | --- |
| Concrete Bloom built-in | Versioned immutable `bloom://ocio/.../config.ocio` URI | Resolve only through the qualified Bloom registry; aliases such as “default” or “latest” are invalid durable identity |
| Project-relative config archive | normalized relative path to an external `.ocioz` | Hash archive bytes and validate all referenced resources within the archive |
| External config archive | absolute file URI to `.ocioz` | Hash bytes; retain as an explicit external dependency |
| External loose config | absolute file URI to `config.ocio` | Hash the config and every resolved external resource in a deterministic manifest |

Environment variables, a process-global current config, `OCIO` lookup, current working directory,
search-path discovery, `ocio://default`, and a mutable “latest” alias are never durable project
identity. They may help a relink UI discover candidates, but evaluation begins only from the
published qualified reference.

Bloom v1 stores references and digests only. A `.bloom` container never embeds an OCIO config,
`.ocioz`, LUT, or other color resource. Project-relative `.ocioz`, external `.ocioz`, external loose
configs, and concrete built-ins remain explicit dependencies. New v1 projects always use Bloom
Neutral v1; an explicit relink command may later publish another qualified reference. Mutable aliases
and ambient registry defaults are not persisted. ACES 1.3 CG's exact OCIO built-in URI is a supported
immutable registry entry; its digest is computed from the serialized OCIO configuration using the
same content-revision algorithm. A composition override is not a second config reference: it reuses
the project reference and changes only the effective working-space id.

### OCIO Content Revision Version 1

`expectedRevision.algorithm` is exactly `sha256`. Its lowercase hexadecimal digest is computed from
one closed binary serialization. Integers below are unsigned big-endian; concatenation is exact;
strings are canonical UTF-8 bytes without a terminator.

For a built-in payload or `.ocioz`, revision version 1 is:

```text
SHA-256(
  "BloomOcioRevision\0" || u16(1) ||
  u8(locatorKind) || u64(payloadByteCount) || exactPayloadBytes
)
```

`locatorKind` is `1` for an immutable built-in, `2` for project-relative `.ocioz`, and `3` for
external `.ocioz`. A version 1 built-in registry entry is one exact `config.ocio` payload with no
external resource; its bytes come from the qualified dependency profile. An `.ocioz` payload is the
complete archive byte sequence, not decompressed members, timestamps, or an OCIO cache ID. The
domain prefix, version, and locator tag prevent the same bytes in different revision domains from
sharing an identity accidentally.

An external loose config uses a content graph rooted at the directory containing `config.ocio`.
Version 1 permits only regular files below that root; absolute resource references, parent
traversal, symlinks, hard links, alternate data streams, and ambient search roots are invalid. A
canonical key is the root-relative path after strict UTF-8 decoding, Unicode 15.1 NFC, and `/`
normalization. It has no backslash, drive prefix, empty, `.`, or `..` component and never begins
with `/`. Qualification rejects duplicate keys, NFC collisions, and Unicode 15.1 default-case-fold
collisions even on a case-sensitive host. The qualification dependency profile pins the Unicode
implementation and test corpus.
`config.ocio` is always present as that exact key. Entries are sorted by unsigned UTF-8 byte order,
then revision version 1 is:

```text
SHA-256(
  "BloomOcioLooseRevision\0" || u16(1) || u32(entryCount) ||
  for each entry:
    u32(keyByteCount) || keyUtf8 ||
    u64(payloadByteCount) || exactPayloadBytes
)
```

Files are hashed from the same no-follow opened handles that were validated. A changed identity,
size, or payload during the pass rejects qualification. The sorted entry table plus individual
file digests and resolver diagnostics form a derived qualification manifest for tests, support,
and audit; it is not persisted in `.bloom` and never replaces the versioned aggregate revision.

OCIO's processor and config cache IDs participate only in execution provenance and local cache
partitioning; they do not replace Bloom's content revision or enter portable processor identity.
OCIO may incorporate filesystem metadata for external references; Bloom requires its own
byte/content manifest so a saved expected revision is portable and auditable.

Display, view, looks, exposure controls, and monitor selection are session or render-request state,
not project authoring truth. A render preset may persist an explicit output transform intent, but it
still resolves against the project's qualified config revision.

## Blend modes

A layer's blend mode says how its pixels combine with what is already beneath it in the Layer Stack.
It is a Layer Output parameter, `bloom.layer.blend-mode`, stored as a small integer under one closed
and durable mapping. `Normal` is `0`, so a document that carries no blend mode at all -- every Layer
Output written before this contract -- decodes as `Normal` and renders exactly the picture it always
did.

| Stored | Mode | Separable blend function `B(Cb, Cs)` |
| ---: | --- | --- |
| `0` | Normal | `Cs` |
| `1` | Add | `Cb + Cs` |
| `2` | Multiply | `Cb * Cs` |
| `3` | Screen | `Cb + Cs - Cb * Cs` |
| `4` | Overlay | `2 * Cb * Cs` if `Cb <= 0.5`, else `1 - 2 * (1 - Cb) * (1 - Cs)` |
| `5` | Darken | `min(Cb, Cs)` |
| `6` | Lighten | `max(Cb, Cs)` |
| `7` | Difference | `abs(Cb - Cs)` |

Appending a mode is additive. Renumbering one, or reusing a retired number, is not: it would
re-interpret every saved document, and would require a new schema key rather than a new number.

`Cb` and `Cs` are UN-premultiplied backdrop and source channels. The process representation is
premultiplied, so the kernel divides each pixel by its own alpha, applies `B` per channel, and
re-premultiplies through one fold -- the W3C Compositing and Blending Level 1 general formula with
source-over as the compositing operator:

```text
co = as * (1 - ab) * Cs  +  as * ab * B(Cb, Cs)  +  (1 - as) * ab * Cb
ao = as + ab * (1 - as)
```

`co` is the premultiplied result channel; `as` and `ab` are source and backdrop alpha.

**Alpha compositing is source-over for every mode.** Only the colour combination changes, so a blend
mode never makes a layer cover more or less of what is beneath it than its own alpha says. The alpha
expression above is evaluated with exactly the arithmetic the source-over kernel uses.

Two modes are evaluated without the round trip, because substituting their own `B` into the fold
reduces it exactly:

- **Normal.** `B(Cb, Cs) = Cs` collapses the fold to `co = cs + (1 - as) * cb`, which is
  source-over on premultiplied values. The implementation therefore calls the retained source-over
  kernel for `Normal` rather than re-deriving it, which is what makes a `Normal` layer bit-identical
  to every frame published before blend modes existed.
- **Add.** `B(Cb, Cs) = Cb + Cs` cancels both alpha weightings and leaves `co = cs + cb`, premultiplied
  addition. Dividing by alpha anyway would add two roundings to an exact answer.

The remaining six take the division. Both alphas are strictly positive on that path: an alpha-zero
source contributes nothing and is skipped, and an alpha-zero backdrop makes the fold collapse to the
source pixel itself under every mode, so it is written through exactly.

**Nothing is clamped**, at either end. The process contract forbids clamping before the declared
display boundary, and that applies inside a blend: a negative or HDR channel goes into `B` and comes
out of the fold unclipped. The unit references in `Screen` and `Overlay` are therefore not ceilings
but the reference white of `lin_rec709_scene`: above it, those two formulas extrapolate rather than
saturate, which is the honest reading of a scene-referred space that has no maximum. A non-finite
result, or a finite-input overflow, fails the row with a typed error rather than being clamped into
range.

The vocabulary is deliberately the separable modes only. Each one combines the corresponding channels
of two pixels and nothing else, so each has one closed formula over scene-linear values. A
luminosity, saturation, hue, or colour mode would have to commit to a luminance model and a gamut the
process space does not fix, and belongs with a qualified colorimetry decision rather than with this
set.

Blending happens in the process space, before any display transform. A blend mode is authoring truth
and affects process results; it is not a display, view, or look choice, so it invalidates process
cache results rather than only display ones. Qualification uses per-mode golden rows on a
premultiplied fixture that includes partial alpha on both sides, an HDR channel above 1, and a
negative channel, plus a bit-exactness pin of `Normal` against the retained source-over kernel.

## Hostile Configuration And Resource Limits

All configuration input is untrusted. Resolution is local-only, bounded, cancellable task work. V1
accepts only the concrete built-in registry and local `file` references; `http`, `https`, custom URI
handlers, sockets, data URLs, and network-backed OCIO callbacks are forbidden. Resolution never
downloads a config, LUT, certificate, schema, or missing resource.

The default v1 ceilings are closed:

| Resource | Limit |
| --- | ---: |
| loose `config.ocio` bytes | 8 MiB (`8388608`) |
| physical `.ocioz` bytes | 64 MiB (`67108864`) |
| archive or loose-root regular-file entries | `2048` |
| one expanded archive or loose resource | 64 MiB (`67108864`) |
| all expanded archive or loose resources | 256 MiB (`268435456`) |
| one Bloom-controlled allocation during resolution/build | 64 MiB (`67108864`) |
| one resolution/build task's aggregate resident allowance | 384 MiB (`402653184`) |
| all concurrent resolution/build tasks' aggregate resident allowance | 768 MiB (`805306368`) |
| immutable prepared-processor cache resident bytes | 256 MiB (`268435456`) |
| expanded/compressed ratio per archive entry | `1000:1`, using `max(1, compressedSize)` |
| one normalized resource name | 4096 UTF-8 bytes |
| explicit context variables | `256` |
| one context-variable name | 128 ASCII bytes |
| one context-variable value | 4096 UTF-8 bytes |
| one path after context expansion | 16384 UTF-8 bytes |

Every host byte buffer and container growth uses the Bloom-controlled allocator budget before
allocation; the 384 MiB host-task allowance includes compressed input, expanded resources,
canonical-key storage, validation/hashing scratch, IPC slabs, and the retained immutable handle.
Admission reserves from the 768 MiB host-service allowance, and completion or failure releases it.
The helper's separate hard process ceiling appears below. A dependency build that cannot be bounded
or conservatively preflighted under these ceilings cannot be qualified for production config
intake. Cache insertion is separately byte-accounted and evicts before exceeding its limit.

An `.ocioz` reader accepts only regular, non-executable files with unique normalized relative names.
It rejects absolute paths, empty or dot components, `..`, backslashes, NUL, symlinks, hard links,
devices, encrypted or multi-disk archives, overlapping entries, unsupported compression, nested
archive interpretation, and entries whose declared and streamed sizes, CRC, or limits disagree. It
streams directly into bounded storage and never extracts into a directory.

A loose config has exactly one canonical config-directory root in version 1. Every config and
resource is opened locally through the platform no-follow file service, must be a regular file,
must stay below that root, and is hashed from the opened handle. Ambient and absolute search paths
and environment substitution are disabled. Qualification rejects missing, duplicate, changed,
path-escaping, normalization-colliding, or case-colliding resources and a resource whose identity
changes while it is read.

Context names are unique ASCII identifiers matching `[A-Za-z_][A-Za-z0-9_]*` and sorted by byte
order. Expansion uses only the recorded values, is single-pass, rejects an unknown or recursive
reference, and applies the expanded-path limit before lookup. Context values cannot introduce a URI
scheme, absolute path, or root escape. Archive/resource streaming and manifest hashing check
cancellation at bounded chunk boundaries. The next section defines the stronger isolation required
for OCIO parsing, processor construction, and transform execution, whose third-party calls are not
assumed interruptible.

### Supervised OCIO Execution

OCIO input has two trust classes:

- Bloom Neutral v1 may execute in-process only when its immutable payload, exact dependency build,
  parser/build latency, transform latency, allocator behavior, and hostile-fixture suite are all
  qualified on Linux, macOS, and Windows. A failed qualification moves it to the helper path; it
  never weakens the limits.
- Every project-relative archive, external archive, and external loose config executes through a
  supervised, killable `bloom-color-worker` helper. All OCIO parsing, config construction,
  processor construction, and transform application for those inputs occurs in that helper.

Bloom-owned code may stream, structurally validate, and hash local files in the host. For a loose
root it enumerates the bounded candidate regular-file table without interpreting OCIO syntax. It
then sends the helper a sealed resource table containing canonical relative keys and exact bytes.
The helper has no network access, does not resolve ambient paths or environment variables, cannot
spawn children, and receives no destination path. It returns only versioned Bloom protocol records,
the exact consumed resource-key set, typed diagnostics, opaque tokens, and packed pixel products.
The host accepts only consumed keys present in the supplied table and computes the loose revision
from those same no-follow file handles. OCIO pointers, objects, exceptions, enums, cache objects,
and allocator-owned memory never cross the process boundary.

One helper serves at most one independently cancellable request group at a time. Processor reuse is
represented in the host by an opaque, generation-scoped `PreparedProcessorToken`; the OCIO object
remains in that helper. Pixel application uses sealed shared-memory slabs no larger than 16 MiB
(`16777216` bytes), with checked descriptor, byte count, generation, and request nonce on every
message. Input slabs are read-only to the helper; output slabs are distinct and remain unpublished
until the host validates the complete response. Stale generations and malformed, duplicate, or
out-of-order messages are rejected.

The supervisor uses a monotonic clock and these hard version 1 deadlines: 5 seconds for process
start and protocol handshake, 30 seconds for one config parse plus processor build, and 10 seconds
for one transform slab. Cooperative cancellation gets 250 milliseconds to acknowledge; application
shutdown gets 2 seconds for all outstanding work. Expiry, protocol failure, cancellation, or
shutdown terminates the helper process, invalidates all of its tokens, closes its IPC endpoints,
reclaims shared memory, and publishes no partial processor or frame. A helper has a 512 MiB
(`536870912` byte) hard process-memory ceiling in addition to the Bloom-side task and service
allowances above.

`src/platform` supplies parity-qualified process creation, inherited-handle restriction, memory
limit, monotonic deadline, termination, and cleanup adapters. The concrete OS primitive may differ,
but observable result states and diagnostics do not. If a target cannot prove the memory ceiling,
kill deadline, handle isolation, and orphan-free shutdown, external OCIO capability is
`Unavailable` on that target rather than falling back in-process. Failures use typed codes including
`HelperUnavailable`, `HelperProtocolViolation`, `HelperMemoryLimit`, `HelperDeadline`,
`HelperCancelled`, and `HelperTerminated`; raw exit codes remain secondary provenance.

## Configuration Resolution States

Resolution returns one structured state and diagnostics:

| State | Meaning |
| --- | --- |
| `Ready` | Locator resolves and every expected content digest and required role/space is valid |
| `Missing` | Config locator cannot be resolved |
| `Changed` | Content resolves but differs from the project's expected revision |
| `Invalid` | OCIO cannot parse or validate the config |
| `MissingResource` | Config exists but a referenced LUT or resource is absent |
| `UnsupportedVersion` | Config requires unsupported OCIO/schema behavior |

Opening a project never rewrites its color reference to fit the current machine. The project remains
editable and preserves the unresolved record. Relink or requalification is an explicit command that
publishes a new durable reference and participates in undo, save, and diagnostics.

The current color state is always visible. It is a chip in the window status bar
(`windowStatusBarColorChip`), not a label inside any one editor panel: an artist must be able to
read whether what they are looking at is qualified without having a particular panel open, and the
window status bar is the one surface that is always present. The chip's qualified/unqualified
reading is driven by the CURRENTLY DISPLAYED frame's own qualification bit, never by the activity
of a request in flight, so a retained qualified frame keeps reading as qualified while a later,
unrelated request is running -- the state is never silently relabelled. The same wording backs the
Viewer's accessible description, from one definition, so the two cannot drift apart.

Any state other than `Ready` is fail-closed for qualified processing:

- the Viewer may retain its last-good frame, label it stale, and show an actionable color diagnostic
  in the window status bar;
- Bloom never substitutes another production display transform automatically;
- a temporary reference mapper may be selected manually for troubleshooting, but its result is
  visibly `Unqualified` and cannot satisfy a production display or export request;
- PNG output that requires a qualified display transform fails before file publication;
- flat process OpenEXR output may proceed when its process encoding is already fully identified and
  independent of the missing display transform; and
- headless output reports the same state and diagnostics as the desktop application.

## Qualified Display Intent And Identity

A display request names:

- the qualified config revision and exact ordered context name/value set;
- source Color Interop ID;
- OCIO display and view names;
- ordered looks or explicit look bypass;
- output Color Interop ID where the config declares one;
- display-processing quality and Bloom display-semantics version; and
- packing intent such as straight RGBA8 sRGB.

Version 1 permits no dynamic OCIO property or ambient exposure override; adding one requires a new
serializer version. Names are meaningful only under the qualified config revision. A matching
display/view string from a different config is a different identity.

`DisplayProcessorIdentity` has one closed canonical serialization. `u8`, `u16`, and `u32` are
unsigned big-endian. `text` is `u32(byteCount)` followed by strict Unicode 15.1 NFC UTF-8 with no
NUL; `bytes32` is exactly 32 bytes with no length prefix. The identity bytes are exactly:

```text
ASCII "BloomDisplayProcessorIdentity\0"
u16(1)
bytes32(expected OCIO SHA-256 revision)
u16(contextVariableCount)
for each context variable in ascending ASCII-name byte order:
  text(name)
  text(value)
text(source Color Interop ID)
text(OCIO display name)
text(OCIO view name)
u8(lookMode)                         # 0 = explicit bypass, 1 = ordered list
u16(lookCount)                       # must be zero when lookMode is 0
lookCount * text(look name)
text(output Color Interop ID)
text(display quality ID)             # exactly "reference" in version 1
text(display pixel-semantics profile ID)
text(packing ID)                     # exactly "straight-rgba8" in version 1
```

Version 1 accepts only source Color Interop ID `lin_rec709_scene`, output Color Interop ID
`srgb_rec709_display`, quality ID `reference`, display pixel-semantics profile
`bloom.color.ocio-cpu-display.v1`, and packing ID `straight-rgba8`. The display and view names are
nonempty and each is at most 4096 UTF-8 bytes. Look mode `0` has count zero; look mode `1` has
between 1 and 128 ordered, nonempty names, each at most 4096 UTF-8 bytes. The existing context
limits remain 256 variables, 128 ASCII bytes per name, and 4096 UTF-8 bytes per value. The complete
canonical identity, including its domain prefix, counts, and length fields, is at most 2 MiB
(`2097152` bytes). Every count and byte-length calculation is checked before allocation or
caller-buffer mutation. A record that exceeds any component or total limit is invalid rather than
truncated.

The version 1 display pixel-semantics profile ID is exactly
`bloom.color.ocio-cpu-display.v1`. Its qualification freezes Float32 input/output, alpha handling,
chunk independence, finite-value failure, and exact output bits for the profile's fixtures on every
supported platform. The canonical bytes themselves are the identity record; when a fixed-size key
is needed, it is SHA-256 of those bytes with no additional prefix.

Target-specific facts are a separate `DisplayProcessorExecutionProvenance`: exact OCIO and compiler
versions, target triple/ABI, dependency-lock digest, qualified-prefix digest, build options, helper
protocol build, and OCIO's opaque processor cache ID. Provenance is retained in capability reports,
diagnostics, and local cache partitioning, but it is not part of the portable
`DisplayProcessorIdentity`. A build may claim the shared semantics profile only after cross-platform
goldens prove the same canonical identity and output bits. Otherwise it receives a distinct profile
ID or remains unqualified. Request generation, cancellation, budgets, owner, helper PID, shared
memory handles, cache residency, and destination path enter neither record.

## CPU Display Processor Boundary

`src/color` owns a Qt-free `bloom_color_ocio` adapter and the typed host side of the helper protocol.
Its public API uses Bloom value types and immutable image views; OCIO classes, exceptions, pointers,
and enums remain private. The boundary has three products:

1. `ResolvedColorConfig`, an immutable qualified config and resource manifest;
2. `PreparedCpuDisplayProcessorHandle`, an immutable `DisplayProcessorIdentity`, execution
   provenance, and either a qualified in-process built-in lease or an opaque helper token; and
3. `PreparedDisplayFrame`, an immutable packed display buffer and its display identity.

Preparation results own ordered diagnostics and status; successful immutable processor and frame
products do not absorb task-attempt diagnostics into cache identity or lifetime.

Config/resource resolution and processor preparation form a blocking-I/O stage. Its only successful
host result is an immutable `PreparedCpuDisplayProcessorHandle`; it never contains a raw OCIO
object. A dependent display-application stage receives that product through a typed mailbox and
applies it in bounded in-process chunks for a qualified built-in or bounded shared-memory slabs for
a helper. The application controller submits the dependent stage after observing successful
completion. No task calls `wait`, `get`, joins a worker, nests a task submission and waits for it,
or blocks the UI event loop. Frame output uses this same staged graph rather than constructing a
processor inside an encoder or pixel loop.

The application owns a bounded LRU of processor handles, partitioned by semantic identity and
execution provenance. A cache miss builds outside the cache lock, then inserts only if the exact
identity is still desired. Entries are byte/cost accounted, duplicate concurrent builds coalesce
without blocking the UI, and eviction never invalidates a handle retained by an in-flight task.
Killing a helper invalidates all handles in its generation before another request can observe them.
There is no process-global mutable OCIO config or unbounded processor history.

## UI Colour Boundary

Authoring colour parameters remain straight `Color4d` values whose numeric interpretation is the
effective project/composition working space. Solid, Text, Shape, Color value and Color operand
schemas retain the historical `bloom.reference.linear-srgb` authoring label for schema
compatibility; it no longer means that the stored numbers are permanently Rec.709 values. Opening
or migrating a document never rewrites those numbers. Changing the working space re-interprets
them, while the explicit Convert Existing Colours command preserves appearance as an undoable
operation.

The qualified `PreparedCpuDisplayProcessorHandle` exposes `referenceToDisplay(Color4d)` and
`displayToReference(Color4d)`. Both use the resolved project OCIO display/view; no UI implements a
transfer function. They operate on straight RGB, preserve binary64 alpha exactly,
and reject invalid/non-finite input, float overflow and incompatible floating-point environments.
The forward result clamps RGB to [0, 1]. The inverse retains extended range and uses OCIO's
lossless optimization mode so display white does not acquire artificial HDR values. The existing
forward frame processor, config digest, display identity and identity goldens are unchanged.
The pair is tested within an absolute `3e-5` float tolerance on the unclipped domain. Clipping is
not invertible: an HDR reference colour must remain available separately from its display swatch.

`KColor` carries a `Display` or `Reference` tag. QColor, hex and HSV/HSL representations are
Display-only; attempting to paint or format a Reference value directly returns no display value.
The kit receives a conversion callable from `CompositionSession::colorConverter(schemaKey)`;
it has no dependency on project types or OCIO. The accessor accepts the four reference colour
schemas above and refuses unknown encodings. Each session prepares its immutable built-in processor
off the UI thread, publishes readiness through a timer, and cancels publication when destroyed.
Worker-owned state contains no widget or session pointer; session destruction never waits for the
worker. Pending or failed conversion disables reference swatches and leaves explicitly labelled
reference numbers available. A failed preparation reports a session diagnostic. This currently
supports the same fixed Bloom Neutral v1 display/view as the viewer.

Properties (Solid, Text and registry colour rows) and node-card colour cells bind this accessor.
Their chips and picker controls paint Display values and emit Reference values to the existing
session commands. Hex text, recent colours, sampled screen colours, HSV/HSL and the picker's
available spatial forms describe display sRGB. Merely opening or closing a picker preserves the
original reference value, including HDR RGB and alpha, and issues no command.

Expanded Properties RGBA fields normally show normalized display values. If any stored RGB
channel is negative or above 1, all four fields show the original reference numbers with a
`reference` suffix. The swatch still shows the converted, clamped display colour. Typing an
out-of-range RGB value into a display field authors that channel as a reference value and switches
the row to reference presentation. Unchanged channels retain their exact reference numbers, so
editing alpha or one component cannot accumulate conversion error in the others. Composition
background preferences already use Display values and retain that interpretation. The separate
legacy timeline colour-row adapter still requires adoption of this boundary.

Regression pins cover display `#F03B2E` becoming approximately reference
`(0.871367, 0.043735, 0.027321)`, the Properties and node-chip pixels matching that pick, the
qualified viewer pipeline producing RGBA8 `(240, 59, 46, 255)`, stable repeated hex edits, HDR edits,
and unchanged stored colour numbers after a production 1.9-to-1.10 document migration. UI pins
run at DPR 1, 1.25, 1.5 and 2.

## Alpha And Pixel Flow

OCIO processes color channels, never alpha. Bloom's display adapter performs this checked flow:

1. read finite premultiplied process `RGBA32F`;
2. if alpha is exact binary32 zero, produce exact positive-zero RGB; otherwise divide each
   premultiplied binary32 RGB component by alpha with one binary32 round-to-nearest,
   ties-to-even result and reject non-finite division output;
3. apply the OCIO CPU processor to binary32 RGB only in bounded chunks;
4. retain alpha unchanged;
5. require binary32 processor output and reject a non-finite component;
6. apply only the output preset's declared clamp, transfer, quantization, and packing; and
7. publish no partial display frame on failure or cancellation.

Negative and HDR process values survive until the declared display/output boundary. Alpha-zero RGB
is canonicalized according to the existing process primitive contract. The CPU path is the
correctness oracle for the separately gated OCIO GPU processor.

Every binary64 or exact-rational to binary32 boundary in this contract uses IEEE-754
round-to-nearest, ties-to-even, with gradual underflow and preserved signed zero. NaN, infinity, or
finite overflow fails before publication. Implementations must use a qualified conversion routine
and reject an incompatible ambient floating-point environment; they may not inherit a caller's
rounding mode or flush subnormals silently.

## Deterministic Fixtures And Gates

Qualification uses repository-owned minimal OCIO configs and LUTs with stable content digests,
never an artist workstation's environment. Fixtures cover:

- matrix-only, one-dimensional LUT, and three-dimensional LUT processors;
- negative, HDR, zero-alpha, translucent, opaque, and non-finite failure cases;
- context-variable identity and changed/missing external resources;
- display/view switches that reuse the process frame and invalidate only display results;
- a missing config retaining a stale last-good Viewer frame without publishing it as current;
- processor-cache hit, miss, coalescing, bounded eviction, cancellation, and shutdown;
- canonical `DisplayProcessorIdentity` bytes and the same output bits on Linux, macOS, and Windows
  under the shared semantics profile;
- malformed/stale IPC, shared-memory descriptor mismatch, process-memory exhaustion, parse, build,
  slab, cancellation, and shutdown deadlines, forced termination, token invalidation, and
  orphan/shared-memory cleanup; and
- in-process Bloom Neutral qualification failing closed to the helper route when any latency,
  allocator, or hostile-input gate is not satisfied.

No color path is qualified by subjective visual inspection alone. Golden bytes, processor identity,
structured diagnostics, task behavior, and resource budgets are acceptance evidence.

## Version 1 Decisions

- Process interpretation is exactly `lin_rec709_scene` regardless of the config's internal reference
  space.
- New projects use immutable Bloom Neutral v1 at
  `bloom://ocio/neutral-v1/config.ocio`; its digest is supplied by the qualified build profile, and
  a payload change requires a new URI.
- Projects persist only qualified references and digests; color payload embedding is outside v1.
- A config that cannot resolve the exact process and requested output identities fails closed; no
  ambient or approximate fallback is production-qualified.
- This document is an accepted implementation contract. The OCIO adapter, built-in registry,
  supervised helper, qualification profiles, cache, and hostile-input gates remain pending Batch 7
  implementation.

Primary references:

- [OpenColorIO configuration API](https://opencolorio.readthedocs.io/en/v2.5.2/api/config.html)
- [OpenColorIO developing guide](https://opencolorio.readthedocs.io/en/v2.5.2/guides/developing/developing.html)
- [OpenColorIO ACES CG configuration](https://opencolorio.readthedocs.io/en/v2.5.2/configurations/aces_cg.html)
- [Color Interop Forum color-space identifiers](https://github.com/AcademySoftwareFoundation/ColorInterop)
- [Evaluation primitive color and alpha contract](evaluation-primitives.md)
- [Frame output contract](frame-output.md)


## PNG And JPEG Input In v0

Auto and sRGB interpret encoded samples as display-referred sRGB. `CpuInputProcessor` prepares the
OCIO inverse of Bloom Neutral v1's `srgb_rec709_display` display transform into
`lin_rec709_scene`. This is an input preparation, not a new colorspace or config revision. PNG16
samples retain their precision through normalization to float. Straight RGB is converted first,
then multiplied by alpha for the immutable `Rgba32fImage`. Alpha itself is never color-converted.

Linear and Raw bypass the inverse transform; they use the normalized samples as linear reference
values and retain the same premultiplied image contract. The Image node's Premultiply option
means the source samples are straight. Turning it off declares associated input: nonzero alpha
is removed before conversion and restored afterward; zero-alpha RGB becomes zero. Auto inherits
the asset color interpretation; explicit per-node sRGB/Linear/Raw overrides it.

The image process key includes source/member content digest, interpretation and the unchanged
Bloom Neutral config revision. Display/view changes remain display-cache concerns. Evaluator
semantics advances once, 5 → 6, to identify image input and sequence evaluation. Primitive 5,
plan 3, animation 2 and Bloom Neutral v1 identities stay fixed. Output identity goldens were
independently derived using the S5 byte-envelope oracle with those four version values.

## Explicit Colour Space Transform

`bloom.ocio-colour-space-transform` uses the shared image-effect operation. Its constant String
parameters `from` and `to` select non-data colour-space ids from the exact project OCIO config;
an empty id means the effective working space. `bypass` defaults to false. Explicit transforms
may target nonlinear spaces such as ACEScct; the project working space remains scene-linear.

The evaluator shares prepared CPU processors by config revision, source id, and destination id.
It transforms straight RGB and preserves alpha under the image-effect alpha contract. Missing ids,
data spaces, or unavailable processors pass the image through with a node-addressed typed warning,
including on warm cache hits. The existing preview diagnostic path exposes that reason in the Viewer
and status line. Bypass and true identity share the input pixels exactly.


## Show Look Nodes And LUT Assets

The Colour menu contains `bloom.ocio-colour-space-transform` and `bloom.ocio-file-transform`.
Both lower to the reusable image-effect operation. CST resolves `from` and `to` in the exact
selected config, with an empty id meaning the effective working space. File Transform resolves
`processSpace` the same way and executes working → process space → LUT → process space → working.
For the ACES show workflow this is ACEScg → ACEScct → show LUT → ACEScct → ACEScg. Colour kernels
un-premultiply RGB, preserve alpha, and re-premultiply after processing; zero alpha remains zero.

LUT assets accept `.cube`, `.clf`, `.spi1d` and `.spi3d`. Import captures the existing locator and
SHA-256 digest off the UI thread. The file is reopened without following the final symlink,
checked as a regular file, bounded, hashed and structurally preflighted before preparation.
The observed digest must match the imported asset. A changed digest changes the effect memo key
and reports `ChangedFile`; relinking adopts new bytes through an undoable command. Missing ids,
missing files and every refusal pass through with a typed evaluation warning, including on cache
hits. A canonical `.cube` whose table is the exact identity grid with the default unit domain is an
exact identity, including HDR values outside that domain; no clamp or process-space round trip is
introduced. OCIO-proven identity processors use the same path. No imported image or LUT is rewritten.

External LUT parsing, processor construction and application use the supervised colour helper.
The Linux implementation sends anonymous sealed resources and distinct pixel slabs over a private
socket with descriptor, inode, length and nonce checks. The helper verifies structural limits again
before OCIO and checks OCIO's parsed 3D grid sizes before preparing a CPU processor, including
when its detected format differs from the file extension. Landlock denies filesystem access;
seccomp denies network creation, child creation,
execution and cross-process memory/signal access. No output path enters the protocol. CLF external
references and XML entities are refused. The file cap is 64 MiB and a 3D LUT edge is at most 129.
The existing 5/30/10-second deadlines, 512 MiB helper ceiling and typed supervisor failures apply.
Host reservations conservatively account for input bytes, sealed copies, scratch, slabs and opaque
handles under the 768 MiB service ceiling; a request stays below 384 MiB and allocations below
64 MiB. The runtime cache retains at most four file processors, accounting one MiB per opaque
handle. Cancellation kills and reaps an active helper, invalidates its token and publishes no
partial image. A later evaluation prepares a fresh token.

Linux requires the confinement primitives as well as the existing process supervisor. macOS,
Windows and Linux hosts without those capabilities report `HelperUnavailable` for external LUTs.
They preserve the editable graph and pass through with diagnostics. There is no in-process external
LUT fallback. The sealed-file intake also returns `HelperUnavailable` on macOS and Windows, so new
LUT import is refused there while saved LUT records remain editable. Built-in CST processing
remains available on all three desktop targets.

File Transform's `look` marker opts it into request-level `bypassLookNodes`. The default is false;
true skips only marked effects and has a separate frame/memo identity. COLOR-4 owns the Viewer
footer switch. Export captures its own policy: the COLOR-5 handoff preset sets
`bypassLookNodes=true`, while review sets false. Neither changes durable node parameters or the
Viewer setting. Handoff approval states **Look: OFF (handoff)**; review evidence states
`look: baked (N look-tagged effects)`.

The VFX deliverable converts the effective working space to `ACES2065-1` (AP0) through the exact
config, writes premultiplied 32-bit float EXR with PIZ, and labels its sequence from 1001. The
review deliverable applies look-tagged effects, then the config's `Rec.1886 Rec.709 - Display` /
`ACES 1.0 - SDR Video` display transform before H.264 quantization. Its default target is 30 Mbps.
The ACES 1.3 CG built-in's default display is sRGB; the named review deliberately selects the
Rec.709 pair. Raw display-referred presets use the config's default display/view. Missing pairs
fail explicitly. The processor pair and config revision are recorded in analysis and evidence;
there is no export display/view picker.
