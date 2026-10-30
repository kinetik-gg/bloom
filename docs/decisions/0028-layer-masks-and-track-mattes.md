# ADR 0028: Layer Masks, Mask Values And Track Mattes

Status: accepted (lane decision under issue #360; the supervisor may renumber at integration)

Date: 2026-10-23

## Context

Bloom has no layer mask, track matte or Mask value type:

- `LayerOutputBoundary` and `CompiledLayerOutput` carry no mask or matte fields.
- The Viewer's shape and pen tools always create a new shape layer.
- `PathValue` exists (`ParameterValueKind::Path`) but only as a compile-time constant; there is no
  Path animation curve, so a path cannot be keyed.
- Merge slot roles are only `content` and `audio`; the `SetMatte` command named in
  [`layer-graph-model.md`](../architecture/layer-graph-model.md) does not exist.
- `SocketValueKind` has no Mask, although
  [`evaluation-primitives.md`](../architecture/evaluation-primitives.md) states that a Mask is not a
  grayscale color image.

The pieces a matte needs already exist. Set Alpha and Multiply Alpha (`ImageCombineKernel`, fixed by
#361) read a second image as coverage through an Alpha/Red/Green/Blue/Luma selector with an invert
bit, with GPU parity on every required route. Separable blur and other neighborhood effects run on
the GPU as generated neighborhood programs.

## Decision

### 1. Mask values

`SocketValueKind::Mask` is a new transport kind. A Mask is single-channel coverage in `[0, 1]`
defined over composition pixel space. It connects only to Mask inputs; there is no implicit
conversion to or from Image in either direction.

At runtime a Mask is carried in the existing image transport as premultiplied white: every channel
holds the coverage. That representation is private to the lowering, and the type rules keep it from
reaching an Image input.

- The new **Mask** node (`bloom.mask`) produces a Mask from one path with the same feather, expansion,
  opacity and invert controls as a layer mask (section 2). Its output covers the composition canvas.
- **Image Mix** gains an optional `mask` input. Its per-pixel factor is `factor * mask`. With the
  input unconnected the node behaves exactly as before.

### 2. Ordered per-layer masks

`LayerOutputBoundary::masks` is an ordered list of `LayerMask` records:

```text
LayerMask = { id: LayerMaskId, name, mode: None|Add|Subtract|Intersect|Difference, inverted,
              path, feather, expansion, opacity }
```

`path`, `feather`, `expansion` and `opacity` are `ParameterId`s in the composition parameter store,
so they animate through the ordinary parameter and keyframe machinery. `mode` and `inverted` are
plain authoring fields, closed and not animatable. Their schema keys are `bloom.mask.path`,
`bloom.mask.feather` (Float64 pixels, `[0, 1000]`), `bloom.mask.expansion` (Float64 pixels,
`[-1000, 1000]`) and `bloom.mask.opacity` (unit Float64). The mode's stored integers are durable and
are never reordered.

The mask path is in the layer's content space: the pixel space of the image that feeds the Layer
Output, before the layer transform. A mask therefore moves, scales and rotates with its layer, and
feather and expansion are measured in content pixels.

Coverage of one mask at pixel center `p`:

```text
s = signed distance from p to the path's flattened outline (negative inside; nonzero winding)
d = s - expansion
c = clamp(0.5 - d / max(feather, 1), 0, 1)        (feather 0 is a one-pixel antialiased edge)
c = inverted ? 1 - c : c
c = c * opacity
```

An open path, or one with fewer than two anchors, covers nothing. Masks accumulate in list order.
The starting coverage is 0 when the first mask whose mode is not None is Add or Difference, and 1
otherwise:

| Mode | Result |
| --- | --- |
| Add | `a + c - a*c` |
| Subtract | `a * (1 - c)` |
| Intersect | `a * c` |
| Difference | `a + c - 2*a*c` |
| None | `a` (the mask is skipped) |

A layer with no masks, or only None masks, is unmasked. Otherwise its content image is multiplied,
in premultiplied space, by the accumulated coverage before the layer transform. The data window is
unchanged.

### 3. Animatable paths

`PathAnimationCurve` is a fifth curve kind holding whole-value `PathKeyframe`s. Each key has a
`PathValue`, the ordinary outgoing interpolation (Hold, Linear, EaseInOut) and the ordinary ease
handles. A mask path (a layer mask's or the Mask node's, schema `bloom.mask.path`) may be bound to
one; a shape layer's path stays constant.

Interpolation is per anchor. Points and handles are interpolated by the eased segment factor, and a
missing handle counts as its anchor point. Two keys whose anchor counts or `closed` flags differ
hold the earlier key until the later key's time. The evaluator samples a Path curve exactly as it
samples a scalar curve: rational segment factor, the same ease rule, and no allocation beyond the
sampled value.

### 4. Track mattes

`LayerOutputBoundary::trackMatte` is an optional typed relation:

```text
TrackMatte = { source: LayerId, channel: Alpha|Luma, inverted }
```

- **Validation.** The source must be a different layer in the same Merge as the target. The
  relation graph over all layers must be acyclic. A layer may be the source for several targets and
  may itself have a matte.
- **Hiding.** A layer that is the matte source of any layer in its Merge contributes nothing to that
  Merge's composite. It is still evaluated for its targets, whatever its enabled and solo state.
- **Space.** The matte image is the source layer's output after its own masks, effects, transform,
  opacity and matte. It is applied to the target's output after the target's transform, so both
  meet in composition pixel space.
- **Time.** The source is evaluated on its own layer clock (ADR 0024) at the request time. Outside
  its active range or content extent the matte is absent, and an absent matte leaves the target
  fully transparent.
- **Math.** Matte apply is Multiply Alpha: the target's premultiplied RGBA is multiplied by the
  selected coverage (alpha, or Luma of the premultiplied RGB), inverted when asked.
- **Evaluation.** The target's opacity and blend mode then apply in the Merge exactly as before.

### 5. Compilation, GPU and identity

- Layer masks lower to a new `LayerMaskKernel` image effect on the Layer Output's input. It carries
  the ordered compiled masks with sampled paths and scalars.
- The Mask node lowers to an opaque white Solid over the canvas followed by the same kernel.
- Image Mix's mask is an optional `maskInput` on its `CompiledImageEffect`.
- A track matte lowers to an `ImageCombineKernel` Multiply Alpha effect between the target's Layer
  Output and its Merge entry, with the source's Layer Output as the secondary input. The matte
  source's own Merge entry is omitted.
- Plan semantics stays 9. The new kernel alternative, the effect's `maskInput` and `trackMatte`
  fields, and a Merge entry naming a track-matte effect are additive: no existing plan value changes
  meaning, and an older evaluator refuses the new shapes rather than misreading them.

The CPU evaluator is the reference. The GPU runs the mask kernel as one fixed neighborhood
program through the same production preparation path as the separable blur. The host flattens
each outline once and packs the segments, their reciprocal factors and the per-mask controls into a
read-only storage buffer that every dispatch uploads; the shader text never changes. The data bytes
are part of the prepare key and the command identity, so every sampled path is its own cached
result, while the compiled program and the native pipeline are shared: an animated path changes
data, never the program, and playback never stalls on a compile. The buffer is padded to a
power-of-two capacity so a path whose flattened segment count drifts while animated keeps one
pipeline. Both routes evaluate the same Float32 operations in the same order with no division and
a multiply-only square root, so mask coverage is bit-identical between the CPU reference and the
GPU. Outlines are capped at 8192 segments for the GPU program; beyond that GPU preparation fails
closed and the explicit CPU fallback renders the frame. The Image Mix mask is a fifth binding (binding 4) of the pinned `image_combine` shader.

The coverage contract adds these Required axes, with CPU parity on every required preview and final
route and no exemption:

- `feature.mask.layer`: rasterization, every mode, feather, expansion, invert and opacity.
- `feature.mask.value`: the Mask node and Image Mix's mask.
- `feature.matte.track`: Alpha, Luma, inverted, and an absent source.

Sampled mask paths and scalars, the matte selection and the source layer's mapped time participate
in operation identity through their operands, as every other parameter does.

### 6. Reachability

A layer that is a matte source is consumed by its targets even when its slot is hidden, disabled or
solo-excluded, so `document::RenderReachability` follows the relation from every reachable target.
`confineNodeChange` never gates a matte source's layer boundary: a change there reaches output
through the target's span, so the walk continues ungated and fails closed to whole-span. A target
keeps its ordinary gate, since its matte only removes coverage within its own span.

### 7. Document schema and commands

Document schema 1.24 is one additive, version-only step:

- empty `masks` and absent `trackMatte` on every layer boundary;
- the Path curve kind and the `bloom.mask` node;
- Image Mix's optional `mask` input. It is an input port, not a parameter binding, so the node keeps
  schema 1 and every stored Image Mix record is unchanged.

Existing documents decode and render bit-identically.

Typed commands, each with exact undo:

- `AddLayerMask`, `RemoveLayerMask`, `MoveLayerMask`;
- `SetLayerMaskMode`, `SetLayerMaskInverted`, `RenameLayerMask`;
- `SetLayerMaskPath`, which sets the constant, or the key at the playhead when the path is keyed;
- `SetTrackMatte`.

Feather, expansion and opacity use the ordinary parameter and keyframe commands. Every mask and
matte command declares its composition `renderScope()`.

### 8. Authoring

- **Viewer.** With a layer selected, the Rectangle, Ellipse and Pen tools add a mask to that layer
  instead of creating a shape layer; with no layer selected they behave as before. The selected
  mask draws its outline and anchors in layer space; dragging an anchor or handle renders live in
  the Viewer from a session-only parameter override of the path (`beginMaskPathEdit`, accepted by
  the snapshot compiler for a reachable layer's mask path, feather, expansion and opacity) and
  commits one `SetLayerMaskPath` on release. The document never changes mid-drag. Dashed feather bounds are drawn at `expansion +/- feather / 2`.
- **Anchor editing.** A confirmed path -- a mask or a Pen shape path, through one shared
  implementation (`ui::path_editing`) -- is edited after the fact: move anchors, drag handles, add a
  point on a segment (a De Casteljau split that keeps the shape), delete points (a mask keeps
  three, a shape path two), multi-select, and switch each anchor's handle mode. The mode is a
  document field on every path anchor (`PathAnchorMode`: Corner, Mirrored, Aligned, Free, Auto),
  persisted in 1.24, carried by Path keys and by scripting path values (`mode`). It is editing
  intent only: the curve is the stored points and handles. Older paths infer it from their
  handles. Auto handles are Catmull-Rom from the neighboring anchors and are recomputed by any
  edit that moves a neighbor. `docs/ux/ui-grammar.md` owns the gestures.
- **Scripting.** Python, the CLI and MCP add and edit masks through `bloom.layer.add-mask`,
  `set-mask-path`, `animate-mask-path`, `stop-animating-mask-path`, `remove-mask`, `move-mask`,
  `set-mask-mode`, `set-mask-inverted` and `rename-mask`, and set mattes through
  `set-track-matte`/`clear-track-matte`. A path argument is the scripting `path` value kind (ADR
  0022) in the layer's content space.
- **Timeline.** A layer row discloses a Masks group with one row per mask (name, mode, inverted) and
  key rows for its path, feather, expansion and opacity. A Track Matte column selects the source
  layer, the channel and the invert bit.
- **Properties.** A Masks section lists each mask with closed-enum Mode, an Inverted check, Feather,
  Expansion and Opacity. A Track Matte section mirrors the Timeline column.

## Consequences

- Masking and mattes are typed document truth, edited by commands and rendered identically by
  preview and final routes on every platform. Linux qualifies the GPU path locally; macOS and
  Windows use the existing supported CPU fallback until their GPU qualification.
- Matte apply reuses the qualified Multiply Alpha path, so only the mask kernel and the Image Mix
  mask binding are new GPU work.
- A Path curve is whole-value, unlike the component curves of vectors and colors. Key summaries and
  the graph view treat a path key as a key without a scalar value.
- Documents without masks or mattes compile to the same operations as
  before.

Primary references: [`layer-graph-model.md`](../architecture/layer-graph-model.md),
[`evaluation-primitives.md`](../architecture/evaluation-primitives.md),
[`animation-and-time.md`](../architecture/animation-and-time.md) and
[`interaction-model.md`](../ux/interaction-model.md).
