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
