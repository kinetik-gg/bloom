# ADR 0012: Python Scripting And Tiered Add-ons

Status: accepted

Date: 2026-08-25

## Context

Bloom needs a Blender-like artist automation API, studio pipeline integration, headless scripting,
and a credible route for third-party extensions including custom UI. Supporting multiple public
languages or exposing internal document/Qt objects directly would multiply compatibility cost and
violate Bloom's command, threading, modularity, and project-preservation rules.

## Decision

- Python is Bloom's one public scripting and add-on language.
- Follow the Python minor version selected by Bloom's VFX Reference Platform generation; the first
  target is CPython 3.13.x for CY2026.
- Bundle a private interpreter and expose stable proxies, immutable reads, typed commands,
  transactions, tasks, events, diagnostics, and contribution registries.
- Organize the public API around Blender-familiar `bloom.context`, `bloom.data`, `bloom.ops`,
  `bloom.types`, and explicit `register()` / `unregister()` concepts. Keep permission checks at the
  host gateways and keep persistent mutation on the command/undo path.
- Treat that familiarity as clean-room behavioral inspiration only: do not copy Blender source,
  add-on implementation code, generated bindings, assets, or GPL-licensed implementation material.
- Use standard wheels, `pyproject.toml`, and namespaced Python entry points plus a small inspectable
  Bloom manifest.
- Accept wheels, ZIP/source archives, local folders, and Git URLs through one inert staging and
  validation pipeline. Resolve Git sources to commits and never execute code during root discovery.
- Make process-hosted Python plus host-rendered schema UI the long-term ordinary third-party tier.
- Reserve full PySide QWidget integration for trusted, GUI-only, exact-version add-ons using a
  Bloom-built matching PySide/Shiboken stack.
- Keep native C++ UI and stable external ABI as separate future compatibility tiers.
- Never execute project scripts or enable/install project-required add-ons automatically.
- Keep Python out of render inner loops and deterministic expression semantics.

The detailed runtime, packaging, lifecycle, UI, security, project-data, and rollout contract is in
[`../architecture/scripting-and-addons.md`](../architecture/scripting-and-addons.md).

## Consequences

- Python aligns Bloom with artist familiarity, VFX studio pipelines, ASWF library bindings, and
  official Qt Python tooling.
- Lua is not maintained as a duplicate core API; optional pipelines may own it for a concrete need.
- The public SDK depends on language-neutral command/task/registry boundaries, not direct access to
  C++ internals.
- In-process code is trusted and can still freeze or crash Bloom; permissions must not be described
  as a sandbox.
- Missing add-on data and editor IDs remain preservable and diagnosable.
- A credible release requires API versioning, stubs, documentation, package validation, safe mode,
  diagnostics, headless parity, and Linux/macOS/Windows fixtures.
