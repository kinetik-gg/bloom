# ADR 0022: Language-neutral host boundary

Status: accepted

Date: 2026-09-17

## Context

Bloom needs a headless host boundary before Python, MCP, and AI providers are attached. The UI,
future Python facade, and other clients must share project truth, command validation, undo history,
task ownership, rendering, and publication. Exposing mutable document objects or allowing arbitrary
client code in the render path would make those clients a second implementation of Bloom's
semantics.

## Decision

Adopt six Qt-free contracts in `bloom::scripting` as the language-neutral host facade:

- immutable revisioned read snapshots and stable-ID query proxies;
- a typed command gateway with operation IDs, argument schemas, expected revisions, and atomic
  transactions;
- cancellable task submission over Bloom's task scheduler with progress and diagnostics;
- typed subscriptions for revision, history, and rejection events;
- contribution and operation registries with explicit capability discovery; and
- diagnostics and capability reporting with typed outcomes rather than exceptions as the API
  protocol.

`bloom-cli` is the first non-Qt client. Python, MCP, and AI providers are clients of these same
contracts; they do not receive direct access to `Document`, renderer internals, Qt objects, GPU
handles, or mutable C++ pointers. No client code executes in a render kernel, render scheduling
inner loop, GPU shader, or deterministic evaluation path. Bloom owns truth, commands, animation,
data, compositing, and output publication.

Operation IDs are stable, namespaced strings (`bloom.<domain>.<kebab-verb>`). Each ID has one
descriptor containing its argument schema and factory. A descriptor may report a typed capability
or argument diagnostic when a host build does not provide an adapter, but it remains discoverable.
Stable IDs and revision tokens are the only durable object identity exposed to clients.

The facade is versioned independently from the document schema. A v1 client may rely on additive
descriptor and capability discovery, stable meaning for existing IDs, and explicit rejection when a
host cannot provide an operation. Removing or changing the meaning of an ID requires a new facade
major version; schema additions and new optional fields are minor-version changes. The host reports
its facade version and capabilities, and never silently downgrades a request.

## Consequences

The UI remains a thin adapter over the same command and event seams. Headless rendering uses the
existing compile, evaluate, display-preparation, analysis, approval, and publication chain; the
range runner owns exact frame-time iteration. Python and MCP bindings can be added without changing
document storage or renderer semantics. More adapters and contribution registries can be added
incrementally while every request still has an owning module, a typed result, and a cancellation
boundary.

## SCRIPT-1 client addendum (2026-09-17)

The optional Python client binds these contracts with nanobind 3.0.1. CPython's C API owns isolated
interpreter startup/shutdown; nanobind owns extension binding, not embedding lifecycle. The native
core is precompiled by the dependency superbuild against the host Python SOABI, with robin-map
recorded as its vendored transitive. Host CPython plus development headers is an explicit
SCRIPT-1 development prerequisite rather than a newly bundled or qualified dependency.

The pure-Python layer derives keyword signatures from registry descriptors, projects read-only
stable IDs, groups operations with expected revisions, and delivers event callbacks only through
the subscribing thread's pump. Native task tickets observe Python work running on a separate
Python worker. The evaluator and render kernels remain native. Every registered ID carries an
implemented factory in this shared registry rather than a client-specific implementation.

The Script editor borrows the live host through `SessionBinding`: immutable snapshot and context
callbacks, queued transaction/history callbacks, a native event stream and the existing publication
services. No mutable document pointer crosses the binding. The UI thread drains one authoring
transaction per pump slice; ordinary Python runs on the dedicated scripting thread. Native waits
release the GIL. A generation token revokes the binding on New/Open, and stale queued transactions
are refused before reaching the replacement document. Render requests pin one snapshot and carry
cancellation through compile, analysis and publication. The native event stream makes subscription
release a lifetime barrier; callbacks only enqueue native records and cannot reenter the stream.

One interpreter is shared by the editor registry and initialized lazily. Closing an individual panel
does not terminate it. Application shutdown requests cancellation and joins the interpreter only
after its queued requests have been revoked. Python tracing supplies cancellation and a foreground
GUI CPU budget; long work uses `bloom.tasks` and native progress tickets. Trusted native extensions
still need cooperative shutdown. `.pyi` conformance and UI tests cover keyframe/undo, replacement,
responsiveness, cancellation and Python/CLI/UI render-byte parity.

The Qt-free `bloom-mcp` process implements JSON-RPC 2.0 over MCP stdio using qualified yyjson. Its
five tools map to Query, atomic transactions, the reference render runner, MEDIA-4's owned sequence
export runner and event subscriptions. Input schemas, bounded memory/queues, request-ID cancellation
and protocol errors belong to this client; project validation, undo, preservation analysis and
publication remain in their existing owners. It does not expose an HTTP service or Python source
execution. The user guides record protocol versions, limits and platform-dependent export support.

## SCRIPT-2 ergonomics addendum (2026-09-18)

Every registered operation ID now has a complete argument schema and a working factory, so
discovery and callability are the same set. Each `ArgumentSchema` names its kind, carries a one-line
summary and an example value of that kind, and the schema publishes the shortest working call. A
rejection therefore names the operation, the argument, what was expected and a call the artist can
paste, identically in Python, MCP and the JSON/CLI path.

Arguments that name the object an artist is already looking at carry a `ContextSource`
(`composition`, `time`, `layer`, `selection`, `project`). The host fills an omitted contextual
argument from the live session before the factory runs, so every client applies the same defaults
and reports them from the same flag; a context that cannot answer produces a rejection that says so
rather than a bare missing-keyword error. Explicit targets are never overwritten, which keeps
headless and pipeline calls deterministic.

`ValueKind` gains `time` (a whole frame or an exact numerator/denominator pair) and `value` (an
authoring value whose width the operation decides from the parameter it edits). These name argument
shapes the registry already accepted ad hoc; they add no document schema and no new identity.
