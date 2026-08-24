# Bloom Project Format

Status: accepted

Updated: 2026-08-25

## Purpose

`.bloom` is Bloom's durable authoring container. It preserves the canonical document model needed
to reopen and evaluate a project; it is not a general VFX interchange format. OCIO, OpenEXR,
OpenTimelineIO, and other adopted standards remain explicit adapters at their own boundaries.

The first format must be inspectable, deterministic at the semantic level, safe to open from
untrusted sources, forward-migratable, and practical on Linux, macOS, and Windows.

## Container Contract

Version 1 uses a constrained ZIP document container aligned with the core profile described by
[ISO/IEC 21320-1]. Bloom imposes a smaller application profile:

- UTF-8 entry names with `/` separators
- no encryption, split archives, executable entries, or platform-dependent path semantics
- no absolute paths, empty components, `.` components, `..` components, or duplicate entries
- an explicit allowlist of required entries and manifest-declared payload entries
- bounded entry count, compressed size, expanded size, nesting depth, and compression ratio
- CRC and structural validation before decoded content enters document construction
- no extraction of the archive to a working directory during normal open

The initial archive contains exactly:

```text
manifest.json
document.json
```

Future embedded media, thumbnails, or module payloads use manifest-declared paths and do not change
the meaning of those two required entries.

The manifest starts with this shape:

```json
{
  "format": "org.kinetik.bloom.project",
  "containerVersion": { "major": 1, "minor": 0 },
  "document": {
    "path": "document.json",
    "schemaVersion": { "major": 1, "minor": 0 }
  },
  "requirements": []
}
```

Exact JSON Schemas and golden fixtures enter the repository with the first codec implementation.

## Document Encoding

`manifest.json` and `document.json` are strict UTF-8 [RFC 8259] JSON. Bloom's reader rejects byte
order marks unless a future schema explicitly permits one, duplicate object keys, invalid Unicode,
non-finite numbers, comments, trailing commas, and JSON5 extensions.

Encoding rules are explicit:

- Typed 64-bit IDs are canonical unsigned decimal strings, preventing precision loss in generic
  JSON tools.
- Rational numerators and denominators are canonical signed decimal strings.
- Parameter values carry a discriminator such as `bool`, `int64`, `float64`, `vec2`, `string`, or
  `rational`.
- Parameter sources carry a discriminator such as `constant`, `animation-curve`, or `driver`.
- Edge destinations distinguish `node-input` from `layer-stack-input`.
- Object members have a fixed writer order.
- Nodes, edges, parameters, boundaries, and parameter bindings are written in stable ID or key
  order.
- Layer Stack entries retain semantic authoring order and are never sorted.
- Finite floating-point values use the shortest correctly rounded decimal representation that
  round-trips to the same binary value.

The document revision, undo history, selection, panel layout, caches, proxies, thumbnails, and
compiled runtime plans are not project truth and are not written to `document.json`.

Durable-ID allocation high-water marks are written and validated against every decoded record.
They prevent a deleted ID from being reused after save/reopen and accidentally retargeting a
preserved extension reference. A missing watermark in a migrated early fixture may be reconstructed
from existing IDs once, but normal v1 saves preserve the allocator state explicitly.

Deterministic `document.json` bytes are a v1 test requirement. ZIP member order, timestamps,
attributes, and compression method are normalized as implementation policy, but exact whole-archive
byte equality is not a compatibility promise until fixtures prove it across all supported hosts.

## Versions, Migrations, And Preservation

Container and document schemas use `{major, minor}` versions:

- Unknown major versions are rejected without mutation.
- A newer minor version may open only when every unknown construct is safely additive and
  preservable.
- Supported older schemas migrate through sequential, deterministic DOM migrations before trusted
  document decoding.
- Unknown core discriminators make the project degraded and read-only rather than guessed.
- Missing optional modules preserve their declared data and block only dependent evaluation.

Two preservation mechanisms remain separate:

1. Declared module authoring data belongs in document-owned opaque extension records identified by
   owner ID, type ID, schema version, optional stable subject, media type, and payload bytes. The
   document model does not depend on JSON.
2. Unknown additive JSON members are retained by Project I/O in a move-only round-trip state. The
   writer overlays known fields onto retained data and reconciles array objects by stable ID, never
   by array position.

In-place save is disabled when Bloom cannot prove that unknown data will survive. Semantic JSON is
normalized when written; preserving original whitespace or key order is not promised.

## Project I/O Boundary

`src/project` owns container parsing, schemas, migrations, diagnostics, and document encoding. It
depends directly on `src/core` and `src/document`. ZIP and JSON implementation types stay private.
The public surface remains concrete rather than becoming a generic serialization framework:

```cpp
struct OpenedProject {
    std::unique_ptr<document::Document> document;
    RoundTripState roundTrip;
    FormatVersion sourceVersion;
    Editability editability;
    std::vector<Diagnostic> diagnostics;
};

OpenResult openProject(const std::filesystem::path& path, const OpenOptions& options);

SaveResult saveProject(const std::filesystem::path& path,
                       const document::Snapshot& snapshot,
                       const RoundTripState& roundTrip,
                       const SaveOptions& options);
```

The API is synchronous internally but must run only through the task system. UI callbacks never
parse, compress, flush, or wait for project I/O. Open and save accept cancellation, progress, and
resource limits. Cancellation ends before the atomic publication step begins.

## Atomic Save

Saving stages a securely created file in the target directory, closes it, reopens and validates the
archive, then publishes through a narrow `src/platform` primitive:

- Linux and other POSIX hosts flush the staged file, atomically rename it, then flush the parent
  directory.
- macOS follows the same publication model with the strongest supported durability flush.
- Windows flushes the staged file and uses `ReplaceFileW` for an existing target or a same-volume
  move for first publication.

Any failure before atomic publication leaves the previous project untouched; staging cleanup is
best effort and diagnosable. If replacement succeeds but a subsequent directory durability flush
fails, the new project may already be visible. Bloom reports a distinct
`PublishedWithDurabilityWarning` outcome rather than claiming that the previous file survived or
that the save completed cleanly. Saves are serialized per canonical target so an older background
task cannot overwrite a newer one. `SaveResult` reports the snapshot revision that reached disk,
and the UI clears dirty state only when the live document still has that revision.

## Asset Locators

Asset paths enter the format only alongside a stable `AssetRegistry`:

- nodes reference stable asset IDs, never filesystem paths
- project-relative paths use UTF-8 and `/` separators
- an absolute [RFC 8089] file URI may be retained only as a relinking hint
- relative paths resolve against the `.bloom` file's parent directory
- saving does not case-fold paths or resolve symlinks
- embedded entry paths obey the container path profile

The first document codec does not invent provisional asset fields.

## Qualified Implementations

The initial qualification targets are:

- [libzip 1.11.4] under BSD-3-Clause for ZIP/ZIP64 reading and writing
- [zlib 1.3.2] under the zlib license as libzip's required compression and CRC dependency
- [yyjson 0.12.0] under MIT for strict JSON, exact 64-bit integers, correctly rounded doubles,
  mutable DOMs, and bounded custom allocation

These are private implementation dependencies pinned by release archive and cryptographic hash.
Their headers are treated as external, their optional features are minimized, and their licenses
and source provenance enter release notices and the software bill of materials. yyjson permits
duplicate keys at the data-structure level, so Bloom performs recursive duplicate-key rejection.

The selected versions are qualification inputs, not persisted format identifiers. A dependency may
be replaced without changing `.bloom` semantics.

## Verification

Project I/O is not complete until tests cover:

- semantic round-trip and deterministic JSON ordering
- maximum IDs and signed integers, rational normalization, exact doubles, and Unicode
- unknown members, unavailable module records, and supported migrations
- malformed JSON and ZIP structures, duplicate keys and entries, traversal paths, and zip bombs
- cancellation and progress behavior outside the UI thread
- atomic-save fault injection and stale concurrent saves
- golden fixtures and parity on Linux, macOS, and Windows

[ISO/IEC 21320-1]: https://www.iso.org/standard/60101.html
[RFC 8259]: https://www.rfc-editor.org/rfc/rfc8259
[RFC 8089]: https://www.rfc-editor.org/rfc/rfc8089
[libzip 1.11.4]: https://libzip.org/documentation/
[zlib 1.3.2]: https://github.com/madler/zlib/releases/tag/v1.3.2
[yyjson 0.12.0]: https://github.com/ibireme/yyjson/releases/tag/0.12.0
