# ADR 0007: Cross-Platform Parity

Status: accepted

Date: 2026-08-25

## Context

Bloom targets artists on Linux, macOS, and Windows. Native graphics, media, window-system, and
filesystem APIs differ substantially, and allowing product code to depend directly on one of them
would produce platform-only features, incompatible projects, and divergent render behavior.

## Decision

- Treat Linux, macOS, and Windows as first-class platforms from the first vertical proof.
- Define product, project, command, and render semantics independently of operating-system APIs.
- Use Qt 6 as the default cross-platform desktop boundary and Bloom-owned interfaces for platform,
  task, media, and render services.
- Localize native API use and conditional compilation to adapters and backend implementations.
- Permit a platform-specific optimization only when every supported platform has an equivalent
  implementation or an explicit, tested fallback with preserved project data.
- Require runtime capability reporting. Missing optional capabilities must degrade visibly or fail
  with an actionable diagnostic; they must never silently alter final output.
- Require shared Linux, macOS, and Windows tests, cross-platform project fixtures, and packaging
  validation before a user-facing feature is considered complete.
- Keep platform paths, UI layout, device resources, caches, and other machine state out of canonical
  project truth.

The detailed contract is defined in `docs/architecture/platform-support.md`.

## Consequences

- Features are designed around portable behavior before native optimization.
- Some operating-system conveniences will use idiomatic presentation but retain equivalent Bloom
  actions and outcomes.
- GPU and media integrations need capability layers and conformance fixtures rather than leaking
  backend objects into product code.
- CI, release engineering, dependency packaging, and cross-platform testing are part of feature
  completion rather than deferred release work.
- A valuable feature may remain experimental until parity or a truthful fallback exists.
