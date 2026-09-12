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

## Scalar Primitive Vocabulary Version 2

`bloom_core` defines checked Float32 and Float64 primitives. Semantics version `2` is an append-only
extension of version `1`: the original enum values, stable IDs, operand orders, formulas, and errors
remain unchanged.

The version 1 base is:

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

Version 2 adds this color-agnostic tranche:

| ID | Operation | Operands | Semantics |
| --- | --- | --- | --- |
| `bloom.core.scalar.absolute` | Absolute | `value` | non-negative magnitude; both zero signs produce `+0` |
| `bloom.core.scalar.negate` | Negate | `value` | reverses the sign, including signed zero |
| `bloom.core.scalar.sign` | Sign | `value` | negative values produce `-1`, positive values produce `+1`, and zero retains its sign |
| `bloom.core.scalar.reciprocal` | Reciprocal | `value` | `1 / value`; both zero signs fail as divide-by-zero |
| `bloom.core.scalar.square-root` | Square Root | `value` | principal square root; strict negatives fail outside the domain and `-0` produces `-0` |
| `bloom.core.scalar.floor` | Floor | `value` | greatest integral value no greater than the input |
| `bloom.core.scalar.ceiling` | Ceiling | `value` | least integral value no less than the input |
| `bloom.core.scalar.round` | Round | `value` | nearest integral value; halfway cases tie to an even integer |
| `bloom.core.scalar.truncate` | Truncate | `value` | integral value toward zero |
| `bloom.core.scalar.fraction` | Fraction | `value` | signed truncation-relative fractional part in `(-1, 1)` |
| `bloom.core.scalar.modulo` | Modulo | `dividend`, `divisor` | truncating remainder with the dividend sign; both zero divisor signs fail |
| `bloom.core.scalar.step` | Step | `edge`, `value` | `0` below the edge and `1` at or above it |
| `bloom.core.scalar.smoothstep` | Smoothstep | `lowerEdge`, `upperEdge`, `value` | clamped interval factor followed by `t * t * (3 - 2 * t)` |
| `bloom.core.scalar.smootherstep` | Smootherstep | `lowerEdge`, `upperEdge`, `value` | clamped interval factor followed by `t * t * t * (t * (t * 6 - 15) + 10)` |

Primitive IDs identify evaluator kernels, not durable artist-facing node types. Adding an ID may
extend the version; changing an existing operation's operand order, precision, formula, validation,
or error behavior requires a new semantics version. Old compiled or cached behavior is never
silently reinterpreted.

The version is `2` even though every version 1 operation retains its exact contract because the
closed primitive vocabulary is itself part of compiled-plan and cache compatibility. A consumer
that recorded version `1` must be explicitly supported as version `1` or recompiled; it cannot
infer version `2` merely because the particular operation it uses predates the extension.

Only exact `float` and `double` inputs are accepted. All operands must be finite; subnormals and
signed zero are valid except for a zero denominator. Result or intermediate NaN/infinity is a
failure, never saturation. Evaluation requires the declared round-to-nearest environment and
supported subnormal behavior; an incompatible ambient floating-point environment fails explicitly.
Implicit contraction and fast-math reassociation are prohibited. Only operations whose contract
names a fused result may call an explicit fused primitive.

Validation precedence is known primitive, exact arity, all-input finiteness, floating-point
environment, operation-domain checks, then result finiteness. Failures expose no plausible numeric
fallback. The code-level error vocabulary distinguishes unknown primitive, invalid arity,
unsupported environment, non-finite input, divide-by-zero, invalid interval, degenerate range,
non-finite result, and an input outside an operation's mathematical domain.

Version 2 freezes these additional details:

- Absolute, Negate, Sign, Floor, Ceiling, Round, Truncate, Fraction, Modulo, and Square Root retain
  the signed-zero behavior stated in the table. In particular, rounding or fraction results that
  approach zero from below retain `-0`. Step and the two smooth functions instead return canonical
  `+0` or `+1` at their closed boundaries.
- Finite subnormal inputs are accepted and are never intentionally flushed. A finite subnormal
  result is successful. Reciprocal of a nonzero value fails only when its rounded result is
  non-finite; therefore reciprocal of the smallest subnormal reports a non-finite result rather
  than divide-by-zero.
- Square Root is the principal result rounded to the bound precision under the required
  round-to-nearest environment. `-0` is in-domain; every finite value less than zero is not.
- Floor, Ceiling, Round, and Truncate return a floating value in the input precision, not an integer
  conversion. Round is round-to-nearest, ties-to-even and uses the required ambient rounding mode.
  Fraction is the signed fractional result returned by splitting at Truncate; integral negative
  inputs therefore produce `-0`.
- Modulo is the finite truncating remainder corresponding to a quotient rounded toward zero. Its
  magnitude is less than the divisor magnitude, and an exact-zero result retains the dividend's
  sign. No Euclidean non-negative adjustment is performed.
- Step compares `value < edge`; signed zeros compare equal and therefore select `1`.
- Smoothstep and Smootherstep require `lowerEdge < upperEdge`. Equal edges report a degenerate
  range; reversed edges report an invalid interval. Values at or below the lower edge return exact
  `+0`, and values at or above the upper edge return exact `+1`. Interior normalization uses the
  robust version 1 Remap factor, including opposite-sign extreme bounds. The polynomial expressions
  are evaluated in the parenthesized order printed in the table, with separate unfused operations;
  the compiler may not contract them.

Power, Logarithm, and Exponential remain deferred until their domain, overflow, and qualified
transcendental-library conformance policy is frozen. Wrap is deferred until half-open endpoint and
extreme-range reduction behavior is frozen. Snap is deferred until its anchor, signed increment,
halfway tie, and overflow rules are frozen. Omitting these operations is preferable to exposing
backend-dependent math under a stable ID.

These are kernels, not artist-visible node records. Artist nodes add typed ports, defaults, units,
animation roles, UI metadata, and lowering without changing the kernel math.
Evaluation is `noexcept`, allocation-free, and has no mutable or hidden service state. It reads the
ambient floating-point controls only to validate them before authored arithmetic.

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

## CPU Image Primitive Vocabulary Semantics Version 4

`bloom_render` now provides the allocation-free CPU reference row kernels used by the first
composition evaluator. Their semantics version is `4`; the evaluator and process-frame cache
identity record that version explicitly. Version 3 added the text coverage kernel and the glyph
rasterizer below; version 4 replaces the translate-only layer resample with the affine one described
under "Layer Transform Resampling". One number covers them all, deliberately: neither the rasterizer
nor the resampler carries a second semantics version that could drift out of the identity a published
frame records.

- Solid authoring colors are straight `Color4d` under the frozen authoring-encoding metadata
  `bloom.reference.linear-srgb`. That metadata remains distinct from the process-image identity.
  Version 1 Solid lowering explicitly declares the authoring encoding numerically equivalent to
  `lin_rec709_scene`; a future authoring encoding requires an explicit qualified transform policy
  instead of relabelling its numbers. The conversion validates the authored value, multiplies RGB
  by alpha in Float64, then performs one checked Float32 conversion. Alpha that is authored as zero
  or rounds to Float32 zero produces exact transparent black.
- The layer transform and opacity are validated once per operation. Bilinear sampling gathers
  premultiplied pixels, uses transparent taps outside the source data window, preserves exact
  integer/zero/one endpoints, and applies opacity to all four sampled components. See "Layer
  Transform Resampling" below.
- Source-over consumes separate source and in-place destination rows. The first Layer Stack entry is
  topmost, so evaluation visits stack entries in reverse order and folds bottom-to-top. Process RGB
  is never clamped.
- The temporary unqualified reference display mapper robustly unpremultiplies, clips only at the
  display boundary, applies the `lin_rec709_scene` to sRGB transfer, and produces straight packed
  RGBA8. Checked-in inverse-transfer half-code thresholds make byte quantization independent of
  platform `libm`. Its prepared display product and identity are distinct from process evaluation.
- Text coverage is rasterized from glyph outlines into 8-bit area coverage, then composited by a row
  kernel that scales an already-premultiplied process pixel by `coverage / 255`. **A coverage byte is
  a linear area fraction, not a gamma-encoded intensity**, so it is used directly as linear alpha and
  no transfer function, sRGB curve, or gamma exponent appears anywhere between the rasterizer and the
  process pixel. Compositing coverage in a display-encoded space is the classic cause of fringed,
  too-thin text; the process space is scene-linear, so the correct result and the simple
  implementation coincide. Zero coverage is exactly transparent black and full coverage is exactly
  the unmodified process pixel, with no multiply that could round either endpoint away.
- Text authoring colors use the same straight `Color4d` values and the same
  `bloom.reference.linear-srgb` authoring-encoding metadata Solid colors use, converted by the same
  Solid conversion above; the coverage kernel then scales that one pixel. A text color is a solid
  color that glyph coverage attenuates, which is why there is no separate text color conversion.
- Every authored arithmetic boundary requires round-to-nearest with preserved subnormal inputs and
  results. Primitive rows allocate no storage, start no threads, and expose structured failures. The
  glyph rasterizer is the one text-path exception: it allocates its coverage bitmap, so it checks the
  bitmap's byte count against the caller's budget from the computed extent BEFORE allocating anything
  and before drawing any glyph.

### Layer Transform Resampling

One inverse-mapped affine bilinear resample serves the whole Layer Output stage: translation composed
with rotation composed with scale, all turning about the authored anchor (the authoring model is in
[`layer-graph-model.md`](layer-graph-model.md), "Layer Transform"). There is no separate translate-only
kernel in the stage; the translate-only case is a path inside this one.

- **Inverse mapping.** Each output pixel centre maps back to one source coordinate through the
  precomputed inverse 2x2 matrix and pivot. Nothing per-pixel computes trigonometry, divides, or
  branches on an authored value.
- **Quarter turns are exact.** A rotation that is an exact multiple of 90 degrees resolves to exact
  `0` and `±1` cosine and sine instead of `std::cos`/`std::sin` of a rounded radian value, so a
  quarter turn maps pixel centres onto pixel centres and interpolates nothing. The reduction modulo
  360 is exact, so an authored 450 or -90 is as exact as a 90 or 270.
- **Translate-only is bit-identical to version 3.** Unit scale on both axes together with a rotation
  that reduces to exactly zero resolves to a path that computes the pre-version-4 arithmetic and
  nothing else — the anchor is not added and subtracted, because doing so would perturb the last bit
  of a subpixel translation. The former translate-only primitive is retained in `bloom_render`,
  unchanged and uncalled by the stage, solely as the reference a test pins that path against; a copy
  in the test tree could drift apart from the shared interpolation and sampling helpers and prove
  nothing.
- **Premultiplied edges.** Taps outside the source data window are exact transparent black, and the
  process representation is premultiplied, so interpolating towards that transparent black is already
  the correct edge falloff: no unpremultiply/repremultiply round trip is involved and no edge pixel
  can carry colour above its own alpha.
- **Bounds.** A layer's output data window is its transformed bounds clipped to the composition, and
  its display window stays the composition's. The transformed bounds are the integer bounding box of
  the forward image of the bilinear support box (the source data window grown by one pixel on every
  side), so they may include a pixel the resample then writes as transparent but can never exclude
  one it would write as opaque. A scaled-down, rotated, or moved layer therefore allocates and
  resamples only the pixels it can reach. The Layer Stack composites each entry over the rows and
  columns that entry's own data window occupies.
- **Empty layers.** A layer whose transformed bounds miss the composition entirely, and a layer
  collapsed by a scale factor of exactly zero, publish no image at all. The Layer Stack treats an
  absent entry image as a layer that contributes nothing, which is exactly what compositing an empty
  layer means — not an evaluation failure.
- **Proxy.** A proxy frame is the same picture at a smaller extent, so the full-resolution transform
  is conjugated by the per-axis proxy factor rather than re-authored. With equal horizontal and
  vertical factors — every proportional proxy extent — the device transform is exactly the
  full-resolution one. With unequal factors the conjugation keeps the proxy a faithfully squashed
  picture of the full-resolution frame, a rotated layer included, rather than pretending device
  pixels are square.

### Text Rasterization Version 1

Text rasterization is Qt-free and deterministic. One face is available -- the DejaVu Sans Book TTF
vendored for the interface, embedded into `bloom_render` as a build-time byte array so no evaluation
reads a font from the filesystem -- and `bloom.text-source` therefore has no font parameter: a font
parameter would persist a choice neither the schema nor the renderer can honor. Outline rasterization
is the vendored `stb_truetype` header, compiled into one translation unit behind a narrow Bloom-owned
adapter; its acquisition provenance, license review, and security review are in
`dependencies/licenses/stb_truetype/`, and that security review qualifies the library only for font
bytes Bloom itself pins.

Layout in version 1 is deliberately minimal and explicitly not text layout: one line, left to right,
each glyph advancing by its own horizontal advance plus the face's kern pair, no wrapping, no
shaping, no bidirectional reordering, and no line breaks -- a newline is a glyph lookup like any other
codepoint. Shaping, font asset identity, and layout contracts remain deferred as the roadmap states;
this version exists so the CPU reference evaluator can produce pixels for the text source that the
document schema has always carried.

Geometry is stated once so compositing has no latitude. The text origin is the pen start on the
ascender line: `x` is the left edge of the first glyph's advance and `y` is the font's ascent above
the baseline, with the baseline snapped to a whole row once for the whole line so the same string at
the same size always rasterizes identically. The evaluator places that origin at the frame's own data
window origin, which puts the first line inside the frame with its ascender flush to the top edge;
the Layer Output position then moves the whole layer from there, exactly as it moves a Solid. The
coverage bitmap is the exact union of every glyph's ink, so its offset from the text origin may be
negative, and clipping to the frame is the compositor's decision rather than the rasterizer's. Em
size is per axis, taken from the same scale factors a proxy evaluation applies to layer translation,
so a proxy frame holds a smaller picture of the same composition rather than full-size glyphs in a
small frame. The authored size is bounded identically by the document schema and the rasterizer; the
two bounds are held equal at compile time in `src/runtime`, the one module that sees both.

Empty content, and content whose glyphs are all blank, rasterize to no coverage and compose a
transparent frame. That is a success, not a failure: a text layer an artist has not typed into yet is
a real, selectable, editable layer. A codepoint the face does not cover resolves to the face's own
missing-glyph box, so unsupported text is visibly missing rather than silently dropped, and content
that is not well-formed UTF-8 is refused at the document boundary rather than rendered as
replacement boxes.

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
| Text coverage | `src/render` | glyph outline rasterization to 8-bit area coverage, coverage-scaled solid composition | First pixels |
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
primitive semantics version `4`, CPU evaluator semantics version `4`, and reference display-mapper
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
