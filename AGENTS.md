# Bloom Repository Instructions

## Authority

- Treat `docs/` in this repository as the canonical current Bloom documentation.
- Treat external Bloom memory as non-normative context unless a repository document explicitly
  adopts a decision.
- If code and repository documentation disagree, verify the live behavior and update the owning
  repository document with the change.

## Reading Order

1. Read `docs/README.md`.
2. Read `docs/product/foundation.md`.
3. Read `docs/architecture/overview.md` before changing source boundaries.
4. Read the exact UX, architecture, or decision record relevant to the task.

## Engineering Boundaries

- Use C++20 and Qt 6 for the application shell.
- Keep Qt types inside `src/ui` and `apps` unless a decision record explicitly changes the rule.
- Treat Linux, macOS, and Windows as first-class targets. A feature is incomplete without equivalent
  behavior or an explicit supported fallback on all three.
- Prefer established open standards and their reference libraries over Bloom-specific interchange,
  color, media, and pipeline mechanisms.
- UI surfaces read project state and issue commands; they do not own or directly mutate project
  truth.
- The menu bar is the only fixed application surface. Workspace content is composed from replaceable
  editor panels.
- Never perform media I/O, decoding, evaluation, rendering, proxy generation, hashing, import,
  export, or other potentially heavy work on the UI thread.
- Long-running work must expose cancellation, progress or indeterminate activity, diagnostics, and
  safe shutdown behavior.
- Keep project state, session/UI state, and derived runtime/cache state separate.
- Keep render semantics independent of a particular GPU or operating-system API. Maintain a CPU
  reference path and capability-tested accelerated paths.
- Build a modular monolith: explicit module ownership and pipeline extension registries, with
  direct typed C++ dependencies for ordinary in-tree code. Do not introduce a global service
  locator, catch-all event bus, or interface for every class.
- Keep module dependencies declared and acyclic. Optional pipelines register capabilities through
  owning editor, document, command, node, render, adapter, and task boundaries.
- Prefer a small vertical slice over speculative subsystem breadth.
- Add tests for durable document, command, project I/O, and rendering behavior.
