# Evaluation Values And Primitive Semantics

Status: working

Updated: 2026-08-25

## Purpose And Scope

Bloom separates artist-facing node definitions from backend-neutral execution primitives. A node
defines stable authoring schema and user intent; its lowering recipe composes checked primitives
into an immutable evaluation plan.

This document owns internal numeric, value, color, alpha, lowering, and conformance semantics. It
does not own node presentation, external interchange policy, or display UI. Those remain with the
layer/graph, standards, and Viewer contracts.

```text
Artist node or structured layer property
        |
        v
Versioned NodeDefinition + parameter/port schemas
        |
        v
Snapshot compiler and lowering recipe
        |
        v
Typed immutable runtime operations
        |
        +---------------------+
        v                     v
Checked domain primitives   Scheduling metadata
        |                   ROI / halo / time / cost
        +----------+----------+
                   v
          CPU reference / GPU provider
```

One checked primitive should power layer controls, graph nodes, scripting, CPU evaluation, and GPU
evaluation. Those surfaces must not develop subtly different versions of the same operation.

## Value Layers

Bloom keeps these layers distinct:

1. Authoring values are durable parameter values such as Float64, `Vec2d`, and straight `Color4d`.
2. Compiled values are validated immutable values bound to exact operations and precision.
3. Canonical process pixels are premultiplied Float32 RGBA in an image with explicit color identity.
4. Prepared display pixels are presentation results and never replace process pixels or authoring
   truth.

Conversions between layers are explicit, checked, and covered by their own semantics. The current
`Image` socket is only the first transport kind; semantic role constraints must exist before masks,
depth, normals, motion, UV, ID, or arbitrary data images can use it safely.

## Ownership

- `src/core` owns allocation-free scalar and small-value math that does not depend on images, Qt,
  project state, ambient color configuration, or a render device.
- `src/render` owns image descriptors, sampling, compositing, filtering, channel, alpha, tile, and
  color-aware image kernels.
- `src/runtime` owns frozen node definitions, snapshot lowering, typed plans, scheduling contracts,
  diagnostics, and backend-neutral evaluator orchestration.
- Optional modules own coherent geometry, 3D, material, audio, or pipeline primitives. A library
  boundary is created only when a concrete implementation requires it.
- `src/ui` presents schemas and results; it never owns primitive semantics.

## Type Binding

Storage resemblance does not imply semantic interchangeability:

- `Color` is not `Vec4`.
- `Mask` is not a grayscale color image.
- Depth, normals, motion vectors, UVs, object IDs, and arbitrary data channels are not color.
- A uniform value, per-element field, image grid, and variable-length list are different shapes even
  when their element type matches.
- Point, direction, normal, and generic vector values have different transform behavior.

There are no implicit Bool-to-number, Vec-to-Color, Color-to-luminance, straight-to-premultiplied,
color-to-data, domain, unit, or coordinate-space conversions. A node definition requests the exact
kind or inserts an explicit conversion operation.

Durable scalar authoring values bind to Float64 until a checked conversion at an image/kernel
boundary. Canonical process pixels bind to Float32. Node registry parameter matching remains exact;
type erasure and per-pixel string lookup are forbidden inside the hot evaluator.

## Scalar Primitive Vocabulary Version 1

`bloom_core` currently defines checked Float32 and Float64 primitives with these stable IDs and
operand orders:

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.add` | Add | `left`, `right` | finite sum |
| `bloom.core.scalar.subtract` | Subtract | `left`, `right` | finite difference |
| `bloom.core.scalar.multiply` | Multiply | `left`, `right` | finite product |
| `bloom.core.scalar.divide` | Divide | `numerator`, `denominator` | both signed zeros are invalid denominators |
| `bloom.core.scalar.multiply-add` | Multiply Add | `multiplicand`, `multiplier`, `addend` | one deliberate fused operation |
| `bloom.core.scalar.minimum` | Minimum | `left`, `right` | `+0/-0` tie produces `-0` |
| `bloom.core.scalar.maximum` | Maximum | `left`, `right` | `+0/-0` tie produces `+0` |
| `bloom.core.scalar.clamp` | Clamp | `value`, `minimum`, `maximum` | closed interval; equal bounds valid; reversed bounds fail |
| `bloom.core.scalar.remap` | Remap | `value`, `sourceMinimum`, `sourceMaximum`, `destinationMinimum`, `destinationMaximum` | reversed ranges and extrapolation allowed; equal source bounds fail |
| `bloom.core.scalar.mix` | Mix | `start`, `end`, `factor` | exact endpoints and extrapolation; never clamps |

The table has semantics version `1`. Primitive IDs identify evaluator kernels, not durable
artist-facing node types. Adding an ID may extend the version; changing an existing operation's
operand order, precision, formula, validation, or error behavior requires a new semantics version.
Old compiled or cached behavior is never silently reinterpreted.

Only exact `float` and `double` inputs are accepted. All operands must be finite; subnormals and
signed zero are valid except for a zero denominator. Result or intermediate NaN/infinity is a
failure, never saturation. Evaluation requires the declared round-to-nearest environment and
supported subnormal behavior; an incompatible ambient floating-point environment fails explicitly.
Implicit contraction and fast-math reassociation are prohibited. Only operations whose contract
names a fused result may call an explicit fused primitive.

Validation precedence is known primitive, exact arity, all-input finiteness, floating-point
environment, operation-domain checks, then result finiteness. Failures expose no plausible numeric
fallback. The code-level error vocabulary distinguishes unknown primitive, invalid arity,
unsupported environment, non-finite input, divide-by-zero, invalid interval, degenerate range, and
non-finite result.

These are kernels, not artist-visible node records. Artist nodes add typed ports, defaults, units,
animation roles, UI metadata, and lowering without changing the kernel math.

## Animation Sampling Version 1

Animation interval selection is exact rational work, not scalar floating-point work. Runtime first
orders and locates keys using normalized `RationalTime`, derives an exact interval factor, and rounds
that factor once to binary64 with round-to-nearest, ties-to-even. Hold returns the left value;
Linear applies Float64 scalar Mix version 1 to the scalar or each `Vec2d` component. Exact key times
and endpoints return stored values or exact factors without an avoidable arithmetic round trip.

The sampling semantics version is `1` and participates in compiled-plan compatibility and cache
identity. Full curve ownership, extrapolation, commands, diagnostics, and the portable fixed-width
rational conversion contract are defined in
[`animation-and-time.md`](animation-and-time.md).

## CPU Image Primitive Vocabulary Semantics Version 2

`bloom_render` now provides the allocation-free CPU reference row kernels used by the first
composition evaluator. Their semantics version is `2`; the evaluator and process-frame cache
identity record that version explicitly.

- Solid authoring colors are straight `Color4d` under the frozen authoring-encoding metadata
  `bloom.reference.linear-srgb`. That metadata remains distinct from the process-image identity.
  Version 1 Solid lowering explicitly declares the authoring encoding numerically equivalent to
  `lin_rec709_scene`; a future authoring encoding requires an explicit qualified transform policy
  instead of relabelling its numbers. The conversion validates the authored value, multiplies RGB
  by alpha in Float64, then performs one checked Float32 conversion. Alpha that is authored as zero
  or rounds to Float32 zero produces exact transparent black.
- Translation and opacity are validated once per operation. Bilinear sampling gathers
  premultiplied pixels, uses transparent taps outside the source data window, preserves exact
  integer/zero/one endpoints, and applies opacity to all four sampled components.
- Source-over consumes separate source and in-place destination rows. The first Layer Stack entry is
  topmost, so evaluation visits stack entries in reverse order and folds bottom-to-top. Process RGB
  is never clamped.
- The temporary unqualified reference display mapper robustly unpremultiplies, clips only at the
  display boundary, applies the `lin_rec709_scene` to sRGB transfer, and produces straight packed
  RGBA8. Checked-in inverse-transfer half-code thresholds make byte quantization independent of
  platform `libm`. Its prepared display product and identity are distinct from process evaluation.
- Every authored arithmetic boundary requires round-to-nearest with preserved subnormal inputs and
  results. Primitive rows allocate no storage, start no threads, and expose structured failures.

The initial evaluator treats a Layer Output position as an absolute source-center coordinate in the
full composition raster. `(width / 2, height / 2)` is identity for the current composition-sized
Solid source. Positive X moves right and positive Y moves down. A proxy scales the displacement per
axis and derives a pixel aspect that preserves full-resolution display aspect. Pixel centers use the
same half-pixel lattice on both sides of the inverse gather, so an integer displacement remains
exact.

Evaluation is deliberately full-frame and single-worker in this version. Preflight validates every
operation reference, computes exact consumer counts and the peak live pixel bytes, and rejects the
request before image allocation when its aggregate budget is insufficient. Images are released
after their last consumer. Cancellation is checked at operation and scanline boundaries, and a
cancelled or failed evaluation publishes no partial frame. The normative successful evaluation
product owns only the premultiplied process image. Display preparation consumes that immutable
product in a separate typed stage and owns its own buffer, diagnostics, budget, and cache identity.
The live `ProcessFrame`, `ReferenceDisplayFrame`, and `PreparedPreviewFrame` types enforce that
ownership split. The preview pipeline runs process evaluation and reference-display preparation
sequentially on one worker without nesting task submissions or exposing a combined cache identity.

## Primitive Families

| Family | Owner | Examples | Priority |
| --- | --- | --- | --- |
| Scalar and control | `src/core` | checked arithmetic, remap, interpolation, comparison, select | Foundation |
| Vector and transform | `src/core` | vector math, matrices, affine/projective transforms | Essential |
| Color values | `src/core` plus color service contracts | tagged color intent, luminance models, color-aware interpolation | Essential; never generic vector math |
| Image composition | `src/render` | fill, copy, premultiply, unpremultiply, source-over, blend | First pixels |
| Sampling and spatial | `src/render` | nearest/bilinear sampling, affine warp, crop, resize, borders | First pixels |
| Channels and masks | `src/render` | extract/combine/copy channels, coverage math, morphology | Essential compositor |
| Neighborhood filters | `src/render` | blur, sharpen, convolution, halo computation | Essential compositor |
| Reduction and analysis | `src/render` | bounds, histogram, statistics, tracking inputs | Advanced |
| Temporal | `src/runtime` and `src/render` | time mapping, frame blending, motion, bounded history | Motion/VFX |
| Procedural | `src/core` or owning domain | versioned hash, random, noise, paths | Motion design |
| Optional domain | contributing module | geometry fields, 3D scenes, materials, simulations | Pipeline-specific |

## Color And Alpha Contract

Color-sensitive primitives require a declared color context. Generic scalar and vector kernels do
not accept color values and never invoke color management implicitly.

An image or color evaluation boundary carries:

- a resolvable color-encoding identity, including the declared scene-linear role where required
- semantic channel roles distinguishing color from alpha, masks, depth, normals, motion, UVs, IDs,
  and other data
- straight or premultiplied alpha association
- data and display windows, pixel aspect, and coordinate convention where relevant
- the processing space selected by the node, render request, or explicit color-transform operation

`Color4d` is a straight authoring value. RGB may be negative or HDR; alpha is finite in `[0, 1]`;
its owning schema supplies encoding and role. `Color4d` performs no conversion and is never passed
to scalar primitives as an opaque four-component number. The initial Solid schema binds its value
to authoring encoding `bloom.reference.linear-srgb`; its version 1 lowering policy declares the
numeric identity conversion to the process space. Authoring metadata is not rewritten to
`lin_rec709_scene` merely because that conversion is currently an identity.

Canonical `Rgba32f` process storage is premultiplied Float32 RGBA. RGB remains finite and unclamped,
alpha remains finite in `[0, 1]`, the image descriptor carries `lin_rec709_scene`, and alpha zero
canonicalizes RGB to exact zero. A qualified OCIO config must resolve that exact ID before any
operation that needs an OCIO transform; a matching alias, role, or display name is insufficient.

The live `ColorEncoding::LinearRec709Scene`, `EvaluationColorIntent::LinearRec709Scene`, CPU image
primitive semantics version `2`, CPU evaluator semantics version `2`, and reference display-mapper
semantics version `2` implement this process identity. They supersede the scaffold's ambiguous
reference-linear naming; cache identity rejects the older semantic versions rather than treating
the rename as metadata-only.

Binding rules:

1. Import establishes explicit source interpretation before color-sensitive processing.
2. Scene/process pixels and display-referred presentation remain separate.
3. Compositing and light-like operations run in a declared scene-linear working space unless the
   node explicitly defines another model.
4. Perceptual operations declare their model and processing space; the implementation does not
   guess one from storage values.
5. Masks and data bypass color transforms by semantic role, while their numeric filtering behavior
   remains explicit.
6. Alpha association conversions are explicit. Zero-alpha unpremultiplication and RGB
   reconstruction use operation-defined behavior and diagnostics.
7. Negative and HDR RGB survive by default. Clamp, gamut mapping, and tone mapping occur only
   through operations whose declared purpose includes them.
8. Process-cache identity includes only configuration, context, process space, alpha, quality,
   precision, provider, and algorithm inputs that can change process pixels. Display-cache identity
   begins with the exact process-frame identity and additionally includes display/view, looks,
   monitor/output intent, prepared-processor identity, packing, and display algorithm versions.
9. CPU and GPU providers implement the same declared operation space and alpha equation. A backend
   cannot omit color processing or substitute display-referred math.

The implemented Solid authoring-to-process policy freezes its declared authoring encoding, identity
conversion, Float64-to-Float32 rounding, range failure, premultiplication order, and zero-alpha
behavior in tests. Layer opacity multiplies all four premultiplied components. Reference
source-over is:

```text
out.rgb = source.rgb + destination.rgb * (1 - source.alpha)
out.a   = source.a   + destination.a   * (1 - source.alpha)
```

RGB is not clamped, valid alpha remains in `[0, 1]`, and transparent borders are exact zero. A
future Color Mix or grade operation defines its own space and alpha contract; generic scalar Mix
does not define color interpolation.

## Execution Classification

Every image or domain primitive declares the minimum information needed for correct scheduling:

| Class | Required contract |
| --- | --- |
| Uniform | one result independent of image coordinates |
| Pointwise | each output element depends only on corresponding inputs |
| Neighborhood | finite input halo derived from parameters and quality |
| Warp/gather | output-to-input coordinate mapping, filter footprint, and border mode |
| Reduction | input domain, deterministic combination policy, and result scope |
| Global | full-domain dependency and bounded resource strategy |
| Temporal | exact requested time, dependency window, sampling, and history policy |
| Analysis | cancellable work producing durable or derived data, diagnostics, and uncertainty |

Region of interest, domain of definition, halo, cancellation cadence, deterministic ordering, cache
identity, device capability, and fallback are semantic contracts rather than backend hints.

## Lowering, Diagnostics, And Compatibility

- Primitive IDs and node type IDs are stable, namespaced, and independently versioned.
- Node schema versions govern saved authoring compatibility. Primitive semantics versions govern
  compiled/evaluated behavior and cache identity.
- Compilation validates exact type, version, roles, and ports, then lowers only reachable nodes.
  Primitive selection happens during lowering, never through per-pixel lookup or type erasure.
- Published plans cannot contain invalid arity. If one does, it is an internal compiler/evaluator
  diagnostic. Numeric failures are scoped to the responsible node and operation.
- A node with no compatible lowering or provider is unsupported with structured diagnostics; it is
  not approximated silently. Preview may retain last-good pixels, while final output fails loudly.
- Primitive implementations avoid hidden allocation, process-global mutable state, ambient color
  configuration, implicit threading, and UI dependencies.
- CPU reference fixtures cover signed zero, subnormals, rounding environment, overflow, extreme
  finite ranges, negative/HDR color, alpha endpoints, hostile image extents, and cancellation.
- GPU/provider registration is per operation and precision. A provider that cannot meet the
  operation's fused, signed-zero, subnormal, failure, and tolerance rules falls back explicitly.

The planned artist-facing vocabulary is maintained in
[`../product/node-catalogue.md`](../product/node-catalogue.md).
