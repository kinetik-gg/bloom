# ADR 0009: Modular Monolith With Pipeline Extension Points

Status: accepted

Date: 2026-08-25

## Context

Bloom needs clear subsystem ownership and room for substantial future pipelines. An extreme
fitness test is integrating a game-engine pipeline with its own assets, editors, graph/evaluation
behavior, renderer services, and build jobs.

Making the current application tightly coupled would require invasive core changes for such a
pipeline. Making every object dynamically pluggable would instead create indirection, lifecycle,
ABI, performance, and debugging costs before those extension points are needed.

## Decision

- Build Bloom as a modular monolith with one application composition root.
- Use ordinary direct typed C++ dependencies within and between known foundation modules.
- Introduce interfaces and registries only at demonstrated replaceability, optionality, platform,
  task, rendering, standards-adapter, or external-integration boundaries.
- Support optional first-party pipeline modules as source-built, statically linked modules by
  default.
- Let pipeline modules register editor panels, assets and namespaced document data, commands,
  nodes/evaluators, render services, standards adapters, and jobs through owning registries.
- Require module dependencies to be explicit and acyclic.
- Pass host services explicitly; do not use a global service locator or catch-all event bus.
- Keep Qt, backend GPU objects, and mutable project stores out of general module contracts.
- Preserve unavailable module data and report missing capabilities rather than silently discarding
  or rewriting it.
- Treat a stable external plug-in ABI, dynamic loading, hot reload, and process isolation as
  separate future capabilities with explicit compatibility and security contracts.

The detailed contract is defined in
[`../architecture/module-system.md`](../architecture/module-system.md).

## Consequences

- Bloom can grow coherent pipelines without turning its core into a collection of special cases.
- Current first-party code remains readable, optimizable, and debuggable without pervasive
  indirection.
- Extension surfaces need stable IDs, versioned descriptors, capability negotiation, lifecycle
  ownership, and conformance tests.
- A future game-engine pipeline can be source-built into Bloom without requiring a binary plug-in
  ABI on day one.
- Public binary compatibility is not implied by internal modularity.
