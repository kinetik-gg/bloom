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

## GPU Coverage And Final Render

- `src/runtime/include/bloom/runtime/gpu_coverage_contract.hpp` is the bounded coverage contract.
  GPU production preparation is required by default for every `CompiledOperation` and
  `ImageEffectKernel` alternative, every image-producing authoring lowering, and every required
  feature/route axis. Adding an alternative or lowering without classifying and covering it is a
  build failure, not a warning.
- Preview and final rendering are both required. Interactive viewer preview, still-frame export,
  sequence/range export, video export, and headless/scripted render are required routes through the
  actual production evaluation paths. There is no preview-only exemption. A final render may read
  the single composited image back once at the CPU codec/file boundary; per-node or per-operation
  full-frame roundtrips are not a GPU implementation.
- A silently CPU whole-frame rendered ordinary operation is not a GPU pass. A CPU runner that cannot
  execute the GPU must report an explicit NotRun/Skipped, and a `--require-device` run must fail; an
  unavailable device must produce explicit Unsupported/CPU-fallback provenance, never a CPU frame
  labelled GPU.
- New processing nodes and operations must define CPU/GPU parity against the CPU reference,
  invalidation/cache identity, cancellation, budget and retirement behavior, and cold/warm RAM
  preview evidence before they are complete.
- Agents MUST NOT create or expand a GPU coverage opt-out to turn a check green without a specific
  user or reviewer instruction. A typed exception may cover an actual pixel operation, feature, or
  route only when it names a non-empty id, a rationale, and an owning document or accepted decision
  reference; no such pixel exception exists today and approval identifiers are never fabricated.
  Host-preparation declarations (media I/O and decompression, font shaping, parameter/geometry
  resolution, one final readback) live in a separate audited list and are not a route to exempting a
  pixel operation, feature, or route.
- Every Required fixture passes under the same policy: a genuine production GPU preparation result,
  or (native-only) real native execution with parity and cache evidence. There is no
  "admitted-but-not-prepared" pass.
- Host preparation in the contract covers media container/sample I/O and decompression, font
  shaping, parameter/geometry resolution, and one final readback. It never covers working-space
  colour conversion, which is a pixel transformation and stays required. Media decode and image
  colour transform are distinct.

## Commit Messages

- Every commit must use Conventional Commits: `type(scope): summary` (scope is optional).
- Use the appropriate type, such as `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, or `build`;
  preserve required attribution trailers. This is a hard requirement.
