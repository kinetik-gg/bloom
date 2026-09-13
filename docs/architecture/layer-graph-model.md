# Layer And Graph Model

Status: working

Updated: 2026-09-13

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

Its content input is OPTIONAL, and so is its stack slot. An artist wires a Layer node up by hand, so
"added but not yet fed" and "fed but not yet in the stack" are ordinary intermediate states, not
compositions the compiler refuses to compile: an unfed Layer Output is classified as an empty image and
draws nothing, and a Layer Output with no slot is simply unreachable from the composition output. The
compile reports no diagnostic for either -- there is nothing wrong with a half-built graph.

Deleting or bypassing the boundary may remove the corresponding timeline row and therefore requires
a clear topology preview or warning. An explicit future `Create Layer from Selection` command may
insert a boundary around an existing output, but it must reference the existing graph rather than
copy or translate it.

### Layer Stack

A `Layer Stack` is a native graph operator with one ordered collection of stable entries. Each layer
entry has a stable slot ID. Graph connections target `(stack node ID, slot ID, input role)`, never an
array index such as `input 3`.

Entry ZERO is the TOPMOST layer: the stack folds from its last entry to its first, so the entry list
reads in the order the Timeline lists its rows and the Merge pill draws its segments. A newly added
layer therefore lands at the FRONT of the list, on top of everything already there -- `AddSolidLayer`
and `AddTextLayer` insert it there rather than appending. Appending put every new layer underneath
every existing one, which is why an artist who added a layer and changed its blend mode saw nothing
change: the layer they had just made had only the composition's transparent backdrop beneath it, and
over transparency every separable mode folds to Normal.

Reordering a layer changes only the ordered entry structure. Source, matte, parent, parameter, and
selection references use stable IDs and must not change merely because the row moved.

The stack evaluator preserves declared compositing order because blend operations are generally not
associative. Independent upstream sources may evaluate concurrently before the ordered blend fold.

There is no parallel `Composition::layers` collection that mirrors the stack and no persistent
generated chain of Merge nodes that must be synchronized with it.

The precise ownership of time mapping and enable parameters between the Layer Output boundary and its
stable stack entry remains an implementation detail for the document spike. Each property must still
have exactly one owning `ParameterId` and one evaluation meaning. The BLEND parameter's ownership is
settled: it belongs to the Layer Output boundary (see "Blending" below), because a blend mode is a
property of the layer, while a stack entry is the ordering of layers.

### Layer Transform

The Layer Output boundary owns the layer's complete placement, as five parameters in this authoring
order, all five animatable:

| Role | Schema key | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `position` | `bloom.transform.position` | Vec2d | composition centre, written at creation | Where the layer centre sits, in composition pixels |
| `anchor` | `bloom.transform.anchor` | Vec2d | `{0, 0}` | The pivot, in full-resolution layer pixels measured from the layer centre |
| `scale` | `bloom.transform.scale` | Vec2d | `{1, 1}` | Per-axis unitless factor |
| `rotation` | `bloom.transform.rotation` | Float64 | `0` | Degrees, clockwise on screen |
| `opacity` | `bloom.layer.opacity` | Float64 | `1` | Unit-interval coverage multiplier |

The geometric model, with `p` a point of the layer and `q` the composition point it lands on, both
measured from the layer centre:

```
q = translation + anchor + R(rotation) * S(scale) * (p - anchor)
```

`translation` is `position` minus the composition-format centre, so a `position` of the format centre
leaves the layer unmoved. The anchor is the one point scale and rotation leave alone; translation
then carries the whole layer. Rotation is clockwise because Bloom's y axis points down, and is
defined in composition pixels — a non-square pixel aspect is not divided out, which is the
convention every timeline compositor uses.

Anchor is measured from the layer centre rather than from a corner for two reasons that are both
contract, not convenience. First, the schema default has to be a constant, and `{0, 0}` is the only
composition-independent spelling of "the layer centre"; a corner-relative default would have to know
the composition format, which can also change later and would silently move every anchor with it.
Second, `position` already measures the layer from its centre, so the two Vec2d rows share an origin.

The centre the anchor is measured from is the centre of the layer's pixel AREA. Pixel centres have
integer coordinates, so a `w`-wide layer occupies `[-0.5, w - 0.5]` and its centre is `(w - 1) / 2`.
That half-pixel is what makes a quarter turn map pixel centres exactly onto pixel centres.

Validation is per schema rather than per value kind: `position`, `anchor`, and `scale` are finite and
otherwise unbounded, `rotation` is finite and unbounded, and only `opacity` carries a `[0, 1]`
domain. A negative scale factor mirrors its axis; a scale factor of exactly zero collapses the layer,
which evaluation renders as an empty layer rather than refusing (see
[`evaluation-primitives.md`](evaluation-primitives.md), "Layer Transform Resampling"). A rotation may
wind past a full turn in either direction, because a rotation curve has to be able to.

### Blending

The Layer Output boundary also owns how the layer combines with what is beneath it in the stack:

| Role | Schema key | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `blendMode` | `bloom.layer.blend-mode` | Int64 | `0` (`Normal`) | How this layer's colour combines with the layers beneath it |

The value is an enumeration stored as a small integer under one closed, durable mapping — `Normal`,
`Add`, `Multiply`, `Screen`, `Overlay`, `Darken`, `Lighten`, `Difference` as `0` through `7`. The
mapping, each mode's formula, and the premultiplied compositing fold that applies them are in
[`color-management.md`](color-management.md), "Blend modes". Validation accepts exactly the integers
that mapping names: one that names no implemented mode is refused rather than folded to `Normal`,
because drawing a different mode than the document asked for would be a silent misrender.

Unlike the five values above it, the blend mode is NOT animatable. There is no meaningful value
between `Multiply` and `Screen`, so a curve over it could only hold or jump, which an enable model
would express and an interpolated curve would not; `isScalarAnimatableSchemaKey` and
`isVec2AnimatableSchemaKey` therefore both reject the key, and no command can put it on a curve.

In the registered parameter order the blend mode comes last, after the four geometric values and
opacity: it is the only Layer Output parameter that is not a continuous value at all. That order is
what the properties grid, the node card, and the timeline all read.

`kLayerOutputNodeSchemaVersion` is `3`. A version-1 or version-2 node — every Layer Output written
before these changes — is upgraded on open rather than refused: Project I/O injects the missing
parameters at their defaults, which together are the identity transform and `Normal` blending, so an
upgraded document renders exactly the picture the build that wrote the file produced. See
[`project-format.md`](project-format.md), "Node Schema Upgrades".

## Parameters And Properties

Node definitions and layer definitions expose typed property schemas containing stable namespaced
keys, value type, unit, default, validation, animation support, and UI roles. Stored values live in
a composition-owned parameter store and reference animation or driver records by stable ID.

Conceptually:

```text
ParameterSource = Constant | AnimationCurve | DriverBinding
```

Animation curves are strongly typed, composition-owned declarations rather than untyped bags of
values. The first scalar and `Vec2d` curve model, exact rational sampling, source transitions, and
gesture boundary are defined in [`animation-and-time.md`](animation-and-time.md).

A `DriverBinding` is the durable pair `{sourceNodeId, outputPort}` -- structurally an output-port
reference that lands in parameter-address space instead of on an input port, which is the whole of
what a driver is. It carries no id of its own: a separate driver table would add a collection, an
encoding, and an id space whose only content is that pair, plus a dangling-reference failure mode the
pair cannot have. The `driverBinding` allocator high-water is still persisted even though no record
uses one, so ids issued before this model are never reused (decision 0018).

Every parameter role of a node IS an input socket of its kind, and an operand socket and the
parameter behind it share one name. That pairing is what makes the rules below one sentence each
rather than a per-node table:

- **Unlinked** shows the inline widget, sourced from the parameter's `ConstantValueSource`.
- **Linked** hides the widget and shows only the socket. What makes a role linked is its parameter's
  `DriverBinding` -- not an edge. One authored value has one durable record of where it comes from,
  so `CanonicalGraph::validate()` refuses an edge that terminates on an operand socket and there is
  nothing for an edge and a binding to disagree about.
- An **inline selector** -- a Math node's operation, a Map Range's interpolation, a clamp toggle --
  declares no socket at all, because it decides which kernel the plan compiles and therefore has to
  be known at compile time rather than delivered per frame.
- A Reroute's pass-through is the one socket with no parameter behind it: it carries someone else's
  value, so it is reached by an ordinary edge and is `required`.

A layer or Properties view may still present a curated schema while the node editor exposes the
complete node schema. The following actions have distinct meanings:

- `Show Input Socket` changes node-editor presentation only.
- `Drive from Graph` creates a typed connection.
- `Convert Value to Node` materializes the current literal without changing its value.
- `Convert Animation to Curve Node` materializes the existing curve without baking it.
- `Expose as Group Input` applies only inside node group encapsulation (deferred; see
  **Terminology**).

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
source, and insert/update/delete a typed keyframe are typed commands. Composite artist actions
create one transaction and one meaningful undo entry. An unfinished direct manipulation uses a
session-only compile override and commits once on release. Undo restores exact IDs, records, edges,
values, sources, and order rather than rerunning a topology heuristic against the current graph.

An immutable composition snapshot freezes nodes, stack entries and order, edges, parameter sources,
animation, assets, required type versions, and document revision. Runtime compilation lowers layer
semantics into explicit time-map, mask/matte, transform, opacity, and ordered blend operations in
derived state only. Artist node schemas and backend-neutral primitive semantics remain separate as
defined in [`evaluation-primitives.md`](evaluation-primitives.md).

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
- `Node Group`: a named frame over a set of nodes in a composition's layout. It organizes the
  canvas and has no evaluation meaning whatsoever (see **Node Groups**).
- `Node Group Encapsulation`: the deferred feature that hides a subgraph behind exposed ports.
  `Expose as Group Input` belongs to it. It is a separate, unimplemented contract: the two are
  named apart here precisely because one is presentation and the other would be topology.
- `Nested Composition`: a separate composition used as a source.

Avoid using the generic word `Group` when one of the distinct terms is intended.

## First Vertical Proof

The first document/runtime slice should prove:

1. Create at least two minimal source nodes, Layer Outputs, stable stack slots, and one composition
   output.
2. Derive timeline rows from the stack and reorder them by changing only stack order.
3. Synchronize primary and contextual selection across viewer, timeline, nodes, and Properties.
4. Edit the layer transform -- position, anchor, scale, rotation -- and opacity through Properties,
   Nodes, and viewer manipulation against the same parameter IDs.
5. Convert opacity explicitly from a literal to a driven value and undo to the exact prior value and
   topology.
6. Save and reopen stable IDs, order, parameter source, animation, and graph connectivity.
7. Compile one immutable snapshot and render the same result through the canonical evaluation path.

Full effects, masks, mattes, parenting, folders and groups, nested compositions, arbitrary
graph-to-layer conversion and multi-selection editor gestures remain deferred. Their contracts are
reserved here so the first proof does not create incompatible shortcuts.


## Node Layout

Compositions own `NodeLayoutRecord{position, width, collapsed, muted}` keyed by `NodeId`,
separately from the canonical graph; `NodeRecord` is unchanged. Position, positive finite width,
and collapsed state are durable presentation only. Mute is the explicit evaluation control in
this record and is interpreted during compilation; moving or collapsing a node never changes its
result. Missing records use the original four-column placement (origin 32, column pitch 256,
row pitch 180, width 128). Unknown-node layout entries are preserved with warning diagnostics,
not document errors. Zoom, framing, grouping, and transient gestures remain session state.

Node definitions and their socket schemas now belong to the Qt-free document module so graph
validation, commands, and compilation share one schema vocabulary without a document-to-runtime
dependency. The runtime header retains source-compatible aliases. Graph add/validate accept an
explicit registry for contributed schemas, defaulting to immutable built-ins; unknown schemas
remain preservable. Existing ports are Image. Known incompatible socket kinds are rejected with
`SocketKindMismatch`. Primitive lowering remains in runtime.


### Text And Mute Lowering

`bloom.text-source` is a lowered, evaluable node. Its schema is exactly three parameters in this
order -- content (String), size (Float64, pixels per em, default 72), color (Color4d, straight
`bloom.reference.linear-srgb`, default opaque white) -- and no font parameter, because the reference
path has exactly one embedded face (see
[`evaluation-primitives.md`](evaluation-primitives.md)'s "Text Rasterization Version 1"). None of the
three is animatable. The color parameter's node-local ROLE is the same string a solid color's is,
while its schema key is its own: a role names which binding of a node a parameter fills, the schema
key is the global identity of the value's meaning, and a text color means what a solid color means,
which is what lets one properties row, one node-card chip, and one session write path serve both.

`kTextSourceNodeSchemaVersion` stays `1` across this change. `AddTextLayer` had never succeeded, so no
project can contain a version-1 text record written against the earlier content-only shape, and a
version bump would only have invalidated records that cannot exist.

`AddTextLayer` builds the same canonical structured-layer topology `AddSolidLayer` builds, with a
text source in the source position. It refuses a non-finite or out-of-domain size, an invalid color,
and content that is not well-formed UTF-8; it accepts EMPTY content, because an artist adds a text
layer and then types into it. A reachable unmuted text source produces no diagnostic: the
`UnsupportedNode` diagnostic it used to raise is now raised only by node types that genuinely have no
compiled operation.

Mute lowers in the compiler: the first Image input passes to the first Image output, or an
unconnected/source image becomes transparent. A muted Layer Output is omitted from stack
participation; other graph consumers can still receive its bypassed input. A muted stack uses
only its first stable slot, and a muted composition endpoint passes through its input (empty
when disconnected). Unused branches and bypassed parameter sources are not evaluated. A muted text
source contributes nothing, exactly like a muted solid, and a muted Layer Output prunes its upstream
text source entirely.

## Node Groups

Compositions own `NodeGroupRecord{id, name, members, padding}` keyed by `NodeGroupId`, stored in the
composition LAYOUT beside `NodeLayout`. A group is presentation: it has no ports, no encapsulation,
no parameters and no evaluation meaning, and compilation never sees one. It is the Blender `Frame`
shape -- a named rectangle drawn around cards -- and deliberately not the encapsulation feature the
terminology reserves `Node Group Encapsulation` for.

A node is a member of at most one group; a member naming a node that does not exist is a warning
diagnostic, exactly as an unknown-node layout entry is, so an unreadable module never costs the
artist the document. Group names follow the same valid-nonempty-UTF-8 rule as layer names. Padding
is the finite, nonnegative inset between the member bounding rectangle and the frame's border, and
its `24` default is frozen in document units so a file frames its members identically on every
build. A group holds its own `NodeGroupId` namespace in the allocator, so a group id is never
reissued after an undo and layout identity still never advances the node watermark.

`GroupNodes(set, name = "Group")` creates one frame over existing nodes; its members leave whatever
group held them, because a node belongs to exactly one. `UngroupNodes(groupId)` removes the frame
and nothing else -- every member keeps its position, width, collapse, mute and wiring.
`RenameGroup(groupId, name)` changes the name in place; an identical name is a no-op.
`SetGroupMembers(groupId, set)` replaces a frame's membership wholesale. `MoveNodes` additionally
carries optional membership deltas, so a drag that ends inside or outside a frame publishes the move
and the membership change in ONE transaction and therefore one undo. Any group a command leaves with
no members is removed by that same command: a nameplate over nothing is not something an artist
asked to keep, and removing or dissolving a node is one of those commands. An empty group remains
representable and durable in the model -- no command produces one, and the format neither invents
nor drops one.

Duplication deliberately puts copies in no group. `DuplicateNodes` copies layout fields, not
membership; the copy lands ungrouped and the editor's own drop rule puts it in a frame if the artist
drops it in one.

### Group Frames In The Editor

A frame is painted BEHIND its members -- behind their links too -- as the bounding rectangle of the
member cards plus the record's padding, with a bordered hairline, a faint `SurfaceRaised` fill at
low opacity, `Radius::Panel`, and an inline-editable title strip along its top. The frame owns no
geometry of its own: it is recomputed from the live cards, including while a gesture is in flight.

While a member is being dragged out of a frame, that frame is computed from the members holding
still, so it does not chase the card leaving it -- otherwise "drop it outside to leave the group"
would name a rectangle the artist can never get out of. A gesture that picks up the frame itself
carries every member at once, excludes nothing from that computation, and changes no membership.

A card's drop is judged on its CENTER against the innermost frame under it, so one card has exactly
one landing; the resulting membership change travels in the same `MoveNodes` as the move. Clicking a
frame selects what it frames: `CompositionSession` owns one selection truth and it is made of
`NodeId`s, so a frame is never itself a document selection. The frame's whole body is its grip, the
Blender behavior this shape comes from, which does mean a box selection has to start outside every
frame.

`Ctrl+G` groups the selection and `Ctrl+Shift+G` ungroups every frame the selection sits in; the
canvas claims both through `ShortcutOverride` like every other key it owns. The selection's context
menu offers `Group`, and `Ungroup` when the selection is in one; a right-click on a frame offers
`Ungroup` and `Rename`. `Enter` keeps its one meaning in this editor -- rename the selected layer
node -- so a frame's title opens on double-click or from its own menu, and `Enter` commits from
inside the field. `docs/ux/interaction-model.md` remains the binding key list.

## Node Authoring Commands

`AddNode(typeId, layoutPosition)` creates one node at a finite position and independent parameter
records from the frozen registry's latest definition defaults. A source type stays graph-only: no
boundary, no slot, no edges. A **Layer Output** type also gets its layer IDENTITY here -- a `LayerId`,
a boundary record, and the name `Layer N` -- but still no stack slot. That split is deliberate: a card
on the canvas has to have a name to rename and a `LayerId` for Properties and the Timeline to address
the moment it exists, while the thing that makes a layer DRAW is its stack slot, and the Timeline
therefore lists a layer only once it has one. `AddSolidLayer` and `AddTextLayer` remain the structured
layer constructors the TIMELINE's Add menu uses, and both build the same topology.

The NODE CANVAS's own Add (Tab search, context menu) creates exactly the node asked for, through
`AddNode`, for every type including Solid and Text. An artist wiring a source into a Layer and a Layer
into Merge is making two decisions, and a canvas Add that made them for them was the substance of the
owner's second report.

`RemoveNodes(set<NodeId>)` validates the entire set, then removes those nodes, incident edges,
layout records, and parameters that no surviving node references. Orphaned owned animation curves
are removed too. Removing a Layer Output also removes its boundary and stable stack slot, without
removing shared upstream nodes. Stack and composition-output nodes cannot be removed.

`DuplicateNodes(set<NodeId>, offset)` allocates new node and parameter IDs, deeply copies owned
curves and keyframes, copies all layout fields with a finite offset, and copies only edges between
the selected nodes. Selected layer boundaries gain a new LayerId, `<name> copy`, and a new slot
immediately after the original, with its required boundary-to-slot edge. Results expose
`node.<oldId>`, `parameter.<oldId>`, `curve.<oldId>`, `layer.<oldId>`, and `slot.<oldId>` mappings.
Driver bindings have no copyable records in the current model, so duplication of driven parameters
is refused. The single canonical stack cannot be cloned together with its slot-addressed edges;
that topology is rejected by graph validation.

`RenameLayer(LayerId, name)` changes the boundary's valid, nonempty UTF-8 human-facing name while
preserving every graph and stack identity. An identical name is a no-op.

`ConnectPorts(OutputPortRef, InputPortRef, registry, insertBefore)` requires existing registered
sockets of connectable kind. It replaces the existing edge at that input, retaining its EdgeId, or
allocates one new edge. A link into an OPERAND socket is written as the parameter's driver binding
rather than as an edge. The entire proposed graph is validated before publication; same-time cycles
are refused with `GraphCycle`. A slot's content must still come from its matching Layer Output
boundary.

A `LayerStackInputRef` destination whose `slotId` is the INVALID sentinel means "a new slot here":
`ConnectPorts` allocates the slot, appends it, moves it before `insertBefore` when one is given, and
connects the Layer Output's image output to it -- one transaction, one undo. The source must be a Layer
Output boundary that does not already hold a slot. **Connecting a Layer to Merge is what creates its
stack slot**, which is why the editor needs no separate command for it and why the Merge card carries
its ordered multi-input even when the stack is empty.

`DisconnectInput(InputPortRef)` removes the edge at an existing input, or restores an operand's
registered default when the input is driven; an unconnected input is a no-op. On a **stack slot** it
removes the slot along with the edge into it: a slot with nothing in it is not a shape the canonical
graph admits, so the slot and the link into it are one thing to the artist and one thing here. The
Layer node keeps its boundary and `LayerId`, so reconnecting it is one gesture rather than a rebuild.

`DissolveNode(NodeId)` requires a registered first Image input/output pair and a connected input.
It removes the node and reconnects the input source to every consumer of the first Image output,
keeping those consumer edge IDs. A consumer that is a stack SLOT is not reconnected: the slot belongs
to the layer, and the layer goes with its boundary node -- so dissolving a participating Layer Output
takes that layer out of the stack, which is what the gesture means. Protected stack/output nodes are
still refused.

`MoveNodes(map<NodeId, Vec2d>)` validates every node and finite position before changing the layout
map. `SetNodeCollapsed`, `SetNodeMuted`, and `SetNodeWidth` change one layout field; width must be
positive and finite. Identical values and empty sets are no-ops. Only mute affects evaluation.

Each command runs through one ordinary transaction and one history entry. Refusals discard all
draft mutations and allocations. Undo and redo restore pinned record values, IDs, edges, parameter
sources, layout, and order rather than rerunning commands. Revisions always advance on publication;
allocator high-water marks never decrease. ProjectSession retains its conservative dirty contract:
undoing or redoing saved content is still a new dirty revision until a savepoint is accepted.

`CompositionSession` owns a NodeId selection set alongside the unchanged tagged primary and layer
context. `selectNodes(set, primary)` requires an existing primary in the set; toggling in a node
makes it primary, and toggling out the primary chooses the lowest remaining NodeId. Single-node
selection replaces the set; layer selection keeps its LayerId primary and selects its boundary.
Parameter/keyframe selection keeps its existing tagged semantics and clears the node set.
Publication prunes missing nodes, clear and rebind clear the set, and selection never creates a
document revision or history entry. Editor gestures and command wiring remain a separate phase.

## Node Editor Interaction

The editor projects `NodeLayoutRecord` position, width, collapse and mute state. Missing records
use `defaultNodeLayout`, including the 128px width. Each registered input/output is a graphics item;
stable stack-slot inputs have separate items too. Expanded cards place each port on its own row,
inputs left and outputs right. Collapsed cards keep their sockets on the header edges. The selected
set uses Accent outlines and its primary uses Foreground. The set and primary flow through
`CompositionSession::selectNodes`, `toggleNodeSelection` and `clearSelection`; Properties and
Timeline retain their existing primary/context contracts.

### Artist-Facing Node Names

Type ids are unchanged. The names the node editor shows are not spelled out of them:

| Type id | Name shown |
| --- | --- |
| `bloom.solid-source` | `Solid` |
| `bloom.layer-output` | the layer's own name, with a small `Layer` eyebrow above it |
| `bloom.layer-stack` | `Merge` |
| `bloom.composition-output` | `Output` |

Anything else falls back to the spelled-out identifier. A parameter ROLE still spells out the same
way it always has -- that fallback is what `node_editor::displayTypeName()` is for, and
`nodeTypeDisplayName()` is the node-type layer above it.

A layer boundary card is named after its layer, so the card alone would no longer say what kind of
node it is; the eyebrow is the one line that still says so -- and it names the layer's BLEND MODE
alongside it whenever that mode is not Normal, because a blend mode is otherwise the one layer
property with no visible trace on a card whose rows are collapsed.

### Merge's Ordered Multi-Input

The Merge node renders ONE socket for the whole layer stack -- a vertical pill, divided into one
segment per ordered slot, topmost first -- instead of one repeated `content` row per layer. Its length
grows by one pitch per slot, so the port itself shows how deep the stack is.

The slot model underneath is exactly as it was. The socket carries the stack's own slot references in
the stack's own order, and an edge finds the socket that terminates it by asking whether the socket
accepts that reference, so every slot edge is still projected as its own wire. No slot, edge, or
ordering record changes shape.

While a link drag is in flight over the pill, a caret marks which position in the order the pointer is
at -- and the pill now ACCEPTS the drop, because a drop there is what creates the slot. A Layer
output released on the pill lands a new slot at the caret's position (upper half of a slot means above
it, lower half below it, past the last one appends). A press ON the pill picks up the link of the slot
under the pointer, so a slot's content can be detached, transferred, or re-dropped at another position
in the order; a press where there is no slot starts from the "new slot" sentinel instead.

### Node Categories

`NodeDefinition::category` declares which Add-surface section a node type is listed under:
`Sources`, `Layers`, `Compositing`, `Values`, `Output`, `Utilities`. The vocabulary is the artist's --
what a node is for -- so it is declared beside the type rather than derived from `NodeLoweringKind`,
which spans several sections at once. The built-ins are Solid and Text under `Sources`, the layer
boundary under `Layers`, Merge under `Compositing`, Output under `Output`, the literal value sources
and `Time` under `Values`, and the whole computing library under `Utilities` (see **Value Graph And
Drivers**).

Add surfaces list entries in that category order and alphabetically inside each one, and
`KSearchPopup` emits a heading whenever the section changes. A section with no matching result has no
heading, and the list is exactly as tall as the rows and headings it holds.

The canvas's own context menu offers those categories as a **cascading `Add Node` submenu**, one
section per category in the same order, entries alphabetical inside each section, and every entry adds
its node at the click position through the same path the search uses. A type the command layer would
refuse -- a singleton already in the composition -- is listed DISABLED with the command's own refusal
in its tooltip, read from a dry run of the add operation rather than from a second copy of the rule.
The search popup belongs to Tab alone: right-clicking to add a node the artist can already name should
not make them type it. Menu rows, the submenu caret and `Size::MenuMinWidth` come from the
application-wide proxy style (`kit/mnemonic_style.hpp`), so these are ordinary `QMenu`s.

A typed query reorders that list by RELEVANCE OF THE NAME, never of the keywords. An entry is still
found by the socket kinds it carries -- that is what the keywords are for -- but a keyword match can
never outrank a name match: sections are ordered by their own best name score and entries inside a
section by theirs, so typing `Scalar` and pressing Enter adds the node CALLED Scalar rather than the
first node in category order that happens to carry a Scalar socket. With no query typed, every score
is equal and the category order above is exactly what the artist reads.

### Value Graph And Drivers

A composition holds two graphs in one node set. The **image chain** produces pixels and is addressed
by `OperationIndex`; the **value graph** produces one number per frame and is addressed by
`ValueOutputIndex`. They share the reachable node set and the topological order, and nothing else --
no `CompiledOperation` alternative was added, and the value graph is compiled and evaluated in its
own pass BEFORE any image operation reads a parameter.

Reachability follows drivers as well as edges: a node whose parameter is driven depends on the value
node that drives it. Those dependencies feed the SAME indegree map the edge set builds, so a cycle
through a driver is refused by the one existing acyclic check rather than by a second rule that could
disagree with it.

A driver binding is DRAWN, exactly as an edge is. The canvas's link list is the graph's edges followed
by one synthesized link per driver binding, rendered as the same item so hover emphasis, selection
emphasis, the cut gesture and the pick-up gesture all reach a driver without a second code path. Such
a link carries no `EdgeId` -- there is no edge to carry one -- so it is addressed by its DESTINATION,
which is what `DisconnectInput` already takes. The pick-up gesture asks an input one question, "where
does your value come from", and an edge and a driver binding are the two answers; dropping a picked-up
driver on empty canvas restores the operand's registered default, in one undoable step.

Pointer slop around a socket is an ARTIST's slop, not a scene measurement: the socket's own hit shape
is fixed in scene units, so the canvas widens the grab radius by the view's inverse scale before
resolving a press. A socket is therefore the same size under the pointer at every zoom, and never
smaller than its painted hit shape. A hosted field keeps its own clicks -- the widened radius is tried
only where no field is under the pointer.

While a link drag is in flight, a socket that cannot take it carries the reason in its own tooltip --
which way round the link would have to go, or which two kinds do not meet -- and drops that line again
when the drag ends.

#### Socket Kinds And Promotion

`SocketValueKind` is `Image`, `Color`, `Scalar`, `Vector2`, `Vector3`, `Integer`, `Boolean`, `String`.
A connection is accepted when the kinds are equal, or when the source appears in this whitelist:

| From | To | Meaning |
| --- | --- | --- |
| `Integer` | `Scalar` | Exact widen; every stored integer below 2^53 is representable |
| `Boolean` | `Integer` | `false` is 0 and `true` is 1, the mapping every stored boolean has |
| `Boolean` | `Scalar` | The same mapping, widened once more |
| `Scalar` | `Vector2` | Splat: the one value in every component |
| `Scalar` | `Vector3` | Splat |

Nothing promotes into `Boolean`, `String` or `Image`, and no vector changes width: each of those would
have to invent information. Every promotion compiles to its own operation rather than being folded
into whoever reads the value, so a widening is visible in a plan dump and diagnosable like any other
step. One predicate -- `document::isAcceptedSocketConnection()` -- answers for the editor's drag
affinity, `ConnectPorts`, document validation and the compiler's edge check alike.

#### Node Library

Every type below ships inside Bloom and is therefore a foundation node type: no manifest requirement
can claim to provide one. None is animatable -- a value graph already lets an artist shape a number
upstream of an operand -- and three of the kinds have no curve model at all.

| Category | Node | Shape |
| --- | --- | --- |
| Values | Integer, Scalar, Vector 2, Vector 3, String, Color, Boolean | One authored value, one output. No input: a literal is where a value comes FROM, and a node whose value arrives from elsewhere is a Reroute |
| Values | Time | No inputs and no parameters. Two outputs, `seconds` (Scalar) and `frame` (Integer), both filled from the same request time the plan's curves are sampled at |
| Utilities | Math | Five operand sockets, one definition, the frozen 24-operation `ScalarPrimitive` vocabulary verbatim, plus a clamp toggle. Only the operands the live operation reads are lowered |
| Utilities | Vector 2/3 Math | Componentwise Add/Subtract/Multiply/Divide, Scale, Normalize, and Cross at three components. Componentwise operations route through the scalar kernel per component |
| Utilities | Vector 2/3 Measure | Length, Dot Product, Distance. A separate type from Vector Math because the result is a Scalar, and a socket's kind is fixed by its definition |
| Utilities | Map Range | Linear, Smoothstep, Smootherstep, with an optional clamp to the destination range |
| Utilities | Clamp, Mix | Direct reuse of the scalar `clamp` and `mix` kernels |
| Utilities | Mix Color | Four independent straight-value channel interpolations. Not vector math over four floats: a Color is not a Vec4, and no gamut or OCIO step is implied |
| Utilities | Compare | Six predicates over two Scalars; Equal and NotEqual spend the node's epsilon, the four orderings do not |
| Utilities | Switch | One definition per socket kind. Bloom has no polymorphic port, and a socket that retyped itself would be a socket that does not know what it carries |
| Utilities | Separate/Combine XY, XYZ, RGBA | Pure de/interleave. Channel extraction is the only way to read a colour's channels as numbers, by the no-implicit-Color-to-Vec4 rule |
| Utilities | Random | Deterministic hash of its seed into `[min, max)`. No entropy source: a cached or exported frame has to agree with the frame that produced it, so a seeded value changes over time only when something wires a changing number into the seed |
| Utilities | Reroute | One definition per kind, Image included. Pure pass-through; an Image Reroute is elided during image lowering and costs nothing at evaluation |

#### Fallbacks

The `bloom_core` kernels report domain failures and never substitute a value, because only the caller
knows what the number is for. At the node boundary that caller exists, so a failure substitutes the
node's documented fallback and records a scoped diagnostic -- the frame still renders, degraded and
explained, the same way a missing module or a mute bypass already behaves.

| Failure | Fallback |
| --- | --- |
| Divide or reciprocal by zero, and every other scalar domain failure | `0` |
| Normalize of a zero-length vector | The zero vector; it has no direction, and an arbitrary axis would look like a real one |
| Degenerate Map Range source range | The destination minimum -- what every value in that range would map to if it had width |
| Reversed Clamp bounds | The value UNCLAMPED; passing it through is visibly wrong, where silently swapping the bounds would look correct |
| Frame number not representable at the request time | `0`, reported |

#### Animatable Value Literals

A literal **Scalar**, **Vector 2** and **Colour** node's own authored value is ANIMATABLE: their schema
keys join `isScalarAnimatableSchemaKey`, `isVec2AnimatableSchemaKey` and `isColor4AnimatableSchemaKey`
respectively, so the existing keyframe commands, the existing curve kinds, the existing exact rational
sampler and the existing keyframe diamond all serve them without a second path. A value literal on a
curve lowers to a curve index in `CompiledValueOperand`, and the value graph samples it at the frame
being rendered -- so a Scalar node keyed 0 to 1 over ten frames, driving a layer's opacity, produces a
composited alpha of `frame / 10` at every one of them.

Editing an animated literal's number on its card writes a KEY at the session time rather than
replacing the curve with a constant, by the same rule every layer row already follows; its row shows
the sampled value at the session time.

Four literal kinds stay constant-or-driven, and deliberately: **Vector 3**, **Integer**, **Boolean**
and **String** each need a curve KIND the document does not have (a Vec3 curve, or a Hold-only integer
or boolean curve), which is a document-format change with its own schema ladder step rather than a
widening of the animatable set. Every generic OPERAND schema also stays constant-or-driven: an operand
is a value a node reads, and the artist already shapes it with a curve upstream -- wire an animated
Scalar node into the socket -- so a curve of its own would be a second authoring path to one picture.

The keyframe surface for a value literal is its NODE CARD's diamond. The Timeline's rows are layers
and the Properties panel's rows are a layer's roles, so neither has a place to hang a value node's
lane today; giving them one is a timeline-model change rather than an animation one.

#### Evaluable Parameter Kinds

`CompiledScalarParameter`, `CompiledVec2Parameter` and `CompiledColorParameter` each gained one
alternative for a value-graph output, and `CompiledValueOperand` gained three for the curve tables. A
new alternative appearing is not a plan-semantics change, so neither the plan nor the evaluator
semantics version moved and no cached frame digest shifted.

The remaining kinds -- a Layer Output's `Integer` blend mode, a Text source's `String` content -- are
linkable in the editor and durable in the document, but nothing yet carries their value into a
compiled operation, so a driver on one is reported through the existing `UnsupportedParameterSource`
diagnostic rather than silently ignored.

#### The Output Node Is A Sink

`bloom.composition-output` declares ONE input and NO output. Nothing connects from the end of the
composition: the compiler's reachability walks backwards from this node, so an edge leaving it was
never followed, and a socket that leads nowhere is an invitation to draw a wire that means nothing.
The rule is stated once, in `CanonicalGraph::addEdge()` and `validate()`, as "a node whose registered
definition declares no output is a sink" -- not as a special case for this type, and not as "this
particular port is undeclared", which is still the compiler's own `UnknownPort` diagnostic about a
different mistake. A document written before this that carries such an edge decodes with the edge
dropped rather than refused; the graph's `compositionOutput` endpoint keeps its port spelling, which
is how the document has always named the endpoint rather than a socket.

Nesting one composition inside another belongs to a separate **Composition source** node -- a node
that READS another composition's output -- not to an output port on this one.

### Node Cardinality

`NodeDefinition::cardinality` declares how many instances of a node type one composition may hold.
`Many` is the default. `OnePerComposition` marks a structural singleton, and exactly two built-in
types carry it: the `Layer Stack` operator and the `Composition Output` endpoint. A second one is not
a graph a composition can mean.

`AddNode` enforces it and refuses with the type it refused, so every Add surface -- keyboard, menu,
search -- inherits one rule instead of keeping its own copy. The Add search reads the refusal back
from a dry run of the same operation and lists the type as a disabled result carrying that message;
it does not reimplement the check. Cardinality constrains authoring only: it is not a validation rule,
and a document that already holds more than one is neither rejected nor repaired by it.

### Application Integration Limit

The interaction implementation accepts a `NodeGraphicsScene::Submit` transaction adapter. The
current application editor factory constructs `NodeGraphEditor` with only `CompositionSession`,
whose command execution and result publication remain private. No production adapter is installed.
Consequently **node command authoring is not enabled in the application yet**. Persisted projection,
sockets, session click/Shift/box selection, Ctrl+A, Home/Ctrl+0/Ctrl+1, zoom/pan, existing in-node parameter
edits and legacy Add Solid work. Command gestures show no draggable cursor without an adapter;
command-only node menus and cursor-positioned Add search are not offered. The legacy Add menu
keeps Solid and Text creation, both through their own session paths. Keyboard authoring requests
report unavailability through the existing session status signal.

Enabling the following command interactions requires one public session submission method which
executes a transaction, passes its result through the existing snapshot/history/refusal publication,
and returns created IDs to the editor for selection. No additional selection API is needed. This
is an integration gap, not permission for the UI to own a document or another command history.
Offscreen interaction tests supply an explicit adapter to the real command stack and publish the
snapshot through public session rebinding; they do not establish application wiring readiness.

### Transaction Adapter Contracts

With that adapter supplied, the following behavior is implemented and covered by event tests:

- A card drag previews every selected card and publishes one `MoveNodes` on release. Its right edge
  previews a width change and publishes `SetNodeWidth` on release, with a 128px gesture minimum.
  Persisted positive widths are read exactly. Preview movement does not issue per-pixel commands.
  Escape, focus loss, or a replacement snapshot cancels the transient gesture. Canvas manipulation
  retains the current view instead of reframing it during command publication.
- Socket dragging draws a live cubic Bezier. Opposite Image sockets connect with `ConnectPorts`;
  an occupied input rewires while retaining its edge ID. An incompatible socket is Error red and
  release changes nothing. Graph-cycle and other command refusals use `commandRejected`, the
  application's existing transient status path, without dialogs.
- Picking up an occupied ordinary input hides its old link during the preview and carries its
  source. Dropping on another input transfers the link in one transaction. Dropping on empty
  canvas executes `DisconnectInput`; Escape restores the preview without changing the document.
  Other socket drags released on empty canvas open search with that socket armed.
- Links have a widened hit shape, hover emphasis and brighter ink when either endpoint is selected.
  Ctrl+right-drag cuts every crossed ordinary wire with several `DisconnectInput` operations in
  one transaction. A moved single node with an unconnected first Image input and a first Image
  output highlights a wire under the cursor and inserts with two `ConnectPorts` operations in the
  same transaction as the move. Any command refusal rolls the entire transaction back.
- Tab and canvas Add… open `KSearchPopup` at the cursor. Case-insensitive word filtering matches
  readable node type names and socket-kind names. Enter, arrows, pointer selection and Escape use
  the kit popup machinery. Every built-in registry kind is listed, each carrying its own command's
  real refusal when it has one. Solid uses `AddSolidLayer` and Text uses `AddTextLayer`, both building
  a structured layer; other kinds use `AddNode`.
  Creation, cursor layout and the first compatible armed-port connection share one transaction.
  The editor's compound operation delegates all durable writes to existing commands and only reads
  the returned IDs to resolve subsequent operations.
- Delete/Backspace remove the selection; Ctrl+D duplicates with a 24px offset, selects returned IDs,
  and starts floating placement. Escape cancels placement while keeping the undoable copies at their
  initial offset. Enter, and a double-click on the card, rename a layer node. Dissolve, mute and
  collapse are context-menu commands and bind no key; mute and collapse still toggle in one
  transaction for the entire selection (a mixed selection becomes uniformly enabled for that state),
  and empty or protected targets report refusal through the same shared guard. Ctrl+0 fits, Home is
  its alias, Ctrl+1 is 100%, and Ctrl+A selects all. `ShortcutOverride` claims only canvas commands;
  focused proxy field widgets retain text editing, Tab included.
  `docs/ux/interaction-model.md` is the binding list for this editor and every other.
- Canvas menus offer Add…, Fit, 100%, Zoom In/Out and Select All. Node menus offer only accepted
  Duplicate, Dissolve, Mute/Unmute, Collapse/Expand, Rename and Delete operations. Rename appears
  only for a single participating Layer Output and edits its header through `RenameLayer`. Each of
  those menu items calls the editor's own named command method, never a synthesized key press, so a
  command can exist in the menu without owning a key.
  `canApplyNodeOperation` is needed because commands have no dry-run surface: it checks the actual
  operation and final validation on an isolated draft, without publishing or advancing live IDs,
  revisions or history. Menus do not maintain a second set of command eligibility rules.
- Muted bodies and their proxy controls use 50% opacity; the header retains normal opacity and the
  existing vendored Phosphor eye-slash badge. Collapsed cards are header-only pills. A driven
  parameter role hides its corresponding widget and keeps its socket; every control a card builds
  registers the role it edits, so the rule is applied by role rather than by a per-control list.
  Linking an Image transport input never hides a value control, because an Image port backs no
  parameter.

### Detachable Links

No link is structural. Stack-slot content edges and participating Layer Output boundary outputs used
to be: they were projected with explanatory tooltips and could not start or receive a drag, be cut, or
be auto-insertion targets, because nothing could repair the topology a detach would break. Creating
and removing a slot by connecting and disconnecting it is what removes that asymmetry, so every link
now answers to the same three gestures:

* drag an input's link end off and drop it on empty canvas -- `DisconnectInput` on that destination;
* Ctrl+right-drag a cut stroke across it -- the same command, once per crossed link;
* right-click it -- a menu offering **Disconnect** and **Delete Link**, which are one command under the
  two names an artist might look for it by.

Which durable record a destination is addressed through -- an edge, a driver binding, or a stack slot
-- is `DisconnectInput`'s business, not the gesture's, which is why one gesture serves every kind.
Every link also carries a tooltip naming both of its ends. Removing a layer outright still uses
`RemoveNodes`, which removes its boundary and slot together. Duplicating a node whose parameter is
driven still refuses: a copy would need a second driver nothing asked for. Duplication of incompatible
canonical-stack topology still refuses through command validation.

These are Qt scene/widget interactions without platform-specific input code. The same implementation
and offscreen event tests apply to Linux, macOS and Windows; this change's executed gates are Linux.
