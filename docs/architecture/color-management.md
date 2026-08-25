# Color Management

Status: accepted

Implementation status: standalone version 1 durable value validation and domain-separated
built-in/archive and external-loose content revision primitives are implemented. Project ownership,
the qualified Bloom Neutral asset/profile, resolution, OCIO integration, processing, and
cross-platform qualification remain pending.

Updated: 2026-08-25

## Purpose

Bloom uses OpenColorIO (OCIO) for production color configuration and transforms while keeping
project identity, image ownership, task execution, and error behavior in Bloom-owned types. Color
management is an explicit pipeline boundary; it is not ambient process state and it is not hidden
inside image storage.

The version 1 process interpretation is closed: a canonical process image is finite premultiplied
`RGBA32F` in scene-referred, linear-light Rec.709 primaries with a D65 white point. Its Color
Interop Forum identifier is exactly `lin_rec709_scene`. The identifier is the durable semantic
identity; a display, view, look, monitor, or output choice does not change it.

Every qualified OCIO configuration must resolve exactly one non-data color space whose declared
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

## Durable OCIO Configuration Identity

Project color settings own:

- `colorSettings.processColorSpaceId`, exactly `lin_rec709_scene` in project schema 1.0; and
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
Neutral v1; an explicit relink command may later publish another qualified reference without
changing the fixed process interpretation. Mutable aliases and ambient registry defaults are not
persisted.

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

Any state other than `Ready` is fail-closed for qualified processing:

- the Viewer may retain its last-good frame, label it stale, and show an actionable color diagnostic;
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
