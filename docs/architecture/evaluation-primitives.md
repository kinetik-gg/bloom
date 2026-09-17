# Evaluation Values And Primitive Semantics

Status: working

Updated: 2026-09-17

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

## CPU Image Primitive Vocabulary Semantics Version 7

`bloom_render` now provides the allocation-free CPU reference row kernels used by the first
composition evaluator. Their semantics version is `7`; the evaluator and process-frame cache
identity record that version explicitly. Version 3 added the text coverage kernel and the glyph
rasterizer below; version 4 replaced the translate-only layer resample with the affine one described
under "Layer Transform Resampling"; version 5 added the per-mode blend kernel described under
"Blending"; version 6 adds the path coverage and stroke outlines below; version 7 adds affine vector coverage. One number covers them all: neither the rasterizer, the resampler, nor the
blend kernel carries a second semantics version that could drift out of the identity a published
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
- Blending takes the same two rows plus the layer's own blend mode, and is what the Layer Stack stage
  actually calls; source-over remains the kernel the `Normal` mode delegates to, unchanged. See
  "Blending" below.
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
- **Bounds.** A layer's output data window covers its transformed local content, including content
  outside the composition; its display window stays the composition's. The transformed bounds are the integer bounding box of
  the forward image of the bilinear support box (the source data window grown by one pixel on every
  side), so they may include a pixel the resample then writes as transparent but can never exclude
  one it would write as opaque. A scaled-down, rotated, or moved layer therefore allocates and
  resamples only the pixels it can reach. The Layer Stack composites each entry over the rows and
  columns that entry's own data window occupies.
- **Empty layers.** Empty content and a layer collapsed by a scale factor of exactly zero publish
  no image. Off-composition content remains available for a parent transform to bring back. The Layer Stack treats an
  absent entry image as a layer that contributes nothing, which is exactly what compositing an empty
  layer means — not an evaluation failure.
- **Proxy.** A proxy frame is the same picture at a smaller extent, so the full-resolution transform
  is conjugated by the per-axis proxy factor rather than re-authored. With equal horizontal and
  vertical factors — every proportional proxy extent — the device transform is exactly the
  full-resolution one. With unequal factors the conjugation keeps the proxy a faithfully squashed
  picture of the full-resolution frame, a rotated layer included, rather than pretending device
  pixels are square.

### Blending

The Layer Stack stage folds each entry through one blend kernel carrying that entry's own Layer Output
blend mode. The mode vocabulary, every formula, and the premultiplied compositing fold are owned by
[`color-management.md`](color-management.md), "Blend modes"; what belongs here is the primitive's
contract.

- **Ordering and blending are separate.** The stack says which layer is above which and is still
  visited in reverse so the first entry is topmost; the mode comes from the Layer Output the entry
  names, so reordering two layers and re-blending one are independent edits.
- **Alpha is source-over under every mode.** Only the colour combination varies, computed with exactly
  the alpha expression the source-over kernel uses, so a mode never changes how much of the backdrop a
  layer covers.
- **`Normal` is bit-identical to version 4.** The kernel delegates to the retained source-over row for
  that mode rather than re-deriving it, so a composition whose every layer is `Normal` produces the
  same bits it did before blend modes existed. The semantics version still moves, because the same
  plan value can now mean a different picture.
- **`Add` needs no round trip.** Its separable function reduces the general fold to premultiplied
  addition exactly; the other five non-`Normal` modes unpremultiply, apply the mode, and
  re-premultiply, with Float64 intermediates and one Float32 rounding at the end.
- **Alpha endpoints are exact.** An alpha-zero source is skipped and leaves the backdrop untouched; an
  alpha-zero backdrop takes the source pixel through unchanged, which is what the fold collapses to
  under every mode. Source-over's other shortcut -- an opaque source replacing the destination -- does
  NOT generalise and is deliberately absent, because every mode but `Normal` still reads the
  backdrop's colour at full source alpha.
- **Nothing is clamped.** A negative or HDR channel survives the blend, and a non-finite or
  finite-overflow result fails the row with a typed error rather than being clipped.
- **The blend mode is a discrete authored value.** It carries no curve: the schema declares it
  non-animatable, so the compiled plan holds a resolved enumerator rather than a parameter source.

### Text Rasterization And Local Layout

Text rasterization is Qt-free and deterministic. `bloom_render` embeds four vendored faces as
build-time byte arrays: DejaVu Sans Book, Inter Regular, Inter Medium, and Inter SemiBold. A text
font is a non-animatable reference to a `Font` asset, never embedded project bytes. The compiler
resolves that asset through the platform catalogue, reads a bounded system file at compile time,
verifies its digest, and registers the parsed face once per digest in the glyph registry. Evaluation
never reads a font from the filesystem. A missing or changed reference emits a warning and uses
embedded DejaVu Sans while preserving the asset record for relink or replacement. A pre-reference
text node with no binding still produces the old pixels. Outline rasterization is the vendored
`stb_truetype` header, compiled into one translation unit behind a narrow Bloom-owned adapter; its
acquisition provenance, license review, and security review are in
`dependencies/licenses/stb_truetype/`, and that security review qualifies the library only for font
bytes Bloom itself pins. Each embedded file has a configure-time byte-count assertion.

Text v2 lays out multiple lines using glyph advances and kerning plus authored letter spacing.
Without a box, each line is aligned Left, Center, or Right within the widest line and the glyph union
is the local bounds. With a box, optional word wrapping uses the box width, horizontal alignment is
within the box, vertical alignment positions the line stack, and Clip or Grow determines whether the
coverage window is clipped to the box. Point text's anchor mode shifts the baseline anchor without
changing its content-sized bounds. Line baselines are separated by em size times line height and
snapped to integer rows. Empty lines advance layout without inventing ink. CRLF is accepted. Shaping
and bidi remain deferred.

Solid v2 evaluates its required dimension operands
into exact local bounds and uses a containing integer buffer, with fractional edge coverage.

Empty content, and content whose glyphs are all blank, rasterize to no coverage and compose a
transparent frame. That is a success, not a failure: a text layer an artist has not typed into yet is
a real, selectable, editable layer. A codepoint the face does not cover resolves to the face's own
missing-glyph box, so unsupported text is visibly missing rather than silently dropped, and content
that is not well-formed UTF-8 is refused at the document boundary rather than rendered as
replacement boxes.

Layer v4 maps `centre(bounds) + anchor` to position in composition space. The plan carries required
dimension and layout operands and one placement contract. At the sampled frame time, the worker
retains each operation's local and output rectangles and each layer's transformed corners and anchor.
These values accompany the immutable ProcessFrame and operation-cache result; the viewer reads them
without evaluation or rasterization. They are derived state and never persisted as project truth.

Merge v2 takes the union of transformed input bounds and retains its intermediate image outside the
composition. Composition Output crops to the composition window. Source bounds are distinct from the
bilinear support window so filtering padding never shifts an anchor. Zero scale and empty glyph
content produce empty geometry.

Preflight validates operand references and schema domains. Actual intermediate image allocations also
check the remaining request pixel budget, including live text coverage. Cancellation is checked at
operation and scanline boundaries, and cancelled/failed evaluation publishes no partial frame.
Operation memoization includes dimension, typography, and dependency values and retains matching
evaluated geometry. Display preparation remains a separate typed stage.

Evaluator semantics 8, primitive semantics 7, plan semantics 5, and animation semantics 2 are the
current identities. TEXT-2 moved the plan version for the text reference/layout grammar and SHAPE-1
moved the evaluator and primitive versions for path rasterization; the two lanes landed together, so
all three steps are reflected in one set of re-derived identity goldens. Existing point-text pixels
remain pinned because the zero-box defaults take the old path; only new box-layout pins were added.

### Path coverage

`src/render/path_raster.cpp` owns the Qt-free path reference primitive. Coordinates stay in
full-resolution author space. Cubic subdivision bounds deviation and control-polygon excess to
1/32 of an output pixel using the larger per-axis proxy scale. Subdivision is capped at depth 20
and generated geometry at 262,144 points; pathological geometry fails with a diagnostic. The
reference evaluator checks the floating-point environment before invoking these kernels.

Coverage is deterministic 4×4 centre sampling, with half-open scanline crossings and winding or
parity accumulation. The byte is `(coveredSamples * 255 + 8) / 16`, a linear area fraction.
Analytic integration is deliberately deferred: the fixed grid gives a small, independently
verifiable CPU contract. Rectangle corners and ellipses use cubic quarter arcs; ellipses use four
cubics with kappa 0.5522847498307936. Polygon corner radii use tangent circular arcs represented by
cubics, constrained to half the adjacent edge lengths. Polygon and Star extrema are fitted to
the authored bounding box.

A stroke builds consistently wound outline polygons for segment strips, joins and caps, then fills
their union. Miter joins have a fixed limit of four half-widths and fall back to Bevel; Round joins
and caps flatten to the same tolerance. Inside and Outside closed strokes use a full-width outline
on each side and intersect coverage samples with the fill region or its complement. This defines
alignment even at self-crossings under either fill rule. Open strokes use Center. Degenerate
zero-length paths have no coverage. Bounds include visible stroke extents; Inside uses the path
bounds. Fill and stroke share `coverageSolidRow`, with stroke composited source-over afterwards.

Flattening, outline construction and coverage scanlines check cancellation. Evaluation reports the
normal operation progress and accounts for concurrent coverage/stroke rows before allocating the
content-sized process image. Empty paths are successful empty images. The reference tests compare
square, circle and five-point-star coverage with independent geometric predicates, and pin cap,
alignment, fill-rule, proxy and cancellation behavior. Runtime tests pin rectangle, circle, star,
stroked-line and even-odd-path pixels and animation.

SHAPE-1 advances image primitive semantics 5 → 6 and evaluator semantics 6 → 7; TEXT-2 advances
plan semantics 4 → 5 in the same integration, and animation semantics stay 2. The independent
output-identity oracle reproduces every prior identity golden set -- including each lane's own
pre-merge set -- before deriving the single combined replacement; existing pixel goldens remain
unchanged and the new text and shape pixel pins keep passing.

## Primitive Families

| Family | Owner | Examples | Priority |
| --- | --- | --- | --- |
| Scalar and control | `src/core` | checked arithmetic, remap, interpolation, comparison, select | Foundation |
| Vector and transform | `src/core` | vector math, matrices, affine/projective transforms | Essential |
| Color values | `src/core` plus color service contracts | tagged color intent, luminance models, color-aware interpolation | Essential; never generic vector math |
| Image composition | `src/render` | fill, copy, premultiply, unpremultiply, source-over, blend | First pixels |
| Sampling and spatial | `src/render` | nearest/bilinear sampling, affine warp, crop, resize, borders | First pixels |
| Channels and masks | `src/render` | extract/combine/copy channels, coverage math, morphology | Essential compositor |
| Path coverage | `src/render` | bounded cubic flattening, winding/parity coverage, filled stroke outlines | Built |
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
primitive semantics version `7`, CPU evaluator semantics version `8`, and reference display-mapper
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

## Viewer Analysis Requests

`EvaluationRequest`, `ProcessFrameIdentity`, and `PreviewRequestIdentity` carry an optional `roi`:
a nonempty integer pixel rectangle at the resolved request resolution, inside its display window.
The viewer stores its selection in composition coordinates and rounds its scaled minimum down and
maximum up when resolving a proxy request. Export requests leave ROI absent.

ROI limits raster output, not content geometry. Solid, Text, Shape, Layer Output, and Merge clip
applicable data windows before their row passes; evaluated bounds, polygons, anchors, and the
composition display window remain unchanged. A gather cannot crop its inputs to the output ROI:
transforms and their upstream dependencies currently retain conservative full input support so
translation, filtering, nested merges, and parenting cannot lose contributing pixels. Media decode
also retains its complete source. This is a correctness fallback, not a claim that every upstream
operation costs one pixel for a one-pixel request. Composition Output stores only the requested
rectangle. Both display preparers pad the remaining viewport with transparent black.

ROI participates in image-operation and preview-frame keys and process-frame equality. The plan
cache is unchanged. An absent ROI retains the existing operation-key encoding and raster path;
primitive, evaluator, animation, and display semantics versions are unchanged. Identity fixtures
compare ROI pixels bit for bit with full evaluation, retain the same bounds, and restore the
unchanged full-frame result after clearing ROI.

`ViewAdjust{exposure, gamma}` belongs to display preparation and preview display identity. Exposure
multiplies linear display light by `2^exposure` after the view transform and before sRGB encoding.
Gamma applies `pow(encoded, 1/gamma)` to the encoded RGB before RGBA8 quantization. Alpha is
unchanged. The qualified path uses the processor's floating display output before quantization,
undoing only the sRGB encoding for the exposure step; it never adjusts already quantized bytes.
Finite EV values in `[-32, 32]` and gamma in `[0.01, 10]` are accepted. EV zero and gamma one bypass
adjustment and preserve both original display paths bit for bit.

Each viewer prepares its adjusted result on a cancellable worker, coalesces changed controls, and
rejects retired results. Its controls persist by editor-area identity in QSettings. The shared
neutral preview, thumbnails, process identities, authored colours, and export requests do not
inherit this adjustment. Adjusted and ROI-padded qualified buffers retain qualified provenance;
`PreparedPreviewFrame::displayBufferView()` selects the final viewport buffer.

The pixel probe reads straight RGBA8 and its normalized values from that viewport buffer. Reference
RGBA is the exact premultiplied `lin_rec709_scene` Float32 process value, widened to Float64 without
a colour transform. A retained process image supports a constant-time pixel read. Display-only
cache hits, or positions outside the preview ROI, request a one-pixel ROI through the evaluator on
a worker. The probe caches one frame/position result, cancels superseded work, and discards late
results after leave or frame changes. Exposure, gamma, channel display, and background never change
its reference values. Allocation limits, progress, diagnostics, and shutdown use the existing task
and evaluation contracts.

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

## Operation memoization

The session's `CpuCompositionEvaluator` owns a synchronized, byte-bounded LRU shared by its
foreground, RAM-preview and background requests. Solid, Text, post-transform Layer Output, Merge,
Composition Output and individual value-graph operations retain successful results. Image hits
share immutable pixel storage; they do not copy or rerender it. Absent trimmed-layer images can
also be retained. Failed value operations rerun so their diagnostics are reproduced. Cancelled or
failed image operations are never inserted; complete earlier operations remain reusable.

An operation address includes its plan revision and resolved content. The content key includes
project/composition and source-node identity, operation kind, its own resolved pixel-affecting
operands, composition format, resolution/proxy, and an optional ROI. Merge keys include input order
and whether a slot applies layer blending. Downstream keys include the SHA-256 content hashes of their inputs.
Exact time participates only for time-dependent operations. Values use their kernel options,
resolved operands and frame rate; these results are independent of image resolution. Keys encode
floating-point bits, including signed zero, without rounding or locale-dependent text.

A second lookup by resolved content adopts an unchanged operation into a new revision. Only the
latest revision address is retained, so editing an unrelated node neither discards sibling images
nor grows an alias table. An edited solid color invalidates that solid, its transformed Layer
Output, Merge and Composition Output; the text branch remains reusable. There is no revision-wide
operation-cache flush. Composition-format or proxy changes produce distinct image keys.

Compilation derives transitive time-dependence metadata for both programs. Curves and Time nodes
are dependent, as are operations reached through their value outputs, drivers or image inputs.
Static operations and a Merge of static inputs are invariant. Layer visibility additionally enters
its resolved key: crossing either half-open trim boundary cannot reuse the opposite visibility
state. This metadata is derived runtime state and does not change serialized plan identity inputs.

`playback/operation-cache-bytes` is the operation allocation from the session's one
`MemoryBudgetLedger`, which also allocates `playback/ram-preview-memory-bytes`.

The ledger computes three numbers. The **host reserve** is what Bloom leaves to the rest of the
machine: `max(8 GiB, 40% of physical)`. The **usable budget** is physical memory minus that reserve,
and is the ceiling an explicit override may reach. The **default total** is the ceiling the
unconfigured 60% operation / 40% preview split may reach, and is the most conservative of three
independent limits: the usable budget, 50% of physical memory, and 80% of `availableMemory` at
startup. On Linux `availableMemory` is `MemAvailable` from `/proc/meminfo`, which is the kernel's own
estimate of what can be allocated without swapping; where that file cannot be read it falls back to
`sysconf(_SC_AVPHYS_PAGES)`, and on Windows to `MEMORYSTATUSEX::ullAvailPhys`. A platform that
reports nothing (0) simply does not apply the availability limit.

No rule may push a budget below the 3 GiB low-memory floor (2 GiB preview + 1 GiB operation), and no
rule may invent more budget than the machine reports. The resulting defaults:

| Physical | Reserve | Usable | Default total | Operation | Preview |
| --- | --- | --- | --- | --- | --- |
| 8 GiB | 8 GiB | 3 GiB (floor) | 3 GiB (floor) | 1 GiB | 2 GiB |
| 16 GiB | 8 GiB | 8 GiB | 8 GiB | 4.8 GiB | 3.2 GiB |
| 32 GiB | 12.8 GiB | 19.2 GiB | 16 GiB | 9.6 GiB | 6.4 GiB |
| 60 GiB | 24 GiB | 36 GiB | 30 GiB | 18 GiB | 12 GiB |

The rule exists because the previous one -- reserve `max(4 GiB, 25%)`, then split all of it --
allocated 27 GiB operation + 18 GiB preview on a 60 GiB workstation. One application may not
legitimately plan to hold 45 of 60 GiB while the kernel, the compositor and a browser need the rest
and the swap is 3 GiB.

Missing, invalid, or zero settings use the default split; one override keeps its exact value where
possible, clamped to the **usable** budget rather than the default total, and reduces the other
cache; two overcommitted overrides are proportionally clamped. An artist who deliberately asks for
more than Bloom would choose still gets it, up to the point where the machine itself would starve.
The preview allocation retains its 2 GiB floor when the default total allows it. The effective pair,
not the raw settings, is what the window status bar reports. If physical memory is unavailable or
too small to leave the reserve, the 3 GiB floor is used without exceeding reported physical memory.

### Memory pressure response

Budgets are a plan; they are not a promise the machine will keep. The window status bar polls
`availableMemoryBytes()` on its existing five-second disk-cache cadence. When `MemAvailable` falls
below the ledger's reserve, both in-memory caches are trimmed to 50% of their budgets --
`OperationCache::trimToBytes()` and `PreviewFrameCache::trimToBytes()`, neither of which changes the
configured budget, so the caches refill once the machine recovers -- and the bar shows the transient
notice "Memory pressure: caches trimmed". The trim is not repeated while pressure persists; it is
armed again once availability recovers above the reserve, so a machine that stays busy does not
produce a message every five seconds.

Separately, and independent of any poll, a cache insert never takes a process past a failed
allocation. `std::bad_alloc` raised while retaining an entry -- in `OperationCache::store()` or
`PreviewFrameCache::insert()` -- unwinds the partial insert, counts it (`allocationFailures`), and
returns. Caching is an optimization: the value the caller produced is untouched and the frame still
renders.

### Cache byte accounting

Every cache in the process counts its bytes against a budget or a hard cap. The accounting surface
is test- and diagnostic-only; none of it is UI.

| Pool | Bound | Counter |
| --- | --- | --- |
| `OperationCache`, operation entries | operation budget | `retainedBytes(Operation)` |
| `OperationCache`, decoded media entries | operation budget (shared) | `retainedBytes(DecodedMedia)` |
| `PreviewFrameCache` display buffers | preview budget | `residentBytes()` |
| `MediaDiskCache` pending async writes | 64 entries **and** 256 MiB | `statistics().asyncQueueBytes`, `peakAsyncQueueBytes` |
| `AssetController` proxy thumbnails | 512 entries **and** 8 MiB | `proxyCacheBytes()` |
| `AssetController` decoded audio buffers | 2 GiB aggregate | `decodedAudioBytes()` |
| Viewer display buffers | one retained frame plus at most one channel-remap copy, per open viewer | -- |

The disk cache's pending-write queue is the pool that mattered. It was bounded at 64 entries and
never at bytes, and a queued write owns the last reference to a decoded Float32 image: 64 pending
4K RGBA32F frames are 7.9 GiB and 64 pending 8K frames are 31.6 GiB of process memory no budget ever
saw. It is now bounded by bytes as well, and the byte bound is the binding one -- a write whose
payload alone exceeds the capacity is refused at `storeAsync()` rather than staged and then
discarded by the writer thread. A refused write is counted in `droppedAsyncWrites` and only means
the next read decodes again; it can never mean incorrect pixels.

The shared cache accounts retained image/value storage and entry/key overhead, evicts by
least-recently-used last use, and refuses an entry larger than the budget without evicting useful
entries for it. Runtime `setByteBudget(0)` disables retention and releases entries. Images already
held by an active evaluation or published frame live until those owners release them; eviction does
not invalidate readers. Shared image storage is conservatively charged for every retaining entry.
Concurrent misses may independently evaluate the same operation; cache access is synchronized, but
row kernels never execute under its mutex.

Decoded media is stored in the same evaluator-session cache with a revision-independent address.
Because a burst of new revision-qualified operation entries otherwise makes cold media the oldest
entry, a decoded entry receives an eight-cache-access grace period. Unprotected operation entries
are evicted first during that grace; if every entry is protected, the byte budget still wins.
This small priority rule keeps a recently decoded source available across preview and export work
without making decoded media immortal.

The existing display-frame cache remains the upper level. A frame hit needs no evaluator work;
a miss consults operation memoization before running kernels. Only display buffers count against
the frame-cache budget; retained process images count against the separate operation budget.
`ProcessFrame::operationCacheStatistics()` exposes that request's hits, misses and evaluated node
IDs for later status-bar use, without introducing UI. Preflight validation, request pixel-storage
limits, cancellation, progress and row-band execution still apply on cache hits. Memoization
changes no pixels, identity goldens or evaluator/primitive semantics versions.

Interactive override plans and requests bypass both lookup and insertion, including value results.
Their temporary gesture values cannot populate the operation cache. `bypassOperationCache` also
provides a serial uncached reference for verification. Caching is process-local; nothing is persisted
in the project or on disk.

Evaluated operation geometry is retained beside cached display pixels. The frame-cache budget
counts both payloads; releasing the Float32 process image leaves bounds queries and overlays available
on playback cache hits without evaluation.

### Current Compiled Bounds Contract

Compiled Solid dimensions, Text layout operands and Shape geometry operands are required. Shape
bounds include the enabled fill and stroke. Layer transforms always place the
local-bounds anchor at the authored position, and Merge always unions its inputs' bounds. Plans
carry no historical evaluation selector. Unsupported document node versions are rejected before
compilation. Current identity uses plan semantics 5, animation sampling 2, evaluator 8, and render
primitives 7. SHAPE-1 adds shape pixels and TEXT-2 adds box-text pixels, both while preserving all
existing source pixel goldens.

## Transformed vector coverage

Image primitive semantics 7 adds affine path coverage and glyph outlines. Curves flatten with an
output-space error bound; stroke outlines are built before the affine map. Compound contours use
nonzero winding, and native text-box clipping is transformed with the glyphs. The original native
text coverage routine and its byte goldens remain unchanged. CPU evaluator semantics 8 selects
this coverage for transformed vector-only Layer Output chains and retains bilinear sampling at
raster boundaries.

## Text layout query

`render::layoutText(font, content, parameters, options)` returns a checked, Qt-free `TextLayout`:
line indices, half-open source UTF-8 byte ranges, text-space line boxes, glyph line/byte positions
and advance rectangles, plus caret height. It uses the same pen placements as native coverage and
transformed glyph outlines, including kerning, letter spacing, horizontal alignment, point anchors,
word wrapping, snapped line rows and vertical box alignment. Advance rectangles include the gap to
the next glyph's pen; their widths sum to the line advance. Empty content retains one empty line
and a caret at the text anchor. Explicit empty lines retain their source ranges.

Wrapping retains original byte positions even where its established whitespace normalization omits
source whitespace or inserts a soft break. Consumers convert source bytes to their input toolkit's
cursor units; Qt UTF-16 positions never enter the render API. The query returns logical advance
geometry without allocating a coverage bitmap, and shares validation and cancellation with the
placement path. It changes no rendering semantics, schema versions or existing pixel goldens.
