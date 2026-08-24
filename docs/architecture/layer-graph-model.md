# Layer And Graph Model

Status: working

Updated: 2026-08-25

## Purpose

Bloom is natively node-based and natively layer-authorable. These are two professional workflows
over one document and render pipeline, not separate modes that translate or synchronize competing
representations.

The node editor exposes complete typed dataflow and arbitrary valid topology. The layer timeline
and Properties editor expose structured, task-oriented controls over layer-compatible parts of the
same graph. Neither workflow is treated as a simplified import/export view of the other.

## Accepted Invariants

1. A composition has one canonical typed graph and one explicit output endpoint.
2. Layer order and layer participation are explicit graph semantics, not a second render model.
3. Every editable value has one stable parameter identity. Timeline, Properties, Nodes, and viewer
   manipulation issue commands against the same IDs.
4. Static values, animation, and graph-driven values are explicit mutually exclusive parameter
   sources; a UI edit never silently disconnects a driver or bakes animation.
5. Layer and graph edits use the same typed command, transaction, validation, undo, dirty-state,
   snapshot, and diagnostic paths.
6. Arbitrary valid graph topology remains valid. Opening or editing the timeline never flattens,
   normalizes, copies, or silently rewires it.
7. A graph becomes layer-addressable only through an explicit layer boundary and stack
   participation. Bloom does not infer layers heuristically from arbitrary nodes.
8. The runtime compiles one immutable graph snapshot into backend-neutral evaluation semantics.
   CPU and GPU backends do not reinterpret layer behavior independently.

## Working Graph Shape

The working representation combines an explicit per-layer graph boundary with one ordered stack:

```text
Source -> Effects / Mask / Transform -> Layer Output --+
Source -> Effects / Mask / Transform -> Layer Output --+-> Layer Stack -> Composition Output
Shared or custom graph ----------------> Layer Output --+
```

### Layer Output

A `Layer Output` is an explicit graph node or equivalent first-class boundary that owns a stable
`LayerId`. It declares that one image-producing graph result participates as a layer and exposes the
standard layer-facing property bindings that apply at that boundary.

Deleting or bypassing the boundary may remove the corresponding timeline row and therefore requires
a clear topology preview or warning. An explicit future `Create Layer from Selection` command may
insert a boundary around an existing output, but it must reference the existing graph rather than
copy or translate it.

### Layer Stack

A `Layer Stack` is a native graph operator with one ordered collection of stable entries. Each layer
entry has a stable slot ID. Graph connections target `(stack node ID, slot ID, input role)`, never an
array index such as `input 3`.

Reordering a layer changes only the ordered entry structure. Source, matte, parent, parameter, and
selection references use stable IDs and must not change merely because the row moved.

The stack evaluator preserves declared compositing order because blend operations are generally not
associative. Independent upstream sources may evaluate concurrently before the ordered blend fold.

There is no parallel `Composition::layers` collection that mirrors the stack and no persistent
generated chain of Merge nodes that must be synchronized with it.

The precise ownership of standard transform, time mapping, opacity, blend, and enable parameters
between the Layer Output boundary and its stable stack entry remains an implementation detail for
the document spike. Each property must still have exactly one owning `ParameterId` and one
evaluation meaning.

## Parameters And Properties

Node definitions and layer definitions expose typed property schemas containing stable namespaced
keys, value type, unit, default, validation, animation support, and UI roles. Stored values live in
a composition-owned parameter store and reference animation or driver records by stable ID.

Conceptually:

```text
ParameterSource = Constant | AnimationCurve | DriverBinding
```

Not every property is permanently shown as a socket. A layer or Properties view may present a
curated schema while the node editor exposes the complete node schema. The following actions have
distinct meanings:

- `Show Input Socket` changes node-editor presentation only.
- `Drive from Graph` creates a typed connection.
- `Convert Value to Node` materializes the current literal without changing its value.
- `Convert Animation to Curve Node` materializes the existing curve without baking it.
- `Expose as Group Input` applies only inside a semantic node group.

A graph-driven property remains visibly driven. Typing into its field must not silently replace the
connection; the artist chooses how to disconnect or preserve the evaluated value.

## Editor Projections And Selection

Selection is a tagged stable document reference, not a widget pointer or row index.

- Selecting a layer row or viewer object selects its `LayerId`. Nodes highlights its layer boundary
  and related graph region; Properties shows the structured layer schema.
- Selecting an internal node keeps that node as the primary selection. Timeline may highlight the
  owning layer as context but must not replace the node selection.
- Selecting an effect, mask, parameter, edge, or keyframe selects that actual object.
- Hover and panel focus are session state and do not replace document selection.
- Locking applies consistently to viewer, timeline, Properties, and graph edits targeting the
  locked layer boundary.

Node graph scopes such as Entire Composition, Selected Layer, Node Group, and Nested Composition
are filters over the same graph. They are not copied shadow graphs. Breadcrumbs, Reveal in Full
Graph, Back/Forward, Go Up, Frame Selection, and Frame All make scope explicit.

A visual `Layer Block` may frame related nodes, but its position and collapsed state are graph-editor
session state. Shared upstream nodes may remain outside a block and can feed multiple layers.

## Representability

The layer workflow describes graph structures honestly:

- `Structured`: fully representable and safely editable through layer controls.
- `Custom Graph`: still has a layer boundary, but contains branches or connections that the compact
  effect/property stack cannot fully represent. Safe common properties remain editable.
- `Graph-only Processing`: reachable processing outside the selected Layer Stack, such as a
  post-stack operation. Timeline shows a persistent summary with `Reveal in Graph`, not a fake row.

These states require text or accessible semantics in addition to color. Invalid or broken topology
is a diagnostic rather than an authoring mode.

If Bloom cannot express a requested layer operation as a localized, lossless graph patch, it blocks
the operation or explains the affected topology and offers `Reveal in Graph`. It never deletes
shared nodes, flattens branches, replaces custom merges, disconnects drivers, or bakes animation as
an incidental side effect.

## Commands And Evaluation

Persistent actions such as add/remove/reorder layer, connect ports, set a matte, set a parameter
source, and upsert a keyframe are typed commands. Composite artist actions create one transaction
and one meaningful undo entry. Undo restores exact IDs, records, edges, values, and order rather
than rerunning a topology heuristic against the current graph.

An immutable composition snapshot freezes nodes, stack entries and order, edges, parameter sources,
animation, assets, required type versions, and document revision. Runtime compilation lowers layer
semantics into explicit time-map, mask/matte, transform, opacity, and ordered blend operations in
derived state only.

## Hard-Case Contracts

- **Matte:** explicit typed connection with channel, invert, space, and time policy. Row adjacency
  may be UI sugar but is never render truth.
- **Parent:** transform inheritance only, stored through stable identity and validated as an acyclic
  relation. Parenting does not imply compositing or a matte.
- **Shared graph:** multiple layers may reference one upstream result. Removing a layer does not
  imply deleting shared nodes.
- **Adjustment layer:** explicit stack semantic that consumes the accumulated image below; not a
  transparent ordinary layer.
- **Nested composition:** explicit composition-instance node with time mapping and overrides. The
  child retains its own graph; recursive same-time dependencies are rejected.
- **Cycles:** same-time evaluation cycles and transform-parent cycles are rejected. Future temporal
  feedback requires an explicit delay/feedback node with history and cache semantics.
- **Missing module:** preserve unknown node payloads, layer boundaries, entries, and edges while
  reporting scoped evaluation diagnostics.

## Terminology

- `Layer`: one ordered, stable item in a Layer Stack.
- `Layer Output`: the graph boundary representing one layer.
- `Layer Stack`: the ordered compositing operator represented by the timeline.
- `Effect`: an image-processing node in a recognized layer chain.
- `Mask`: geometry-derived coverage within a layer.
- `Matte`: image-derived coverage connected from a layer or graph output.
- `Parent`: transform inheritance only.
- `Folder`: timeline organization with no render effect.
- `Layer Group`: a nested Layer Stack with explicit compositing semantics.
- `Node Group`: graph encapsulation with exposed ports; it does not imply a layer.
- `Nested Composition`: a separate composition used as a source.

Avoid using the generic word `Group` when one of the distinct terms is intended.

## First Vertical Proof

The first document/runtime slice should prove:

1. Create at least two minimal source nodes, Layer Outputs, stable stack slots, and one composition
   output.
2. Derive timeline rows from the stack and reorder them by changing only stack order.
3. Synchronize primary and contextual selection across viewer, timeline, nodes, and Properties.
4. Edit position and opacity through Properties, Nodes, and viewer manipulation against the same
   parameter IDs.
5. Convert opacity explicitly from a literal to a driven value and undo to the exact prior value and
   topology.
6. Save and reopen stable IDs, order, parameter source, animation, and graph connectivity.
7. Compile one immutable snapshot and render the same result through the canonical evaluation path.

Full effects, masks, mattes, parenting, folders and groups, nested compositions, arbitrary
graph-to-layer conversion, and multi-selection editing remain deferred. Their contracts are
reserved here so the first proof does not create incompatible shortcuts.
