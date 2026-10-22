# Planned node delivery

Status: source-audited inventory and bounded implementation plan; planned nodes are not completed

Updated: 2026-10-22

Owner: [issue #331](https://github.com/kinetik-gg/bloom/issues/331).

## Verified inventory

At source baseline `45431368`, assembly registers 112 types: 12 image/audio/structural definitions,
34 foundational value/control types and 66 descriptor utilities. This is a source-derived count, not
an executed registry dump or a catalogue completion percentage. The product catalogue mixes node types,
selector operations, recipes, UI surfaces and optional domains.

| Area | Present | Remaining gap |
| --- | --- | --- |
| Sources | Solid, Text, Shape, Image/sequence, Video, Audio, Composition Source | Gradients, Checker/Grid/Bars and procedural image generators |
| Shape | Seven modes, fill/stroke, Viewer drawing and pen editing | Several shape operands are constant-only; path streams/modifiers absent |
| Text | Raster text, font/layout controls, editable Viewer text | Portable shaping, animators and text-on-path |
| Composition | Layer transforms, Layer Stack displayed as Merge, Composition Output, nested composition | Image Mix/matte/alpha operators, standalone transform/canvas nodes |
| Color effects | OCIO Colour Space Transform and File Transform | Ordinary grading, filter, keying and matte effects |
| Values | Constants, math, vectors, switches, time, random, conversions and strings | Exposed advanced scalar/matrix/motion functions; do not mistake these for image effects |
| Data readers | Data Sample, Ramp Sample, Table Lookup and Point Set Read backend/persistence | Named typed picker and ordinary desktop create/edit workflow |
| Graph organization | Reroute and presentation frames | Reusable groups with typed exposed ports |
| Viewer | Session inspection, channels, EV/gamma and pixel probe | Extend the existing service where useful, rather than duplicate render truth |

Registry evidence: `src/document/node_definition_registry.cpp`, `value_node_definitions.cpp`,
`value_utility_nodes.cpp`. The closed operation/effect variants in
`src/runtime/include/bloom/runtime/compiled_plan.hpp` distinguish implemented pixel operations from
catalogue proposals. Existing tests were inspected/located; their presence is not a fresh passing gate.

Before expansion, complete [existing correctness and workflow repairs](user-control-policy-audit.md),
including Add-search eligibility, exact numeric edits and end-to-end color intent. Data-block authoring
and missing shape parameter animation should be bounded follow-up slices rather than hidden prerequisites.

## Ordered chunks

### 1. [#332: Pointwise image effects: Exposure, Offset, Multiply, Invert](https://github.com/kinetik-gg/bloom/issues/332)

undefined

Independent foundation; schedule after existing correctness fixes.

### 2. [#333: Image generators: Linear Gradient, Radial Gradient, Checker](https://github.com/kinetik-gg/bloom/issues/333)

undefined

Depends on #332.

Bou

### 3. [#334: Image Mix, Set Alpha and Multiply Alpha](https://github.com/kinetik-gg/bloom/issues/334)

undefined

Depends on #332.

Bou

### 4. [#335: Canvas operations: Crop, Pad, Flip and Rotate 90](https://github.com/kinetik-gg/bloom/issues/335)

undefined

Independent foundation; schedule after existing correctness fixes.

### 5. [#336: Resize, Fit and standalone Transform 2D](https://github.com/kinetik-gg/bloom/issues/336)

undefined

Depends on #335.

Bou

### 6. [#337: Gaussian Blur, Box Blur and Matte Blur](https://github.com/kinetik-gg/bloom/issues/337)

undefined

Depends on #334.

Bou

### 7. [#338: Luma Key, Matte Erode and Matte Dilate](https://github.com/kinetik-gg/bloom/issues/338)

undefined

Depends on #334, #337.

Bou

### 8. [#339: Drop Shadow, Glow and Light Wrap recipes](https://github.com/kinetik-gg/bloom/issues/339)

undefined

Depends on #337, #338.

Bou

### 9. [#340: Levels, Curves and image Hue/Saturation](https://github.com/kinetik-gg/bloom/issues/340)

undefined

Depends on #332.

Bou

### 10. [#341: Scalar Sine, Cosine and Power; separate deterministic motion utilities](https://github.com/kinetik-gg/bloom/issues/341)

undefined

Independent foundation; schedule after existing correctness fixes.

### 11. [#342: Reusable Group Input, Group Output and Node Group](https://github.com/kinetik-gg/bloom/issues/342)

undefined

Independent foundation; schedule after existing correctness fixes.

### 12. [#343: Static SVG Vector Asset import and Vector Source](https://github.com/kinetik-gg/bloom/issues/343)

undefined

Independent foundation; schedule after existing correctness fixes.

### 13. [#344: Path processing: Transform Path, Trim Path and Offset Path](https://github.com/kinetik-gg/bloom/issues/344)

undefined

Depends on #343.

Bou

### 14. [#345: Shaped Text Layout, Text Animator and Text On Path](https://github.com/kinetik-gg/bloom/issues/345)

undefined

Depends on #344.

Bou

### 15. [#346: Image Time Offset, Frame Hold and Time Remap](https://github.com/kinetik-gg/bloom/issues/346)

undefined

Independent foundation; schedule after existing correctness fixes.

### 16. [#347: Audio Amplitude, Audio Waveform and Audio Spectrum](https://github.com/kinetik-gg/bloom/issues/347)

undefined

Independent foundation; schedule after existing correctness fixes.

### 17. [#348: Corner Pin, ST Map and Displace](https://github.com/kinetik-gg/bloom/issues/348)

undefined

Depends on #336.

Bou

### 18. [#349: Histogram, Image Statistics and Compare](https://github.com/kinetik-gg/bloom/issues/349)

undefined

Depends on #332.

Bou

## Workflow milestones

First artist-facing milestone combines pointwise grades, gradients, flat-RGBA masking and separable
blur in one saved animated composition. Shadow/Glow then reuse those primitives. Keep each assignment
to its named 3–5 nodes/capabilities; a new framework or adjacent family requires a separate chunk.

Every image-producing node needs schema/discoverable controls, commands/undo/save-reopen, CPU reference
semantics for alpha/HDR/negative values/windows/ROI, actual production GPU parity across preview and
final routes, source/color/time/quality cache identity, cancellation, budget/retirement behavior and
cold/warm RAM-preview evidence. An unavailable device reports explicit Unsupported/CPU fallback or
NotRun; a CPU-rasterized upload is not GPU pixel implementation. Preserve equivalent platform semantics
or an explicit supported fallback. Group expensive checks locally at these milestones, never per node.

Advanced temporal/flow, arbitrary-channel/AOV/deep, tracking/solving, 3D, particles and general audio
remain later families with prerequisites. Their catalogue presence does not make them quick additions.

