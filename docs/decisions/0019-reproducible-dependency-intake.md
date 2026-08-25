# ADR 0019: Reproducible Dependency Intake

Status: accepted

Date: 2026-08-25

## Context

Bloom distributes a native application on Linux, macOS, and Windows and depends on Qt and an
increasing set of VFX libraries. Allowing the application build to download whichever source is
current, accept an arbitrary system package, or let one dependency fetch an undeclared transitive
dependency would make releases difficult to reproduce and their licenses and security posture
impossible to audit reliably.

The VFX Reference Platform is a valuable compatibility baseline, but it deliberately specifies
version families rather than Bloom's exact sources, build options, patches, hashes, or shipped
files. Package-manager lock files alone also do not describe the native build graph and binary
distribution obligations across all three targets.

## Decision

- Maintain a separate CMake dependency superbuild. It produces a versioned, relocatable prefix;
  the normal Bloom build consumes that prefix through package configuration and never builds
  third-party source as a side effect.
- Keep one reviewed, machine-readable dependency lock in the repository. Every direct and reached
  transitive component, including vendored code, has its own exact component record; graph records
  are direct edges whose per-profile transitive closure must be complete and acyclic. Each record
  fixes version and immutable source, SHA-256 digest, upstream signature or provenance policy,
  SPDX license expression, patches, build options, capabilities, platform profiles, distribution
  roles, conformance fixtures, and security review.
- Bind every repository-owned patch, provenance item, copyright/license/notice file, review record,
  security or vulnerability disposition, and conformance fixture through an exact path plus SHA-256
  artifact reference. A pathname or repository revision alone is not evidence identity. A qualified
  prefix copies and inventories the exact supporting bytes for every participating component.
- Freeze closed v1 lock and prefix-manifest shapes, member order, canonical JSON profile, absolute
  byte/depth/value/string/list ceilings, schema paths, production paths, and fixture-only paths.
  Reject unknown members, duplicate decoded keys, non-canonical bytes, or a fixture used as a
  production artifact.
- Domain-separate both identities. The lock identity covers the exact reproduced lock bytes; the
  prefix-manifest identity covers the exact complete manifest, including lock identity,
  qualification results, and installed-file digests. The manifest does not contain its own digest.
  Only a prefix whose derived gate state passes may expose that digest as a qualified-prefix
  identity.
- Split source acquisition from compilation. An explicit online acquisition step may populate a
  content-addressed source cache, but a qualified build and all release builds must succeed with
  networking disabled. Verified archives are extracted through checked cross-platform containment,
  entry, path, type, expanded-byte, and expansion-ratio limits before a recipe can inspect them.
- Freeze source-member portability as `bloom.portable-archive-path.v1`: strict UTF-8, Unicode 15.1
  NFC plus full Default Case Folding collision keys, slash-only relative segments, Windows
  device/ADS/trailing-dot-space rejection, and exact duplicate/prefix checks. Reject absolute,
  drive, UNC, backslash, NUL, dot, dot-dot, link, special-file, and portable-collision entries before
  extraction; host filesystem behavior never defines acceptance.
- Bootstrap Unicode deterministically: the validator pins the reviewed official Unicode 15.1
  archive and the exact normalization, case-folding, and conformance-file digests outside the lock,
  verifies those raw inputs first, then requires the lock and prefix evidence to reproduce them.
  Ambient language-runtime, OS, locale, or filesystem Unicode tables are not acceptance oracles.
- Disable dependency-owned automatic downloads and undeclared feature discovery. Optional tools,
  bindings, codecs, tests, examples, and plugins are off unless the lock and a Bloom capability
  require them.
- Make the qualified prefix self-describing. It carries the lock digest, toolchain and target
  identity—including CMake, generator/build tool, compiler, linker, standard library, SDK,
  deployment target, normalized environment, and reproducible timestamp—build-option digest, patch
  digests, installed-file manifest, and per-component license and source records. Bloom
  configuration rejects a prefix whose identity or required capability does not match the requested
  build profile.
- Inventory every clean-prefix path except the self-referential manifest. Regular files carry byte
  counts and content digests. Linux/macOS may use only bounded, relative, prefix-contained,
  explicitly inventoried symbolic links; macOS frameworks receive no opaque exception. Windows
  rejects all reparse points, and every platform rejects hard links. Package mutation/signing occurs
  after prefix qualification and has a separate inventory.
- Derive qualification from an exact locked gate/result set using only `passed`, `failed`, and
  `not-applicable`; never trust a stored qualification boolean. Version 1 consumer compatibility is
  exact ABI equality, not a version-range heuristic. Build-tool provenance compares exactly to the
  lock during qualification but is not confused with the later consumer's ABI identity.
- The normal Bloom build performs no dependency acquisition or network access. Release and CI use
  only explicitly selected qualified prefixes and never fall back to a host search. A separately
  and explicitly selected developer-system mode may use locally installed packages for iteration,
  but it is unqualified, emits its resolved dependency report, and cannot establish conformance,
  create a release, or update the lock implicitly.
- Generate an SPDX 2.3 JSON software bill of materials, human-readable third-party notices, license
  payloads, corresponding-source or source-offer material where required, and a shipped-file
  inventory from the resolved prefix and application packaging graph. Verify the packaged artifact,
  not merely the source tree.
- Treat every dependency change as a reviewed supply-chain change. Qualification includes API and
  ABI checks, cross-platform builds, applicable conformance and malformed-input fixtures, license
  review, vulnerability review, packaging, and clean uninstall or upgrade behavior.
- Keep third-party types out of Bloom public interfaces unless another accepted decision explicitly
  makes that external ABI part of Bloom's contract.

The detailed file shape, qualification workflow, and current intake candidates are maintained in
[`../architecture/dependency-intake.md`](../architecture/dependency-intake.md).

## Consequences

- Normal Bloom builds become smaller and easier to reason about because they consume declared
  package targets rather than orchestrating a second build graph.
- Release inputs can be cached, mirrored, rebuilt offline, audited, and compared across platforms.
- Dependency upgrades require intentional lock and qualification changes rather than opportunistic
  version drift.
- The repository must maintain superbuild recipes, patches, provenance, notices, and SBOM tooling.
- Unicode data, schema, canonical fixture, and identity updates become reviewed supply-chain format
  changes rather than ambient tool upgrades.
- A developer may opt into faster system-package iteration, but that result is visibly unqualified
  and cannot be mistaken for a release build.

## References

- [Dependency intake architecture](../architecture/dependency-intake.md)
- [VFX Reference Platform](https://vfxplatform.com/)
- [SPDX 2.3 specification](https://spdx.github.io/spdx-spec/v2.3/)
- [SLSA provenance model](https://slsa.dev/spec/v1.1/provenance)
- [CMake `ExternalProject`](https://cmake.org/cmake/help/latest/module/ExternalProject.html)
