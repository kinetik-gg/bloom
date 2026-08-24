# ADR 0011: Canonical Graph With Native Layer Authoring

Status: accepted

Date: 2026-08-25

## Context

Traditional layer interfaces are approachable for motion and structured object editing, while node
graphs expose dataflow and advanced compositing topology directly. Implementing them as separate
models would require translation or synchronization and would eventually produce disagreement in
rendering, undo, animation, persistence, and extension behavior.

Bloom needs both workflows to be first-class.

## Decision

- A composition owns one canonical typed processing graph and one explicit output.
- Layers are explicit, stable, addressable semantics within that graph and its native ordered Layer
  Stack, not a second render model.
- The layer timeline, node editor, Properties, and viewer manipulate the same stable object and
  parameter IDs through the same command path.
- Arbitrary valid graph topology remains canonical and is never automatically flattened or inferred
  into layers.
- Layer exposure and conversion are explicit graph commands that preserve existing upstream
  topology.
- Evaluation compiles one immutable snapshot to backend-neutral semantics used by CPU and GPU
  execution.

The working representation, terminology, hard-case contracts, and initial proof are defined in
[`../architecture/layer-graph-model.md`](../architecture/layer-graph-model.md).

## Consequences

- Layer authoring can remain intuitive without hiding a second processing system.
- Node artists retain complete access to topology, shared subgraphs, drivers, and graph-only
  processing.
- Layer operations must be lossless localized graph patches or be blocked with a clear explanation.
- Stable layer, slot, node, edge, parameter, animation, and keyframe identities are required.
- The timeline must disclose custom and graph-only structures rather than fabricating equivalent
  rows.
- Document, command, serialization, snapshot, and evaluation tests must exercise both authoring
  projections against identical underlying state.
