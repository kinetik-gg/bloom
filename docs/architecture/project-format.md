# Bloom Project Format

Status: accepted

Implementation status: bounded reservations and PMR allocation, canonical integer/rational,
Base64, UTF-8 string, and JSON-layout primitives, canonical manifest encoding and schema checks,
the normative document `1.0` schema artifact and checks, manifest requirement validation, durable
allocator high-water state, opaque extension envelopes, and the Linux staged-artifact close/reopen
verification foundation are implemented. Complete JSON parsing/document encoding, ZIP, migration,
unknown-member overlay, format-specific semantic verification, and cross-platform publication
parity remain pending.

Updated: 2026-08-25

## Purpose And Ownership

`.bloom` is Bloom's durable authoring container. It preserves canonical project truth needed to
reopen and evaluate a project; it is not a general VFX interchange format. Adopted standards remain
explicit adapters at their own boundaries.

`src/project` owns schemas, migrations, constrained-container validation, deterministic encoding,
unknown-member round-trip state, and save staging. `src/platform` owns narrow filesystem identity,
flush, and atomic-publication primitives. `src/document` owns format-independent durable values and
validation. No JSON or ZIP implementation type enters document, command, runtime, UI, or public
extension contracts.

The application-level Open/Save lifecycle, supersession, dirty state, and unsaved-change flow are
defined in [`project-session.md`](project-session.md).

[ADR 0015](../decisions/0015-zip-json-project-container.md) fixes the container choice; this document
is its normative v1 implementation contract.

## Version 1 Constants

The initial container version and document schema are both `1.0`. Version objects always contain
JSON-number members in `major`, `minor` order. Each is an unsigned 32-bit integer.

The schemas use JSON Schema Draft 2020-12. They live at
`schemas/project/manifest-1.0.schema.json` and `schemas/project/document-1.0.schema.json`, with
absolute `$id` values `urn:kinetik:bloom:schema:project-manifest:1.0` and
`urn:kinetik:bloom:schema:project-document:1.0`. They declare the 2020-12 `$schema` and use only
repository-local `$ref` targets during validation. A generic schema validator is useful for
fixtures, but it does not replace Bloom's duplicate-key, resource, canonical-decimal,
cross-reference, allocator, graph, and preservation validation.

`document-1.0.schema.json` contains required `$defs` named `colorSettings-1.0`,
`ocioConfigReference-1.0`, `ocioConfigLocator-1.0`, and `ocioContextVariable-1.0`. The project
definition requires `colorSettings`; the OCIO reference definition requires every member specified
below and selects one closed locator shape with `oneOf`. These definitions validate structure and
lexical bounds. Project I/O additionally validates locator normalization, digest spelling, sorted
context variables, locator/portability agreement, and the fixed v1 process Color Interop ID.
`manifest-1.0.schema.json` contains a required `requirement-1.0` definition with the exact provider,
capability, schema-version, and node-type-coverage members specified below.

Known versioned object definitions permit additional members through
`unevaluatedProperties: true` so a compatible newer minor can be retained. Discriminator values and
array element categories remain closed. The compatibility decoder, not the generic schema alone,
determines whether an unknown member is safely additive.

`manifest.json` and `document.json` are strict UTF-8 [RFC 8259] JSON. The reader rejects a BOM,
invalid UTF-8 or Unicode scalar sequences, lone escaped surrogates, duplicate decoded object keys,
comments, trailing commas, non-finite values, and JSON5 extensions before constructing trusted
document state.

## Constrained ZIP Profile

Container `1.0` is aligned with the core profile in [ISO/IEC 21320-1] and contains exactly two
regular-file entries in this order:

```text
manifest.json
document.json
```

Both names are exact ASCII and carry the UTF-8 flag. Container `1.0` rejects every other entry,
including a manifest-declared payload. A later minor version may add payload entries only after
Project I/O can preserve their bounded bytes and metadata while their owner is unavailable.

The accepted reader profile is deliberately smaller than general ZIP:

- single-disk archive with no encryption, archive comment, entry comment, digital signature,
  prepended data, trailing data, or nested archive interpretation
- stored method `0` or deflate method `8`; no ZIP64 because all v1 limits are below classic ZIP
  limits
- no data descriptors or unapproved extra fields; sizes and CRC are present in local and central
  headers
- local and central names, flags, method, sizes, CRC, and entry counts agree
- entry byte ranges do not overlap each other or the central directory
- entries are regular non-executable files; directories, symlinks, hard-link metadata, devices,
  FIFOs, sockets, and platform-special file types are rejected
- no absolute name, backslash, NUL, empty component, `.`, or `..`; the exact two-name allowlist is
  still checked after path-profile validation
- each entry is streamed into a bounded buffer and its complete CRC is verified before JSON parsing
- normal Open never extracts an archive to a directory

The writer emits no comments, descriptors, extra fields, encryption, ZIP64, or platform-dependent
paths. It writes manifest then document, uses the DOS epoch `1980-01-01 00:00:00`, declares regular
mode `0644` without executable bits, and uses deflate level 6. If deflate would not reduce an entry
or would exceed the permitted expansion ratio, the writer stores that entry instead. Exact whole
archive bytes are not a compatibility promise because a qualified compression implementation may
change; decoded semantics and the uncompressed JSON bytes are deterministic.

### Resource Limits

The v1 writer refuses to create a project outside these limits, so every conforming v1 file written
by Bloom is accepted by the default reader. Limits apply before allocation where the size is known
and during streaming otherwise.

| Resource | v1 limit |
| --- | ---: |
| physical archive size | 260 MiB (`272629760` bytes) |
| entry count | exactly `2` |
| expanded `manifest.json` | 1 MiB (`1048576` bytes) |
| expanded `document.json` | 256 MiB (`268435456` bytes) |
| total expanded bytes | 257 MiB (`269484032` bytes) |
| expanded/compressed ratio per entry | `1000:1`, using `max(1, compressedSize)` |
| JSON nesting depth, root included | `128` |
| JSON values across both entries | `4000000` |
| elements in one array or members in one object | `1000000` |
| one decoded ordinary JSON string, excluding extension `payload` | 64 MiB (`67108864` UTF-8 bytes) |
| one extension `payload` base64 spelling | 89478488 ASCII bytes |
| one decoded opaque extension payload | 64 MiB (`67108864` bytes) |
| all extension `payload` base64 spellings | 192 MiB (`201326592` ASCII bytes) |
| all decoded opaque extension payloads | 128 MiB (`134217728` bytes) |
| default aggregate resident Project I/O memory per operation | 1 GiB (`1073741824` bytes) |
| application-wide resident Project I/O memory across concurrent operations | 2 GiB (`2147483648` bytes) |
| one Project I/O allocation | 256 MiB (`268435456` bytes) |

Namespaced owner, type, provider, capability, and remapper IDs are ASCII lowercase identifiers
matching `[a-z0-9][a-z0-9._-]{0,127}`. Structural schema keys, port names, roles, media types, and
reference-table keys are at most 256 UTF-8 bytes and non-empty. Human-facing project, composition,
and layer names are at most 4096 UTF-8 bytes and non-empty. More restrictive owning schemas may set
smaller bounds.

Open options may impose lower deployment or test budgets. Raising any limit requires an explicit
headless/application policy bounded by the implementation's checked absolute ceiling; it never
happens implicitly because a file requests more memory.

The resident budget is not an archive-size alias. It charges all simultaneously live memory owned
or conservatively reserved by one Open, Save, migration, or Save Copy operation: compressed input,
expanded entries, parser DOM and indexes, duplicate-key tables, decoded `Document`,
`RoundTripState`, opaque payloads, preserved archive bytes, canonical output, and reopen-validation
state. The operation allocator performs checked addition before every reservation and rejects an
individual allocation above 256 MiB. The application-owned `ProjectIoMemoryCoordinator` also
charges the same reservation against one 2 GiB pool shared by every concurrent Project I/O task;
headless entry points construct the same bounded coordinator. Third-party work must use the
operation allocator or reserve a worst-case charge before calling a dependency; unmetered
dependency allocation is not permitted. Moving storage between stages transfers its charge, while
keeping two representations live charges both.

`OpenOptions` and `SaveOptions` default the per-operation resident budget to 1 GiB and may lower it.
No operation budget may exceed the shared 2 GiB implementation ceiling. Per-operation and shared
charges are acquired atomically or not at all, and are released when their owning allocation or
reservation dies. A size conversion, multiplication, accumulation, or allocator charge that
overflows is resource exhaustion, never a wrapped smaller request. Allocation failure and budget
rejection produce typed `ProjectIoCode::ResourceExhausted`, release every Project I/O allocation,
close handles, and invoke RAII cleanup for any unpublished stage. Open returns without constructing
or installing partial trusted state; Save returns `FailedBeforePublication` and never enters the
publication lease. Cleanup failure remains a secondary diagnostic and does not hide the
resource-exhaustion cause.

The base64 spelling bound is exactly `4 * ceil(67108864 / 3)` and is measured on the ASCII value
after JSON string unescaping. It is a narrow exception to the ordinary decoded-string bound, not
permission for another giant string. The aggregate spelling, decoded-payload, expanded-entry, and
total-expanded limits all apply independently; passing one never bypasses another. Project I/O
checks base64 length and padding before allocating decoded storage, accumulates aggregate limits
with checked arithmetic, and streams decoding into the already bounded destination.

## Manifest Shape

The canonical manifest has members in this exact order:

```json
{
  "format": "org.kinetik.bloom.project",
  "containerVersion": {
    "major": 1,
    "minor": 0
  },
  "document": {
    "path": "document.json",
    "schemaVersion": {
      "major": 1,
      "minor": 0
    }
  },
  "requirements": []
}
```

`format` and `document.path` are exact. The schema version repeated at the root of `document.json`
must equal the manifest declaration; disagreement is corruption, not a migration request.

A requirement record has members in this order:

```json
{
  "providerId": "vendor.module",
  "capabilityId": "vendor.module.project-data",
  "schemaVersion": {
    "major": 1,
    "minor": 0
  },
  "providedNodeTypeIds": [
    "vendor.module.node-type"
  ]
}
```

Requirements are sorted by `providerId`, then `capabilityId`, then schema major/minor. Duplicate
provider/capability pairs are invalid. Within each requirement, `providedNodeTypeIds` is sorted by
UTF-8 bytes and contains no duplicate. It may be empty when the capability owns only opaque
extension data. A requirement names data capability needed to interpret some project truth; it does
not persist an installed package version, download source, Python import path, or native ABI.
Opening never installs, enables, imports, migrates, or executes a provider.

The document schema `1.0` foundation node-type set is exactly `bloom.composition-output`,
`bloom.layer-output`, `bloom.layer-stack`, `bloom.solid-source`, and `bloom.text-source`. These types
are exempt from requirement coverage and must not appear in `providedNodeTypeIds`. For every other
distinct `NodeRecord.typeId` in the complete project, exactly one requirement lists that exact ID.
Conversely, every listed node type must occur in at least one `NodeRecord`; therefore the union of
all lists is exactly the project's distinct non-foundation node-type set. Missing coverage,
overlapping coverage, an unused listed type, or a listed foundation type is invalid. This explicit
manifest map determines unavailable node ownership without changing the live `NodeRecord` shape or
guessing from a string prefix.

An opaque extension owner is covered by at least one requirement with the same `providerId`.
Multiple capabilities from that provider may exist, but the exact capability needed by each known
extension type is declared by its registered format definition. Unavailable records retain the
opened requirement set through `RoundTripState`; Bloom never infers their capability from payload
bytes.

Project I/O owns the manifest requirement set alongside `RoundTripState`. Known registered document
types declare the requirement they introduce; requirements from an unavailable provider are
retained from the opened manifest. Before writing, Project I/O recomputes node-type coverage and
rejects a retained requirement that would make the exact-coverage invariant false. A writer removes
a known requirement only after no surviving node, extension record, or other durable construct
needs it. New projects using only foundation types and no extension-owned truth write an empty
array.

## Canonical JSON Primitives

The reader accepts insignificant RFC 8259 whitespace and any object-member order. The writer emits
every non-empty object and array over multiple lines, with one member or array element block per
level, two-space indentation, a single space after `:`, and commas only after non-final members or
elements. Empty containers are `{}` and `[]`. Output uses LF line endings, one final LF, and no
trailing whitespace. Small fragments below may compact whitespace to show type shape; golden files,
not compact fragments, demonstrate canonical layout.

### Decimal Strings And JSON Integers

- Every typed 64-bit ID is an unsigned decimal string. Valid object IDs use `[1-9][0-9]*` and must
  fit `uint64`; zero is invalid.
- Inclusive allocator high-water values use `0|[1-9][0-9]*` and must fit `uint64`.
- Every signed 64-bit semantic integer, including an `int64` parameter value and rational
  component, is a decimal string matching `0|-?[1-9][0-9]*` and must fit `int64`. `+0`, `-0`, a
  leading plus, and leading zero are invalid.
- Schema integers stored as JSON numbers are non-negative base-10 integers without a fraction or
  exponent and must fit their declared `uint32` or smaller range. JSON negative zero is invalid for
  an integer field.
- A rational object has `numerator`, then `denominator`. Its denominator is positive, the pair is
  already reduced, and zero is represented only as `{ "numerator": "0", "denominator": "1" }`.

Non-canonical integer and rational strings are rejected rather than silently normalized. A
supported older schema may normalize them only through an explicit migration fixture.

### Float64

Float fields are JSON numbers that round correctly to a finite IEEE 754 binary64 value. Overflow to
infinity is invalid. The writer starts from the [RFC 8785]/ECMAScript binary64 serialization, then
applies two type-preserving rules:

- positive zero is `0.0` and negative zero is `-0.0`
- when the representation contains neither a decimal point nor an exponent, append `.0`

All other digits, exponent thresholds, lowercase `e`, and exponent sign follow that serialization.
This preserves signed zero and the exact binary64 value while remaining independent of locale. The
reader may accept another finite RFC 8259 decimal spelling and normalizes it on save.

### Unknown JSON Numbers

An unknown additive member can remain editable only when Project I/O can preserve its JSON number
without guessing the absent owner's numeric type. The v1 lossless subset is deliberately narrow:

- a canonical integer token matching `0|-?[1-9][0-9]*` whose mathematical value fits `int64`; or
- the exact canonical Float64 spelling defined above, including the required `.0` for an integral
  finite value and preservation of signed zero.

`RoundTripState` stores the first as an exact `int64` and the second as its exact binary64 bits.
Canonical writing therefore reproduces the same mathematical value and the same canonical token
without retaining a general raw-number spelling. An unknown number outside this subset—including
an out-of-range integer, a non-canonical exponent or decimal spelling, or a decimal that would
round or normalize on rewrite—cannot be overlaid safely. Open returns `PreservedReadOnly` for that
archive rather than silently changing the unknown value. Known schema fields continue to follow
their declared integer or Float64 decoding rules.

### Strings And Object Keys

The writer preserves the decoded Unicode scalar sequence without NFC, NFD, case, or path
normalization. It emits ordinary non-ASCII scalars directly as UTF-8, does not escape `/`, uses the
short escapes `\b`, `\t`, `\n`, `\f`, and `\r`, and uses lowercase `\u00xx` for other control
characters below U+0020. `"` and `\` are escaped.

Duplicate-key detection compares decoded Unicode strings, so `"a"` and `"\u0061"` collide. Member
ordering compares valid UTF-8 bytes without locale or case folding.

## Canonical Document Shape

`document.json` has these members in exact order:

```json
{
  "schemaVersion": {
    "major": 1,
    "minor": 0
  },
  "project": {},
  "idAllocation": {
    "highestIssued": {}
  },
  "extensions": []
}
```

Known members use the order below. Retained unknown additive members follow all known members of
their object in ascending UTF-8 key order.

Document revision, command history, selection, current time, interaction overrides, workspace and
panel layout, caches, thumbnails, proxies, compiled plans, task state, and runtime capability state
are not project truth and are absent from `document.json`.

### Project And Composition

| Object | Required members in writer order |
| --- | --- |
| project | `id`, `name`, `colorSettings`, `compositions` |
| composition | `id`, `name`, `duration`, `format`, `parameters`, `animationCurves`, `graph` |
| composition format | `width`, `height`, `pixelAspect`, `frameRate` |
| rational/pixel aspect/frame rate | `numerator`, `denominator` |

Compositions are sorted by numeric `CompositionId`. `duration` is a positive normalized rational.
`width` and `height` are JSON-number `uint32` values satisfying the document extent limits.
`pixelAspect` and `frameRate` use canonical decimal-string rational objects and must also satisfy
their owning unsigned domain and normalization rules.

Schema `1.0` freezes the live value domains exactly:

| Value | Accepted domain |
| --- | --- |
| `width`, `height` | integer `1..1048576` inclusive |
| composition pixel count | checked `uint64(width) * uint64(height) <= 4294967296` (`2^32`) |
| pixel-aspect numerator/denominator | reduced positive `uint32`, each `1..4294967295` |
| frame-rate numerator/denominator | reduced positive `uint32`, each `1..4294967295` |
| duration numerator | signed-64 decimal component `1..9223372036854775807` |
| duration denominator | signed-64 decimal component `1..9223372036854775807` |

Duration is reduced before serialization and must already be reduced on v1 decode. A zero or
negative duration, a zero unsigned-rational term, an unreduced pair, a dimension above the maximum,
or an extent whose checked product exceeds `2^32` is invalid. Project I/O performs product and
conversion checks before constructing `CompositionFormat` or `RationalTime`; it never narrows a
JSON value first.

### Project Color Settings And OCIO Reference

Version 1 requires one project-level `colorSettings` object. Its known members are written in this
exact order:

```json
{
  "schemaVersion": { "major": 1, "minor": 0 },
  "processColorSpaceId": "lin_rec709_scene",
  "ocioConfig": {
    "schemaVersion": { "major": 1, "minor": 0 },
    "locator": {
      "kind": "builtin",
      "uri": "bloom://ocio/neutral-v1/config.ocio"
    },
    "expectedRevision": {
      "algorithm": "sha256",
      "digest": "0000000000000000000000000000000000000000000000000000000000000000"
    },
    "portability": "builtin",
    "contextVariables": []
  }
}
```

The built-in URI above is exact; the all-zero digest illustrates the 64-hex-character shape only and
must be replaced by the qualified build profile's real revision. `processColorSpaceId` is owned by
`colorSettings`, is the Color Interop Forum identifier, and is exactly `lin_rec709_scene` in document
schema `1.0`. `expectedRevision` is owned by `colorSettings.ocioConfig`; it identifies the referenced
configuration bytes and never replaces or duplicates the process-space ID. The process encoding is
therefore known even while the referenced OCIO configuration is missing. Changing a display, view,
look, or monitor does not alter either durable value; those selections are session or render-request
state.

`OcioConfigReference` has `schemaVersion`, `locator`, `expectedRevision`, `portability`, then
`contextVariables`. The locator is exactly one of these known-member shapes:

```json
{ "kind": "builtin", "uri": "bloom://ocio/neutral-v1/config.ocio" }
{ "kind": "project-relative-ocioz", "path": "color/config.ocioz" }
{ "kind": "external-ocioz", "uri": "file:///show/config.ocioz" }
{ "kind": "external-config", "uri": "file:///show/config.ocio" }
```

- The only schema `1.0` built-in locator is the exact ASCII URI
  `bloom://ocio/neutral-v1/config.ocio`. It has no user information, query, or fragment and resolves
  only through Bloom's qualified immutable registry. Mutable aliases, including any “default” or
  “latest” entry, are invalid durable values. The qualified adapter must return that same concrete
  URI before the reference is published.
- A `project-relative-ocioz` path is non-empty normalized UTF-8 with `/` separators, has an
  `.ocioz` suffix, and contains no absolute root, drive prefix, backslash, NUL, empty component,
  `.`, or `..`. It is at most 4096 UTF-8 bytes and resolves beside the `.bloom` file, never inside
  the project ZIP.
- An external URI is an absolute [RFC 8089] `file` URI with no user information, query, fragment,
  or NUL. `external-ocioz` names an `.ocioz` file; `external-config` names `config.ocio`. The URI is
  at most 16384 ASCII bytes, is preserved as authored, and resolves only through the cross-platform
  file-URI adapter.

`portability` must be `builtin`, `project-relative`, or `external` and must agree respectively with
the locator families above. The required revision record is written as `algorithm`, then `digest`;
`algorithm` is exactly `sha256` and `digest` is exactly 64 lowercase hexadecimal ASCII characters.
`OcioConfigReference` schema `1.0` fixes the revision serialization defined under **OCIO Content
Revision Version 1** in [`color-management.md`](color-management.md). For the built-in and `.ocioz`
families, the digest is SHA-256 over the domain prefix `BloomOcioRevision\0`, unsigned big-endian
version `u16(1)`, locator-kind tag, payload byte count, and exact built-in `config.ocio` or complete
archive bytes. For an external loose config, it is SHA-256 over the distinct domain prefix
`BloomOcioLooseRevision\0`, `u16(1)`, and the sorted canonical resource table: entry count followed
by each normalized root-relative UTF-8 key and exact payload bytes with the specified big-endian
length prefixes. That table contains `config.ocio` and every permitted resolved resource. Unicode
15.1 NFC, path rejection, collision handling, no-follow reads, and mutation checks are part of that
normative color contract; a hash over only `config.ocio`, raw filesystem enumeration order, or an
informal concatenation is invalid. Filesystem metadata and OCIO's runtime cache ID never replace
Bloom's versioned, domain-separated content revision.

A context-variable record has `name`, then `value`. Names match
`[A-Za-z_][A-Za-z0-9_]{0,127}`, values contain no NUL and are at most 4096 UTF-8 bytes, and the array
contains at most 256 records sorted by UTF-8 name with no duplicate. It is a complete explicit set:
ambient environment variables, the `OCIO` environment variable, current working directory, and
process-global configuration never supplement it during evaluation.

Syntactically valid but missing, changed, or unavailable known references remain known project
truth and are preserved unchanged; they diagnose and block only color-dependent processing. An
unknown color-settings, OCIO-reference, or locator discriminator prevents trusted color semantics
and therefore opens as `PreservedReadOnly`. Safely additive unknown members on known shapes follow
the ordinary `RoundTripState` rules. Container `1.0` never embeds an `.ocioz`: reference-only color
identity preserves the exact two-entry ZIP profile.

### Parameters

A parameter record has `id`, `schemaKey`, then `source`. Parameters are sorted by numeric
`ParameterId`.

Sources have these exact shapes:

```json
{ "kind": "constant", "value": {} }
{ "kind": "animation-curve", "curveId": "1" }
```

Those are the only parameter-source discriminators in document schema `1.0`. Drivers are deferred
until a schema defines their declaration table, ownership, references, and source encoding. A future
driver source encountered through a newer compatible container is unknown core semantics: Bloom
preserves the bounded archive as `PreservedReadOnly` rather than constructing a lossy placeholder
or rewriting the source.

The live document model may contain `DriverBindingSource` while driver authoring is being developed,
but native v1 Save is intentionally a restricted supported-subset encoder. Its admission pass walks
every parameter before creating a stage. If any live source is `DriverBindingSource`, Save returns
`FailedBeforePublication` with typed `ProjectIoCode::UnsupportedDocumentFeature` and the exact
composition/parameter path. It does not discard the driver, sample it into a constant, serialize an
undeclared discriminator, or fall back to a degraded rewrite. This is an unsupported save feature,
not invalid live document truth. Constant and animation-curve sources remain the complete writable
v1 subset.

Constant values have one of these shapes and member orders:

```json
{ "kind": "bool", "value": true }
{ "kind": "int64", "value": "-1" }
{ "kind": "float64", "value": 1.0 }
{ "kind": "vec2", "x": 0.0, "y": 0.0 }
{ "kind": "color4", "red": 0.0, "green": 0.0, "blue": 0.0, "alpha": 1.0 }
{ "kind": "string", "value": "text" }
{ "kind": "rational", "numerator": "0", "denominator": "1" }
```

The parameter schema owns the semantic kind, color encoding, alpha meaning, range, and animation
capability. The decoder rejects a value/source kind that the named schema does not accept; it never
converts between variants by convenience.

### Animation

Animation curve records are sorted by numeric `AnimationCurveId` and have one of these shapes:

```json
{
  "id": "1",
  "kind": "scalar",
  "keyframes": []
}
```

```json
{
  "id": "2",
  "kind": "vec2",
  "keyframes": []
}
```

A scalar key has `id`, `time`, `value`, then `outgoingInterpolation`; a `vec2` key replaces `value`
with an object containing `x`, then `y`. Interpolation is exactly `hold` or `linear`.

Keys are written in strictly increasing exact rational time. Equal normalized times are invalid, so
`KeyframeId` never breaks a tie. Curves are non-empty; the final key's interpolation is canonical
`linear`. Curve kind, ownership, values, ranges, source references, and single-owner rules follow
[`animation-and-time.md`](animation-and-time.md).

### Canonical Graph

The graph object has `nodes`, `edges`, `layerOutputs`, `layerStack`, then `compositionOutput`.

| Object | Required members in writer order |
| --- | --- |
| node | `id`, `typeId`, `schemaVersion`, `parameters` |
| parameter binding | `role`, `parameterId` |
| edge | `id`, `source`, `destination` |
| output-port reference | `nodeId`, `port` |
| node-input destination | `kind`, `nodeId`, `port` |
| layer-stack destination | `kind`, `stackNodeId`, `slotId`, `role` |
| layer output boundary | `nodeId`, `layerId`, `name`, `outputPort` |
| layer stack | `nodeId`, `entries` |
| layer stack entry | `slotId`, `layerId` |

Destination `kind` is exactly `node-input` or `layer-stack-input`. Nodes sort by numeric `NodeId`,
edges by numeric `EdgeId`, and layer-output boundaries by numeric `LayerId` then `NodeId`. Parameter
bindings sort by UTF-8 `role` then numeric `ParameterId`. Layer Stack entries remain in semantic
top-to-bottom authoring order and are never ID-sorted. `compositionOutput` is a required output-port
reference.

## Inclusive Allocator State

`idAllocation.highestIssued` has these required members in exact order:

```json
{
  "composition": "0",
  "node": "0",
  "edge": "0",
  "layer": "0",
  "layerSlot": "0",
  "parameter": "0",
  "animationCurve": "0",
  "keyframe": "0",
  "driverBinding": "0",
  "extensionRecord": "0"
}
```

Durable IDs are project-global within each typed namespace. Equal numeric values in different typed
namespaces are valid; duplicate declarations in the same namespace across compositions are not.
Validation rejects every cross-composition collision before initial `Document` construction or
publication.

Each value is inclusive:

- `0` means the namespace has never issued an ID and its next allocation is `1`
- a value below `uint64` maximum makes the next allocation `highestIssued + 1`
- `18446744073709551615` means that maximum was issued and the namespace is permanently exhausted

The exact allocator namespaces in schema `1.0` are `composition`, `node`, `edge`, `layer`,
`layerSlot`, `parameter`, `animationCurve`, `keyframe`, `driverBinding`, and `extensionRecord`; no
other member is accepted as a known v1 namespace. Every high-water value is an unsigned decimal
string in `0..18446744073709551615`. Every declared object ID is an unsigned decimal string in
`1..18446744073709551615`. The document's sole `ProjectId` has the same nonzero typed-ID domain but
is not allocator-backed.

`driverBinding` remains required even though `DriverBindingSource` is outside the v1 writable
parameter-source subset and no driver declaration table is serialized. The live allocator may have
issued driver IDs before every driver source was removed; Save preserves that inclusive high-water
so reopen can never reuse one. A nonzero `driverBinding` high-water with no live driver source is
valid. A live driver source still fails Save with `UnsupportedDocumentFeature` as specified above.

Every serialized typed ID in a namespace must be nonzero and at most its high-water value. The
high-water may exceed all surviving declarations because deleted and undone allocations remain
consumed. Commit, restore, undo, redo, clone, import, and reopen may increase but never lower it.

`ProjectId` is not allocator-backed because one container owns one project. A normal schema `1.0`
document missing any high-water member, including `driverBinding`, is invalid. Reconstruction from
surviving IDs is permitted only inside an explicit migration from identified pre-1.0 development
schemas and is never the normal v1 decode path.

## Opaque Extension Envelope

An extension record is format-independent document truth with a project-global
`ExtensionRecordId`. Records are sorted by numeric record ID and serialize with these members:

```json
{
  "id": "1",
  "ownerId": "vendor.module",
  "typeId": "vendor.module.record-type",
  "schemaVersion": { "major": 1, "minor": 0 },
  "subject": null,
  "mediaType": "application/octet-stream",
  "referencePolicy": { "kind": "none" },
  "payload": "AA=="
}
```

`subject` is always present and is either `null` or `{ "kind": ..., "id": ... }`. Supported kinds
are `project`, `composition`, `node`, `edge`, `layer`, `layer-slot`, `parameter`,
`animation-curve`, and `keyframe`. Its typed target must exist in current document truth; an orphan
subject is invalid. A driver-binding subject is not part of v1 because v1 declares no driver table
or driver-binding subject semantics. The allocator watermark reserves the `DriverBindingId`
namespace without making driver bindings writable project records.

`payload` is canonical [RFC 4648] base64 using the standard alphabet, required `=` padding, and no
whitespace or line breaks. Decoded bytes are preserved exactly. `mediaType` describes those bytes;
the document model never depends on JSON or a provider-specific C++ type.

Reference policy has one of three exact forms:

```json
{ "kind": "none" }
```

```json
{
  "kind": "host-table",
  "references": [
    {
      "key": "primary-layer",
      "target": { "kind": "layer", "id": "1" }
    }
  ]
}
```

```json
{
  "kind": "owner-remapper",
  "remapperId": "vendor.module.record-remapper",
  "version": { "major": 1, "minor": 0 }
}
```

- `none` declares that the opaque payload contains no Bloom durable IDs.
- `host-table` requires payload schemas to refer through stable table keys rather than embedding
  raw Bloom numeric IDs. Keys are unique and sorted by UTF-8 bytes. Targets use the same typed kinds
  as subjects and must exist. Bloom remaps only table targets.
- `owner-remapper` declares that the bytes may embed Bloom IDs and require the named versioned
  provider remapper. Clone or import is rejected when that exact compatible remapper is unavailable.

Bloom never scans or rewrites opaque payload bytes heuristically. Duplicate records, invalid owner
or type IDs, orphan targets, invalid base64, incompatible reference-policy members, and payload
limit violations are invalid document state. An unavailable owner does not invalidate an otherwise
well-formed envelope; Bloom preserves it and diagnoses only affected editing/evaluation paths.

## Ordering Summary

Canonical ordering uses semantic values, never serialized decimal-string lexical order:

| Collection | Order |
| --- | --- |
| requirements | provider ID, capability ID, schema version |
| one requirement's `providedNodeTypeIds` | UTF-8 node-type ID |
| OCIO context variables | UTF-8 name |
| compositions | numeric `CompositionId` |
| parameters | numeric `ParameterId` |
| animation curves | numeric `AnimationCurveId` |
| curve keys | exact rational time |
| nodes | numeric `NodeId` |
| edges | numeric `EdgeId` |
| layer output boundaries | numeric `LayerId`, then `NodeId` |
| parameter bindings | UTF-8 role, then numeric `ParameterId` |
| Layer Stack entries | semantic authoring order |
| extension records | numeric `ExtensionRecordId` |
| host reference entries | UTF-8 key |
| retained unknown object members | UTF-8 key after known members |

## Versions, Migrations, And Preservation

Container and document versions use independent `{major, minor}` values:

- unknown major versions are rejected without mutation
- supported older versions migrate through sequential deterministic DOM migrations before trusted
  document decoding
- a newer minor opens editable only when every unknown construct is additive, bounded, and
  provably preservable
- unknown core discriminators are never guessed
- unavailable optional providers preserve their neutral extension records and unknown node type IDs
  and block only dependent behavior

Declared provider data and accidental future JSON are different mechanisms:

1. Opaque extension records are document truth and participate in snapshots, validation, commands,
   undo, clone/import policy, and allocator state.
2. Unknown additive JSON members remain owned by Project I/O in move-only `RoundTripState`; they do
   not enter `Document` or acquire authoring semantics.

Round-trip reconciliation follows exact rules:

- known writer fields always replace retained values with the same key
- unknown members on singleton objects attach by schema path
- unknown members on collection objects attach by the collection's declared stable identity, never
  array position
- deleting a known object drops its attached unknown members; a newly allocated object inherits
  none
- an unknown array element is editable only when the owning schema declares a stable identity
  extractor and proves that preserving it cannot change known semantics
- structural clone/import is rejected when retained unknown data cannot be remapped safely
- whitespace, original escape spelling, known-number spelling, and original member order are not
  retained; an unknown number is editable only through the lossless typed subset above

Array reconciliation identities are fixed: requirement provider/capability pair; composition,
parameter, curve, keyframe, node, edge, and extension record typed ID; binding role within its node;
layer ID for a Layer Output; slot ID for a Layer Stack entry; and key for a host reference-table
entry. An array without a declared identity cannot retain unknown elements through an edit.

If all unknown data is safely additive, Project I/O overlays the current known document onto the
retained DOM and writes canonical JSON. If an unknown core discriminator prevents trusted document
construction, Open returns a preserved-read-only result containing the bounded original archive and
diagnostics rather than putting raw JSON into document truth. Native Save and Save As are disabled;
only byte-preserving Save Copy is available.

No v1.0 file may contain unknown ZIP members. Support for a later minor with additional members
requires RoundTripState to retain those entries within explicit byte and metadata limits first.

Open editability is explicit:

- `Editable` has trusted core semantics and all capabilities needed by current authoring paths.
- `DegradedEditable` has a trusted `Document` and a complete preservation proof, but unavailable
  providers or safely additive unknown members restrict only affected commands/evaluation.
- `PreservedReadOnly` cannot construct trusted complete core truth; it retains the original archive
  and permits inspection or byte-preserving Save Copy only.

## Project I/O Boundary

The public surface remains concrete rather than becoming a generic serializer. The conceptual
values are:

```cpp
struct DecodedProject {
    std::unique_ptr<document::Document> document;
    RoundTripState roundTrip;
    FormatVersion sourceVersion;
    Editability editability; // Editable or DegradedEditable
    std::vector<Diagnostic> diagnostics;
};

struct PreservedReadOnlyProject {
    PreservedArchive archive;
    FormatVersion sourceVersion;
    std::vector<Diagnostic> diagnostics;
};

using OpenedProject = std::variant<DecodedProject, PreservedReadOnlyProject>;

OpenResult openProject(const std::filesystem::path& path, const OpenOptions& options);

SaveResult saveProject(const std::filesystem::path& path,
                       const document::Snapshot& snapshot,
                       const RoundTripReadView& roundTrip,
                       const SaveOptions& options);
```

The codec API is synchronous internally and runs only through the task system or a headless worker
context. Open/Save options carry resource limits, cooperative cancellation, progress, expected
target state, and diagnostics without depending on Qt. The UI thread never parses, serializes,
compresses, resolves paths, opens files, flushes, waits, or drains I/O work.

## Staging And Atomic Publication

Saving uses the application `PublicationCoordinator` for intent order and a move-only
`src/platform::StagedArtifactLease` from the shared
`src/platform::StagedArtifactCoordinator` for secure staging and publication:

1. resolve the existing parent directory and canonical target identity off the UI thread
2. reject a symlink target leaf, a non-regular existing target, or a missing/non-directory parent
3. securely create a unique same-directory regular stage with exclusive create semantics
4. encode the archive, close it, reopen it, run the complete bounded Open/decode validation, and
   compare decoded known truth and retained-data proof with the captured save input
5. flush the staged file
6. re-check the expected external-file fingerprint and winning application-wide
   `PublicationIntentId` for the canonical target
7. enter the non-cancellable publication lease and atomically replace or create the target
8. flush the parent directory using the strongest supported parity-qualified primitive
9. clean staging data on every pre-publication exit; cleanup failure is a secondary diagnostic

The lease records the securely created stage's filesystem identity. Every reopen uses no-follow
semantics and verifies that identity, file type, and same-directory ownership before using or
publishing it.

Canonical target identity is the resolved parent-directory filesystem identity plus the leaf name
under that platform's native comparison semantics. An existing target's file identity and
fingerprint are additional conflict evidence. Resolution follows parent symlinks once but never
follows a target-leaf symlink. Different spelling, case aliases, and Save As paths that resolve to
the same target share one application intent-order record and one platform publication lease.

The expected external-file fingerprint is either `Absent` or the existing regular file's identity,
byte size, high-resolution modification time, and SHA-256 of its complete bytes. Open computes it
while already streaming the archive; Save rechecks it on the blocking-I/O worker immediately before
publication. Size/time are fast rejection evidence and the hash detects content replacement. This
does not remove the final cross-process time-of-check/time-of-use window.

Platform publication behavior is:

- Linux and other POSIX hosts flush the stage, atomically rename it, then flush the parent directory
- macOS uses the same model with the strongest qualified full-file durability primitive
- Windows flushes the stage and uses `ReplaceFileW` for an existing target or a same-volume atomic
  move for first publication, then requests directory durability where supported

The result is exactly one of:

| Outcome | Contract |
| --- | --- |
| `Published` | target now names the validated new archive and durability steps succeeded |
| `PublishedWithDurabilityWarning` | replacement occurred, but a subsequent durability step failed |
| `Superseded` | a newer same-target publication intent won before this lease began |
| `CancelledBeforePublication` | cancellation won before this lease began |
| `ExternalModificationConflict` | target no longer matches the caller's expected state |
| `FailedBeforePublication` | target was not replaced |

Cancellation is observed through every stage boundary and bounded encode/decode loop before the
publication lease. After lease entry, publication is not cancellable and a late cancellation never
relabels a visible replacement as cancelled. If replacement succeeds but parent-directory flush
fails, Bloom cannot claim the previous file survived; it reports
`PublishedWithDurabilityWarning`.

Same-process project saves, Save Copy operations, and frame exports are serialized by canonical
target through the application-wide `PublicationCoordinator` defined in
[`project-session.md`](project-session.md); the platform `StagedArtifactCoordinator` owns their
shared staging and lease mechanics. External modification detection is required before replacement
but cannot eliminate the final cross-process race without a future portable locking protocol;
Bloom does not claim otherwise.

## Asset Locators

Asset paths enter the format only alongside a stable `AssetRegistry`:

- nodes reference stable asset IDs, never filesystem paths
- project-relative paths use UTF-8 and `/` separators
- an absolute [RFC 8089] file URI may be retained only as a relinking hint
- relative paths resolve against the `.bloom` file's parent directory
- saving does not case-fold asset paths or resolve their symlinks
- embedded entry paths obey the container path profile when a later version introduces them

The first codec does not invent provisional asset fields.

## Qualified Implementations

Initial qualification targets are:

- [libzip 1.11.4] under BSD-3-Clause for ZIP reading and writing
- [zlib 1.3.2] under the zlib license for the permitted deflate and CRC path
- [yyjson 0.12.0] under MIT for strict JSON, exact integers, correctly rounded doubles, mutable DOMs,
  and bounded custom allocation

They are private implementation dependencies pinned by release archive and cryptographic hash.
Headers are external/SYSTEM, optional compression, crypto, tool, and test features are disabled,
and license/provenance records enter notices and the software bill of materials. yyjson permits
duplicate keys structurally, so Bloom performs recursive decoded-key rejection.

Qualification must prove which malformed local/central-header, overlap, trailing-data, attribute,
and ZIP64 cases libzip exposes. Bloom adds a bounded raw preflight for any invariant the library
cannot enforce. Dependency versions and writer implementation are not persisted format identities.

Acquisition, lock, offline build, provenance, and release evidence follow
[`dependency-intake.md`](dependency-intake.md).

## Required Verification

- deterministic manifest/document bytes from repeated and cross-platform writes
- exact member and collection order, especially numeric IDs `1`, `2`, `9`, `10`, and `uint64` max
- every allocator namespace at fresh, deleted-gap, maximum, exhausted, and below-declaration states
- signed integer extrema, normalized rational extrema, finite/subnormal Float64 values, both signed
  zeros, exponent boundaries, and invalid overflow
- Unicode scalar, combining/non-normalized, astral, control, escaped-key collision, malformed UTF-8,
  and lone-surrogate fixtures
- scalar/Vec2 animation, curve/key ownership, final-key normalization, graph connectivity, Layer
  Stack order, future on-disk driver sources producing preserved-read-only results, live
  `DriverBindingSource` producing typed `UnsupportedDocumentFeature` before staging, retained
  `driverBinding` high-water after source removal, and source/schema mismatches
- exact composition-domain boundaries for dimensions, pixel-count product, positive normalized
  frame rate and pixel aspect, and signed-64 positive duration rationals
- exact, sorted, unique `providedNodeTypeIds` coverage for unavailable node types, including
  omissions, overlaps, extras, and foundation-type rejection
- project color-settings ordering, fixed `lin_rec709_scene` process identity, every OCIO locator
  family, locator/portability mismatch, alias rejection, digest spelling, context ordering, and
  reference-only `.ocioz` handling
- opaque record subject/reference policies, absent remappers, orphan targets, invalid base64, and
  per-record spelling/decoded and aggregate spelling/decoded payload limits
- unknown members at singleton and keyed-array depth across edit, reorder, delete, and new-object
  reconciliation; unknown numeric subset boundaries and unknown core discriminators produce
  preserved read-only state
- duplicate/aliased/traversal names, header disagreement, CRC damage, overlap, executable/special
  attributes, encryption, unsupported methods, descriptors, ZIP64, comments, polyglots, excessive
  depth/count/size/ratio, and decompression bombs
- checked operation and shared resident-memory charges at exact, one-byte-over,
  arithmetic-overflow, dependency-reservation, concurrent-task, and allocation-failure boundaries;
  all exhaustion paths preserve active state and invoke unpublished-stage cleanup, with cleanup
  faults reported secondarily
- fault injection at stage create, write, close, reopen, validation, flush, target-conflict,
  supersession, replace, directory flush, and cleanup
- cancellation before every publication boundary and cancellation after lease entry
- shared golden archives open and preserve identical document truth on Linux, macOS, and Windows

[ISO/IEC 21320-1]: https://www.iso.org/standard/60101.html
[RFC 8259]: https://www.rfc-editor.org/rfc/rfc8259
[RFC 8785]: https://www.rfc-editor.org/rfc/rfc8785
[RFC 4648]: https://www.rfc-editor.org/rfc/rfc4648
[RFC 8089]: https://www.rfc-editor.org/rfc/rfc8089
[libzip 1.11.4]: https://libzip.org/documentation/
[zlib 1.3.2]: https://github.com/madler/zlib/releases/tag/v1.3.2
[yyjson 0.12.0]: https://github.com/ibireme/yyjson/releases/tag/0.12.0
