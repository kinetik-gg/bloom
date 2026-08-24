# Dependency Intake And Qualification

Status: working

Updated: 2026-08-25

## Purpose

Bloom's dependency graph is part of the product. It affects decoded pixels, color transforms,
project safety, cross-platform behavior, installation size, licensing, and the ability to reproduce
a release. This contract turns a third-party library from a convenient local package into a
qualified Bloom build input.

ADR 0019 fixes the mechanism: a separate offline-capable superbuild produces a qualified prefix,
and the application build consumes it. This document defines the implementation and acceptance
boundary. The mechanism is accepted; the superbuild, lock, prefixes, and individual dependency
profiles remain unqualified until they pass the gates below.

## Repository Shape

The dependency implementation should enter the repository as one cohesive boundary:

```text
dependencies/
  dependencies.lock.json       canonical reviewed lock
  superbuild/CMakeLists.txt     dependency-only composition root
  superbuild/projects/          one recipe per direct dependency
  patches/<component>/          immutable, documented patches
  licenses/                     reviewed license/source metadata
  schemas/                      lock and prefix-manifest schemas
cmake/
  BloomDependencyPrefix.cmake   normal-build prefix validation
```

The exact directory names may change before implementation, but the ownership separation may not:
the normal root CMake build must not contain a parallel web of `ExternalProject`, `FetchContent`, or
dependency-specific patch logic.

## Lock Contract

`dependencies.lock.json` is itself the reviewed canonical byte stream. Version 1 accepts only this
JSON profile:

- UTF-8 without a BOM or trailing newline; strings are valid Unicode normalized with the Unicode
  15.1 NFC algorithm;
- no insignificant whitespace, floating-point numbers, duplicate object members, or escaped `/`;
- integers use shortest unsigned decimal form, and booleans and `null` use their JSON spellings;
- object members occur in the exact schema order; unknown members are rejected rather than sorted;
- component records are ordered by canonical-name UTF-8 bytes, dependency edges and unordered sets
  use the schema-declared UTF-8 byte order, and patches carry a zero-based `applyOrder` and occur in
  that order; and
- strings emit non-control Unicode directly as UTF-8, escape `"` and `\\`, use the short JSON
  escapes for backspace/form-feed/newline/carriage-return/tab, and encode other U+0000–U+001F
  controls as lowercase `\u00xx`.

The lock identity is lowercase `sha256:` plus SHA-256 over the ASCII domain
`bloom.dependencies.lock.v1`, one zero byte, the canonical byte length as unsigned 64-bit
big-endian, then the exact canonical lock bytes. A parser must reproduce the input bytes from the
typed value and reject a non-canonical file. Schema fixtures freeze member and list order before an
implementation may call this format version 1.

Each component record contains at least:

- canonical name, exact upstream version, immutable source URL, source archive SHA-256, and source
  commit when relevant;
- upstream signature, Sigstore identity, or signed-tag evidence when published, including the
  verification policy rather than only a `verified: true` assertion;
- SPDX license expression, copyright/notice files, source-distribution obligation, linking form,
  modification state, and the owning review record;
- exact patch paths and SHA-256 digests;
- CMake options, generated features, library linkage, runtime plugins, tools, and bindings that are
  enabled or disabled;
- direct and transitive dependency edges, including vendored code that upstream build systems may
  otherwise hide;
- supported target triples and exact profile constraints, including CMake, generator, build tool,
  compiler, linker, standard library, SDK, and deployment-target identities;
- whether source, library, executable, plugin, data, or license files ship in each package profile;
- conformance fixture set, known vulnerability disposition, and qualification date.

Version ranges, floating branches, `latest`, mutable archive URLs, and unrecorded dependency-owned
downloads are invalid lock values. A new version is a lock change even when it promises ABI
compatibility.

## Portable Archive Path Profile

Every source member name passes the versioned `bloom.portable-archive-path.v1` profile before any
filesystem object is created. The profile pins Unicode 15.1 normalization and full Default Case
Folding data; it never uses a host locale, filesystem collation, or platform case conversion. The
table source and digest are lock inputs and prefix-manifest evidence.

A member name must be shortest-form valid UTF-8. `/` is the only separator; any `\` is rejected
rather than translated. A directory entry may have one terminal `/`, which is removed before path
validation, while a file entry may not. After that removal the path must be relative and contain at
least one segment. Leading `/`, `//`, a drive-qualified or drive-relative prefix such as `C:/` or
`C:foo`, a UNC or extended-device prefix, an empty segment, and repeated or trailing separators are
invalid. Percent escapes and other textual encodings are never decoded into separators.

Each segment is normalized to Unicode 15.1 NFC and must satisfy all of these rules:

- it is neither `.` nor `..`, contains no U+0000, C0 control, or U+007F, and contains none of the
  portable-invalid ASCII characters `<`, `>`, `:`, `"`, `|`, `?`, or `*`;
- its normalized UTF-8 encoding is at most 255 bytes, and the normalized slash-joined path is at
  most 1,024 UTF-8 bytes;
- it does not end in U+0020 SPACE or U+002E FULL STOP; and
- the part before its first U+002E, after Unicode 15.1 full Default Case Folding and NFC, is not
  `con`, `prn`, `aux`, `nul`, `clock$`, `conin$`, `conout$`, `com1` through `com9`, `lpt1` through
  `lpt9`, `com¹`, `com²`, `com³`, `lpt¹`, `lpt²`, or `lpt³`.

Rejecting `:` forbids both drive syntax and NTFS alternate-data-stream names rather than relying on
the extraction host to interpret them. The reserved-name rule applies even when the segment has an
extension, so `NUL.txt` and `com1.anything` are invalid.

The normalized path is the extraction path. Its portable collision key is formed by applying
Unicode 15.1 full Default Case Folding and then NFC to each normalized segment and joining the
results with `/`. Before extraction, Bloom rejects duplicate keys, file/directory type disagreement
at one key, and any non-directory key that is a prefix of another member key. These checks include
explicit and implicit directory entries and therefore catch NFC aliases, case aliases, trailing-name
aliases, and archive-order tricks consistently on Linux, macOS, and Windows.

## Acquisition, Build, And Consumption

The pipeline has four explicit phases:

1. `acquire` downloads locked archives and provenance into a content-addressed cache and verifies
   every digest and published signature. It extracts only after verification into an empty staging
   directory through Bloom's bounded archive reader and the exact portable path profile above.
   Version 1 also rejects devices, FIFOs, hardlinks, and symbolic links. It permits at most 4 GiB
   source bytes, 200,000 entries, 16 GiB
   aggregate expanded bytes, 4 GiB per file, 1,024 UTF-8 bytes per logical path, and a 1,000:1
   aggregate expansion ratio. Checked counters and final path-containment verification are
   mandatory; a tighter component ceiling may be locked but may not exceed these hard maxima.
2. `build` runs with networking disabled, applies only locked patches, and installs into a clean
   staging prefix. Dependency-owned auto-fetch is a hard failure. The profile starts from a
   documented environment allowlist, fixes locale and timezone, records `SOURCE_DATE_EPOCH`,
   removes user package registries, and remaps or rejects build/staging paths embedded in outputs.
3. `qualify` runs component, integration, malformed-input, license, and platform gates and writes a
   signed or checksummed prefix manifest.
4. `consume` configures Bloom against that prefix. It validates schema, lock digest, target,
   toolchain/ABI, configuration, and required capabilities before resolving imported CMake targets.

The prefix manifest records exact CMake, generator, build-tool, compiler, linker, standard-library,
SDK, architecture, deployment-target, environment-profile, and `SOURCE_DATE_EPOCH` identities,
plus build configuration, options digest, source/patch digests, installed-file digests, exported
CMake packages, and qualification results. It stores the lock digest defined above, never a digest
of parser-dependent semantic JSON. A release build rejects unqualified or mismatched prefixes
instead of searching the host system as a fallback.

An explicitly enabled developer-system mode may use `find_package` outside a qualified prefix. The
application About/diagnostic report and build metadata label that mode `Unqualified`; CI never uses
it for release or conformance claims.

## Feature Minimization

Each recipe enables only the capabilities Bloom owns. For the first frame-output slice:

- OpenImageIO builds only the library functionality and image plugins required for PNG and OpenEXR;
- command-line tools, tests, examples, Python bindings, video, camera RAW, texture-system extras, and
  unrelated format plugins remain off;
- OpenEXR and Imath use locked external dependencies or explicitly qualified vendored components;
  their build may not silently fetch libdeflate, OpenJPH, or another package;
- OpenColorIO tools, Python bindings, tests, and optional integrations remain off unless a Bloom
  capability separately requires them; and
- all third-party targets are private implementation dependencies and treated as system headers for
  Bloom warning policy without suppressing Bloom adapter warnings.

The lock, not this prose, is the authority on the exact enabled graph.

## Initial Qualification Candidates

The following current releases are the first candidates for the color/output work. OpenColorIO,
OpenEXR, and Imath align with families named by the VFX Reference Platform CY2026. CY2026 does not
specify OpenImageIO, so Bloom selects and qualifies that candidate independently rather than calling
it platform-mandated. None is qualified or shipping until exact archives, hashes, transitives,
options, licenses, and all target builds exist in the lock:

| Component | Candidate | Reason for starting point |
| --- | --- | --- |
| OpenColorIO | 2.5.2 | Current 2.5 security/bug-fix release and CY2026 family |
| OpenImageIO | 3.1.16.0 | Current supported 3.1 candidate with recent malformed-input hardening; not a CY2026-listed component |
| OpenEXR | 3.4.15 | Current 3.4 security/bug-fix release and CY2026 family |
| Imath | 3.2.3 | Current 3.2 release and CY2026 family |

Upgrading a candidate before its first lock is expected if security review finds a newer fix. Once
locked, no automated job rewrites these versions; update automation may open a report or change for
human review.

## Qualification Matrix

Every locked profile must pass:

- GCC and Clang on supported Linux plus the production MSVC and Apple Clang toolchains;
- x86-64 Linux/Windows and Apple Silicon macOS, with deployment targets matching Bloom packages;
- Debug diagnostics and release-like optimized builds without unresolved symbols or accidental
  host-library references;
- exported package relocation and an install-prefix smoke consumer;
- Bloom's deterministic color and image-output fixtures, error translation, cancellation behavior,
  and resource limits;
- malformed and hostile input corpora under sanitizers where supported;
- license/notice/source completeness and automated comparison of actual packaged files to the
  declared distribution graph; and
- vulnerability scanning with manual disposition for reachable findings.

The VFX Reference Platform informs family alignment, especially on Linux. It does not override
Bloom's Windows/macOS parity gate or authorize an unreviewed patch version.

## SBOM And Release Evidence

Each native package carries or is accompanied by:

- an SPDX 2.3 JSON SBOM naming the application and all resolved shipped components;
- `LICENSE`, `NOTICE`, per-component license texts, and required attribution;
- exact corresponding source or a durable source offer where the selected license requires it;
- the dependency lock, prefix-manifest identity, Bloom revision, toolchain identity, and package-file
  digest inventory; and
- Qt replacement instructions and other obligations required by accepted
  [ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md).

The SBOM is generated from the prefix and final packaging graph, then checked against the actual
artifact. A source-only inventory that omits dynamically loaded plugins or bundled tools fails the
gate.

## Upgrade And Incident Policy

An upgrade starts with a reviewed lock proposal, release/security notes, API/ABI assessment, and
license delta. It rebuilds the clean prefix and reruns all affected conformance and package gates.
Bloom never mutates a qualified prefix in place.

When a vulnerability affects a locked component, the response records reachability, exposure to
untrusted content, affected package versions, mitigation, and the replacement lock. Suppressing a
scanner finding without this record does not qualify a release.

Primary references:

- [ADR 0019](../decisions/0019-reproducible-dependency-intake.md)
- [VFX Reference Platform CY2026](https://vfxplatform.com/)
- [OpenColorIO 2.5.2](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases/tag/v2.5.2)
- [OpenImageIO 3.1.16.0](https://github.com/AcademySoftwareFoundation/OpenImageIO/releases/tag/v3.1.16.0)
- [OpenEXR 3.4.15](https://github.com/AcademySoftwareFoundation/openexr/releases/tag/v3.4.15)
- [Imath 3.2.3](https://github.com/AcademySoftwareFoundation/Imath/releases/tag/v3.2.3)
