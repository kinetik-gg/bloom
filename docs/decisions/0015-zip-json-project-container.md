# ADR 0015: Constrained ZIP And JSON Project Container

Status: accepted

Date: 2026-08-25

## Context

Bloom needs a native `.bloom` format because no adopted interchange standard represents its full
authoring graph, layer boundaries, parameters, extension data, and future migration needs. The
format must remain inspectable and recoverable while eventually carrying optional embedded payloads.
It also opens untrusted input and must behave consistently on Linux, macOS, and Windows.

A plain JSON file is easy to inspect but provides no durable container namespace for future embedded
data. SQLite, Protocol Buffers, and FlatBuffers add useful capabilities in other problem domains but
reduce direct inspectability or complicate opaque extension preservation without removing Bloom's
need for an explicit schema and migration policy.

## Decision

- Define `.bloom` v1 as a constrained ZIP document container aligned with the core profile in
  ISO/IEC 21320-1.
- Require `manifest.json` and `document.json` as strict UTF-8 RFC 8259 JSON.
- Encode typed 64-bit IDs and rational components as canonical decimal strings.
- Persist validated durable-ID allocation high-water marks so deleted IDs are not reused across a
  save/reopen boundary.
- Make semantic JSON output deterministic through stable object-member and record ordering.
- Preserve semantic Layer Stack order rather than sorting it.
- Version container and document schemas independently with major/minor compatibility rules and
  sequential DOM migrations.
- Preserve declared optional-module data in document-owned opaque records and preserve unknown
  additive JSON through Project I/O round-trip state.
- Treat an unprovable preservation path as degraded/read-only rather than silently dropping data.
- Run synchronous codec calls only from the task system; the UI thread never performs project I/O.
- Publish saves atomically through a narrow, parity-tested platform primitive.
- Qualify libzip 1.11.4, its required zlib 1.3.2 dependency, and yyjson 0.12.0 as private pinned
  implementation dependencies. Their library types do not appear in public headers or persisted
  data.

The detailed contract is maintained in
[`../architecture/project-format.md`](../architecture/project-format.md).

## Consequences

- Projects remain inspectable with common ZIP and JSON tools while gaining a namespace for future
  embedded content.
- Bloom owns and must rigorously test a native schema; `.bloom` is not presented as an interchange
  standard.
- Resource limits, duplicate detection, migration fixtures, unknown-data preservation, and atomic
  publication are correctness requirements rather than later hardening.
- Project I/O gains three small C dependencies, all permissively licensed, privately linked, and
  included in third-party release records.
- Exact archive bytes may vary until cross-platform normalization is proven, while decoded project
  semantics and canonical JSON remain deterministic requirements.

Primary references:

- [ISO/IEC 21320-1:2015](https://www.iso.org/standard/60101.html)
- [PKWARE ZIP Application Note](https://support.pkware.com/pkzip/appnote)
- [RFC 8259: The JavaScript Object Notation Data Interchange Format](https://www.rfc-editor.org/rfc/rfc8259)
- [libzip documentation](https://libzip.org/documentation/)
- [libzip build dependencies](https://libzip.org/guides/building/)
- [libzip license](https://github.com/nih-at/libzip/blob/v1.11.4/LICENSE)
- [zlib 1.3.2](https://github.com/madler/zlib/releases/tag/v1.3.2)
- [zlib license](https://github.com/madler/zlib/blob/v1.3.2/LICENSE)
- [yyjson 0.12.0](https://github.com/ibireme/yyjson/releases/tag/0.12.0)
- [yyjson license](https://github.com/ibireme/yyjson/blob/0.12.0/LICENSE)
