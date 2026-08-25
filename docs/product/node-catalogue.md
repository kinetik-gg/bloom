# Node Catalogue

Status: working

Updated: 2026-08-25

## Purpose

This catalogue defines the node families Bloom intends to support. It is a product map, not a
promise that every listed node belongs in the first release. Implementation follows complete artist
workflows: a smaller coherent compositor is more useful than a large catalogue of disconnected
nodes.

Bloom has one canonical composition graph. The Nodes editor exposes that graph directly, while the
Timeline, Properties, and Viewer expose structured layer-friendly projections of the same nodes and
parameters. A feature available through a layer control and a node uses the same document identity,
primitive semantics, runtime operation, and render path.

## Delivery Levels

| Level | Meaning |
| --- | --- |
| `F0` | Foundation already represented in the document or runtime |
| `F1` | Rendering foundation required before the broad effect catalogue |
| `C1` | Essential compositing vocabulary |
| `M1` | Motion-design vocabulary built after the compositor is sound |
| `V1` | Advanced VFX, analysis, or temporally expensive work |
| `X1` | Optional pipeline domain; designed for modular contribution rather than core scope |

Levels express dependency and product importance, not a rigid release number. Nodes may enter
earlier when required by an end-to-end workflow, but they must not bypass the contracts of their
level's dependencies.

## Foundation And Graph Structure

| Level | Planned nodes | Purpose |
| --- | --- | --- |
| `F0` | Solid Source | Deterministic finite RGBA source with explicit color-encoding identity |
| `F0` | Text Source | Authorable text source; evaluation waits for portable font identity, shaping, and rasterization |
| `F0` | Layer Output | Explicit boundary that makes a graph result layer-addressable |
| `F0` | Layer Stack | Stable ordered compositing inputs projected as Timeline rows |
| `F0` | Composition Output | One explicit evaluation endpoint per composition |
| `C1` | Group Input, Group Output, Node Group | Reusable graph encapsulation with typed exposed ports |
| `C1` | Reroute, Frame | Graph organization with no hidden evaluation behavior |
| `C1` | Switch, Bypass | Explicit conditional or pass-through evaluation |
| `C1` | Viewer Output | Non-rendering inspection endpoint that never changes project output |
| `M1` | Nested Composition | Composition instance with explicit time mapping and overrides |

## Sources And Generators

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `F1` | Image, Image Sequence | Stable asset identity, source interpretation, windows, channels, and missing-frame policy |
| `C1` | Video Clip | Explicit stream, time base, orientation, color, alpha, and decode policy |
| `C1` | Shape | Resolution-independent path and fill/stroke source |
| `C1` | Linear Gradient, Radial Gradient, Four-Point Gradient | Explicit interpolation space, repeat mode, and alpha behavior |
| `C1` | Checker, Grid, Color Bars | Deterministic diagnostic and design generators |
| `M1` | Noise, Fractal Noise, Voronoi | Seeded, versioned procedural output with stable CPU/GPU semantics |
| `M1` | Repeater, Scatter | Instance-like repetition without eagerly duplicating authoring data |
| `M1` | Audio Spectrum, Audio Waveform | Deterministic audio-analysis-driven image generation |

## Transform, Layout, And Resampling

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `F1` | Transform 2D | Position, anchor, scale, rotation, opacity, defined sampling, and transparent borders |
| `F1` | Resize, Fit | Explicit output extent, pixel aspect, filter, and fit/fill policy |
| `C1` | Crop, Pad, Reformat | Preserve and deliberately change data/display windows and canvas semantics |
| `C1` | Flip, Mirror, Rotate 90 | Exact discrete transforms where possible |
| `C1` | Tile, Repeat | Defined edge and repeat modes |
| `C1` | Corner Pin, Perspective Transform | Checked projective mapping with explicit invalid-transform diagnostics |
| `M1` | Align, Distribute, Auto-Orient | Layout operations over stable bounds and transform identities |
| `V1` | ST Map, Displace, Vector Warp | Declared coordinate convention, channel roles, scale, filtering, and border policy |
| `V1` | Grid Warp, Mesh Warp, Spline Warp | Editable deformation domains with bounded sampling |
| `V1` | Lens Distortion, Turbulent Displace, Twirl, Bulge | Explicit model, units, sampling, and region-of-interest expansion |

## Compositing, Channels, And Alpha

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `F1` | Over | Reference premultiplied source-over operation |
| `F1` | Mix | Controlled interpolation between images with explicit mask and factor policy |
| `C1` | Merge | Named compositing operator and blend mode with declared alpha equation |
| `C1` | Matte | Explicit image-derived coverage, channel, inversion, space, and time policy |
| `C1` | Set Alpha, Replace Alpha, Multiply Alpha | Alpha changes remain distinct from RGB color transforms |
| `C1` | Premultiply, Unpremultiply | Explicit association conversion with defined zero-alpha behavior |
| `C1` | Channel Split, Channel Combine | Typed construction and extraction of color, alpha, and data channels |
| `C1` | Channel Copy, Channel Shuffle, Remove Channels | Preserve arbitrary channels and never infer color meaning from position alone |
| `C1` | Holdout | Explicit coverage removal with a documented alpha equation |
| `V1` | Deep Merge, Deep Holdout, Deep To Image | Optional deep-image path; never implied by flat RGBA support |
| `V1` | Depth Composite | Explicit depth units, ordering, invalid-data behavior, and antialiasing policy |

## Color And Grade

Color nodes never treat RGBA as an unlabelled four-component vector. Each operation declares its
required processing domain, preserves HDR and negative RGB unless its purpose says otherwise, and
keeps display transforms out of scene/process pixels.

| Level | Planned nodes | Processing contract |
| --- | --- | --- |
| `C1` | Color Space Transform | Explicit source and destination spaces resolved through project color management |
| `C1` | Exposure | Scene-linear light scaling |
| `C1` | Offset, Multiply | Declared scene/process space; no implicit clamp |
| `C1` | Lift/Gamma/Gain | Explicit grading model and working space |
| `C1` | Contrast, Pivot | Declared pivot and processing space |
| `C1` | Levels | Explicit black/white points, gamma, channel policy, and optional clamp |
| `C1` | Curves | Versioned interpolation with explicit RGB, channel, or luminance behavior |
| `C1` | Hue/Saturation, Vibrance | Declared perceptual model and working space |
| `C1` | White Balance | Declared chromatic-adaptation model and working space |
| `C1` | Invert | Separate color, alpha, and mask modes |
| `C1` | Luminance | Declared coefficients and source encoding; produces scalar or mask data |
| `V1` | Gamut Compress, Tone Map | Explicit rendering model; never an automatic hidden repair |
| `V1` | Color Match | Analysis plus an inspectable, reproducible transform result |
| `V1` | Display/View Transform | Presentation-only operation with display, view, look, context, and cache identity |

## Filters, Detail, And Light

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `C1` | Gaussian Blur, Box Blur | Explicit radius units, edge mode, alpha policy, and region expansion |
| `C1` | Directional Blur, Radial Blur | Stable spatial convention and bounded sampling |
| `C1` | Sharpen, Unsharp Mask | Defined kernel and HDR behavior |
| `C1` | Dilate/Erode, Edge Detect | Separate color-image and mask/data modes |
| `M1` | Glow, Bloom | Scene-linear light behavior with explicit threshold and compositing policy |
| `M1` | Drop Shadow, Inner Shadow | Reusable matte, blur, offset, and composite primitives |
| `V1` | Median, Bilateral, Denoise | Neighborhood/global scheduling class and quality controls |
| `V1` | Defocus, Bokeh Blur | Lens model, depth interpretation, sampling quality, and halo requirements |
| `V1` | Convolution | Validated kernel size, normalization, edge, and alpha policy |
| `V1` | Glare, Light Rays, Lens Flare | HDR-aware light effects with declared processing space |

## Keying, Masks, And Matte Refinement

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `C1` | Luma Key, Chroma Key | Declared input color space and explicit matte output |
| `C1` | Difference Key | Reference image alignment and color-space agreement |
| `C1` | Despill | Separate color correction from matte generation |
| `C1` | Matte Choker, Erode/Dilate Matte | Coverage-domain processing rather than color processing |
| `C1` | Matte Blur, Edge Blur | Explicit mask filtering and edge behavior |
| `C1` | Set Matte | Channel, inversion, transform, and time relationship are visible inputs |
| `M1` | Bezier Mask, Rectangle Mask, Ellipse Mask | Stable paths, feather, expansion, fill rule, and motion controls |
| `V1` | Advanced Keyer | Inspectable intermediate mattes and deterministic quality modes |
| `V1` | Edge Extend, Edge Color | Defined RGB reconstruction around transparency |

## Time, Motion, And Feedback

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `M1` | Time Offset, Frame Hold | Exact rational time mapping |
| `M1` | Time Remap | Explicit curve, interpolation, frame-boundary, and missing-frame policy |
| `M1` | Echo, Trails | Bounded temporal window and cache identity |
| `M1` | Motion Blur | Declared shutter interval, sample distribution, and transform source |
| `V1` | Frame Blend | Explicit sample count and blend weights |
| `V1` | Optical Flow, Vector Motion Blur | Flow units, direction, confidence, occlusion, and quality mode |
| `V1` | Temporal Denoise | Bounded history, scene-cut behavior, and deterministic fallback |
| `V1` | Delay/Feedback | Only legal graph feedback boundary; owns history and invalidation semantics |

## Tracking, Analysis, And Utility

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `M1` | Sample Image, Bounds | Typed data outputs with explicit coordinate space |
| `M1` | Histogram, Image Statistics | Reduction operations with declared channel and color domain |
| `V1` | Point Track, Planar Track | Analysis result is durable, inspectable data; solving is cancellable work |
| `V1` | Stabilize, Match Move | Applies explicit tracking data through ordinary transform primitives |
| `V1` | Camera Solve | Optional 3D result with diagnostics and uncertainty, not an opaque side effect |
| `V1` | Compare, Difference, Wipe | Validation and inspection nodes that do not alter final output unless connected |

## Values, Math, And Control

Artist nodes in this category lower to checked reusable primitives. Scalar, vector, matrix, color,
mask, and geometry values remain distinct even where their storage shapes resemble one another.

| Level | Planned nodes | Primitive families |
| --- | --- | --- |
| `F0` | Add, Subtract, Multiply, Divide, Multiply Add, Minimum, Maximum, Clamp, Remap, Mix | Checked Float32/Float64 scalar primitives are implemented; artist node schemas are not yet exposed |
| `C1` | Absolute, Negate, Sign, Reciprocal, Square Root, Power, Logarithm, Exponential | The first five checked scalar primitives are implemented; transcendental primitives and artist schemas remain deferred |
| `C1` | Floor, Ceiling, Round, Truncate, Fraction, Modulo, Wrap, Snap | The first six checked scalar primitives are implemented; Wrap, Snap, and artist schemas remain deferred |
| `C1` | Step, Smoothstep, Smootherstep | Checked interpolation primitives are implemented; artist schemas are not yet exposed |
| `C1` | Equal, Near, Less, Greater, Compare, Select | Typed comparison and control primitives; floating tolerance is explicit |
| `C1` | Boolean And, Or, Not, Exclusive Or | Boolean primitives, not numeric truthiness |
| `C1` | Vector Compose/Split, Add, Scale, Dot, Cross, Length, Normalize, Distance | Checked vector primitives with dimension-specific schemas |
| `C1` | Transform Compose/Decompose, Matrix Multiply, Inverse, Transform Point/Vector | Checked affine/projective primitives with explicit coordinate meaning |
| `M1` | Sine, Cosine, Tangent, Arc Tangent 2 | Explicit angle units and domain diagnostics |
| `M1` | Random, Hash, Noise | Stable algorithm identity, seed, and semantics version |
| `M1` | Color Mix, Color Ramp | Color-aware interpolation with an explicit processing space and alpha policy |

## Shape And Text Processing

| Level | Planned nodes | Important behavior |
| --- | --- | --- |
| `M1` | Rectangle, Ellipse, Polygon, Star, Path | Stable parametric or authored vector geometry |
| `M1` | Fill, Stroke, Gradient Fill, Gradient Stroke | Color-managed paint with explicit stroke and interpolation rules |
| `M1` | Transform Path, Offset Path, Trim Path | Path-domain operations independent of raster resolution |
| `M1` | Merge Paths, Boolean Paths | Declared fill rule and robust topology diagnostics |
| `M1` | Repeater, Wiggle Path, Round Corners | Non-destructive procedural path modifiers |
| `M1` | Text Layout, Text On Path | Portable font asset, shaping, language, direction, and layout contracts |
| `M1` | Text Animator | Selector-driven per-glyph properties over stable text-layout results |
| `V1` | Vectorize Image | Analysis operation with inspectable thresholds and topology output |

## Optional Pipeline Domains

These families validate Bloom's modular architecture but do not expand the core compositing scope.
They should arrive as coherent modules when a real workflow requires them.

| Level | Domain | Candidate node families |
| --- | --- | --- |
| `X1` | Geometry | Mesh/curve/point primitives, transform, selection fields, attributes, topology, instances, realization, sampling, and geometry output |
| `X1` | 3D Scene | Scene input, object/collection query, camera, light, transform hierarchy, render, depth, normal, motion, and ID outputs |
| `X1` | Materials | Typed material inputs, closures, texture sampling, coordinate transforms, and material output through adopted interchange contracts |
| `X1` | Particles | Emitters, forces, simulation zones, attributes, instancing, caching, and render conversion |
| `X1` | Audio | Decode, mix, filter, envelope, analysis, resample, and output nodes with realtime-safe execution classes |
| `X1` | Pipeline | Asset query, validation, publish, conversion, build, and external-process tasks with explicit trust and cancellation boundaries |

## Catalogue Rules

- A node has a stable namespaced type ID, schema version, typed ports, parameter roles, diagnostics,
  execution class, and lowering recipe.
- Similar UI does not justify duplicate math. Nodes compose shared checked primitives and domain
  kernels.
- Nodes declare whether they are uniform, pointwise, neighborhood, warp/gather, reduction, global,
  temporal, analysis, or side-effecting task operations. This drives tiling, region-of-interest,
  caching, scheduling, and GPU eligibility.
- Image ports carry semantic channel roles, color-encoding identity where applicable, alpha
  association, data/display windows, pixel aspect, and coordinate conventions. Data, masks, depth,
  normals, motion, UVs, and IDs are not color.
- Unsupported nodes remain preservable and diagnosable. Bloom never silently substitutes a
  different effect, quality level, color path, or alpha equation.
- CPU reference behavior defines correctness. GPU implementations declare supported semantics and
  prove parity within per-operation tolerances.
- Add-ons and optional modules register through the same node-definition and evaluator boundaries;
  they do not mutate the built-in catalogue or bypass commands, snapshots, tasks, and diagnostics.
