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
- Keep one reviewed, machine-readable dependency lock in the repository. Every direct and
  transitive component records its exact version and immutable source, SHA-256 digest, upstream
  signature or provenance when published, SPDX license expression, patches, build options,
  dependency edges, supported platforms, and whether it ships in a release artifact.
- Make the reviewed lock bytes canonical and domain-separate their SHA-256 identity. Reject a lock
  whose parsed value cannot reproduce its exact versioned canonical bytes; prefix manifests store
  that identity rather than a parser-dependent JSON digest.
- Split source acquisition from compilation. An explicit online acquisition step may populate a
  content-addressed source cache, but a qualified build and all release builds must succeed with
  networking disabled. Verified archives are extracted through checked cross-platform containment,
  entry, path, type, expanded-byte, and expansion-ratio limits before a recipe can inspect them.
- Freeze source-member portability as `bloom.portable-archive-path.v1`: strict UTF-8, Unicode 15.1
  NFC plus full Default Case Folding collision keys, slash-only relative segments, Windows
  device/ADS/trailing-dot-space rejection, and exact duplicate/prefix checks. Reject absolute,
  drive, UNC, backslash, NUL, dot, dot-dot, link, special-file, and portable-collision entries before
  extraction; host filesystem behavior never defines acceptance.
- Disable dependency-owned automatic downloads and undeclared feature discovery. Optional tools,
  bindings, codecs, tests, examples, and plugins are off unless the lock and a Bloom capability
  require them.
- Make the qualified prefix self-describing. It carries the lock digest, toolchain and target
  identity—including CMake, generator/build tool, compiler, linker, standard library, SDK,
  deployment target, normalized environment, and reproducible timestamp—build-option digest, patch
  digests, installed-file manifest, and per-component license and source records. Bloom
  configuration rejects a prefix whose identity or required capability does not match the requested
  build profile.
- Release and CI builds use only qualified prefixes. A clearly marked developer-system mode may use
  locally installed packages for iteration, but it is non-release, emits its resolved dependency
  report, and cannot establish conformance or update the lock implicitly.
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
- A developer may opt into faster system-package iteration, but that result is visibly unqualified
  and cannot be mistaken for a release build.

## References

- [Dependency intake architecture](../architecture/dependency-intake.md)
- [VFX Reference Platform](https://vfxplatform.com/)
- [SPDX 2.3 specification](https://spdx.github.io/spdx-spec/v2.3/)
- [SLSA provenance model](https://slsa.dev/spec/v1.1/provenance)
- [CMake `ExternalProject`](https://cmake.org/cmake/help/latest/module/ExternalProject.html)
