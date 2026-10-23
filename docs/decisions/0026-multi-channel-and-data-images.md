# ADR 0026: Multi-Channel And Data Images

Status: accepted (owner 2026-10-23, issue #363)

Date: 2026-10-23

## Context

Bloom's only image transport is the canonical process image: premultiplied Float32 RGBA with the
frame's working-space colour identity (`evaluation-primitives.md`, "Colour And Alpha Contract").
The OpenEXR reader accepts one part with R/G/B/A or Y/A channels and rejects everything else
(`exr_backend.cpp`). VFX plates and renders routinely carry several layers in one file or in
several parts: beauty plus AOVs (diffuse, specular), data passes (depth, normals, motion vectors,
position, UVs, object and Cryptomatte IDs) and arbitrary named channels.

Data channels cannot pass through the process image unchanged:

- input colour conversion would alter them;
- premultiplication and the "alpha zero makes RGB zero" canonicalization destroy them;
- bilinear filtering corrupts IDs;
- Cryptomatte stores 32-bit hashes as float bit patterns that must survive bit for bit.

`evaluation-primitives.md` already states the rule that "Depth, normals, motion vectors, UVs, object
IDs, and arbitrary data channels are not color", but no transport exists for them.

This record defines the descriptor, packing, colour encoding, cache identity, GPU representation
and migration for multi-channel images. It is a prerequisite for issue #364 (Cryptomatte).

## Decision

### 1. A second image transport: the channel set

- `render::ChannelSetImage` is a new immutable image type beside `Rgba32fImage`.
- It holds N named channels (N ≥ 1, bounded by the same pixel and byte budgets) that share one data
  window, display window and pixel aspect.
- Each channel has a `ChannelDescriptor`:
  - **full name:** the EXR channel name, e.g. `diffuse.R`, `depth.Z` or `CryptoObject00.r`;
  - **layer:** the name up to the last `.`; the root layer is empty;
  - **role:** `ColorR/G/B`, `Alpha`, `Luminance`, `Depth`, `Normal{X,Y,Z}`, `Motion{U,V}`,
    `Position{X,Y,Z}`, `Uv{U,V}`, `Id`, `Coverage` or `Data`;
  - **source sample kind:** `half`, `float` or `uint`;
  - **value encoding:** `Colour` (with the layer's input colour space) or `Data` (raw numbers).
- The channel set also carries the part's string metadata (OpenEXR header attributes such as
  `cryptomatte/<hash>/manifest`) as an immutable, bounded key/value map. Metadata is never
  interpreted by the transport itself.
- A new socket kind, `SocketValueKind::ChannelSet`, carries it. There is no implicit conversion
  between `ChannelSet` and `Image` in either direction. Conversion happens only in the explicit
  operations in §4 and through the Image source's colour output (§3).
- Masks remain a separate kind; a channel set is not a mask. A ChannelSet → Mask conversion uses
  the Mask kind owned by lane timeline-masking (#360); the interface is agreed with that lane
  before the Cryptomatte node lands.

### 2. Packing

- **CPU.** Each channel is a planar Float32 array over the shared data window.
  - `half` samples widen exactly to Float32.
  - `uint` samples are stored bit-exact in the 32-bit lane and keep their `uint` kind.
  - Values are never clamped or canonicalized: negatives, values above 1, NaN in data channels and
    ID bit patterns are all preserved.
  - Planar storage keeps per-channel copies O(channel) and lets a selection retain only the
    channels it needs.
- **Admission.** Budget admission charges `channels × pixels × 4` bytes through the existing pixel
  and host-retention ledgers. A selection that names fewer channels decodes only those channels.

### 3. Image source selection, colour encoding and the decode path

- **New parameters.** Image source version 2 (`bloom.image-source` v2) adds two String parameters:
  - `part`: an EXR part name; empty means the first part;
  - `layer`: a layer name; empty means the root layer.
  It also gains a second output, `channels` (ChannelSet), carrying every channel of the selected
  part.
- **Colour output.** The existing `image` output is derived from the selected layer:
  - `<layer>.R/G/B[/A]` or `<layer>.Y[/A]` when present, decoded through the existing colour path:
    input colour space, premultiplication policy (#362 alpha-association inheritance), process image.
  - A layer without colour channels (for example only `depth.Z`) gives no colour output; the
    `image` output reports a typed diagnostic and stays unavailable.
- **Data channels.** Channels whose role is not colour or alpha are decoded raw. They are never
  converted, premultiplied, repaired by the non-finite policy (NaN is kept in data channels) or
  bilinearly filtered.
- **Colour channels inside a channel set** keep their source values plus their declared input
  colour space. They are converted only when an explicit operation asks for colour (§4).
- **Probe.** The probe enumerates parts (name, view, type, windows) and each part's channels and
  layers, and keeps per-part chromaticities. Multipart scanline and single-level tiled parts are
  supported.
- **Still rejected, with typed diagnostics:** deep parts, multi-level tiles and subsampled
  channels (x/y sampling ≠ 1).
- **Sequences.** A failing member no longer aborts a sequence. It is recorded as a per-member
  diagnostic and treated as a gap; this follows the #373 batch policy.
- **Pickers.** Part and layer pickers read the probe through the Assets worker. The asset record
  persists no part or layer list; a selection that no longer exists is diagnosed at evaluation.

### 4. Operations

These are new compiled operations. Each is GPU-required under
`gpu_coverage_contract.hpp`, has a CPU reference, and is bit-exact unless stated otherwise.

| Node | Input → output | Behaviour |
| --- | --- | --- |
| Shuffle | ChannelSet [+ Image] → Image or ChannelSet | Maps named channels or constants (0, 1) to the four output slots or to new names. **To Image:** each mapping declares either `Colour` (convert from the channels' input space to the working space, then premultiply by the mapped alpha) or `Raw` (values enter working-space RGB unchanged, the mapped alpha or 1 is used, and premultiplication follows the process contract; the artist owns the meaning). Raw mapping is never implicit. |
| Channel Copy | ChannelSet × 2 → ChannelSet | Copies named channels from B into A. Windows are unioned and missing samples are explicit zeros. |
| Remove | ChannelSet → ChannelSet | Keeps or removes channels by name, layer or role pattern. |

- Channel-set geometry stays fixed in this record: no layer transform, resample or filter applies
  to a ChannelSet. Spatial work happens after an explicit Shuffle to Image. A later record may add
  point-sampled transforms for data.
- **Viewer inspection.** The Viewer shows any channel or layer of a ChannelSet directly, as raw
  greyscale (one channel) or RGB (three channels or a layer's colour channels), without wiring a
  Shuffle. Artists inspect AOVs, depth, motion and Cryptomatte this way. This is a Viewer display
  choice (session state, like the existing channel display remap): it never changes the project,
  the compiled plan or any render/export result, and it applies no colour conversion or
  premultiplication to Data channels.
- The Image source's proxy resolution applies to channel sets by point sampling with the shared
  PointResampleV1 rule (ADR 0025 / #362), which keeps IDs exact.
- Cryptomatte (#364) is a ChannelSet → Mask/Image operation built on §1–§2. It reads ID/coverage
  rank pairs from bit-exact Float32 lanes and the part metadata map.

### 5. GPU representation

- Channels are packed in groups of up to four into `rgba32f` storage images, in channel-descriptor
  order.
- A host-side channel map (group, lane) travels with the GPU channel set. Kernels address lanes
  through that map; they never derive it per pixel.
- `imageLoad`/`imageStore` preserve every bit (as PointResampleV1 already relies on), so shuffles,
  copies and removals are exact gathers.
- Uploads reuse the rooted, phase-charged upload path, with one upload per group.
- A dedicated single-channel format (`R32_SFLOAT`) is an optional later optimisation behind the
  same descriptor; it does not change semantics.
- Every new operation needs, before it is complete:
  - CPU/GPU parity (bit-exact for gathers; the existing 2e-6 bound for Shuffle `Colour` mappings);
  - invalidation and cache identity;
  - cancellation;
  - budget and retirement behaviour.

### 6. Cache identity

- **Decoded entries.** The decoded-image disk and memory keys add part index and name, the selected
  layer, and the ordered channel names and source sample kinds. The decoder identity becomes
  `bloom-image-decode-4`.
- **Channel-set entries** are keyed by source digest, member frame, part, the ordered channel list
  and proxy scale. Colour interpretation is not part of the key, because channel sets hold source
  values.
- **Shuffle keys** include the full mapping, each mapping's encoding mode, and — for `Colour`
  mappings — the input colour space, working space, config revision and synthesized-space identity
  (#362).
- Compiled-plan semantics and CPU evaluator semantics versions increase by one each.
- **Output identity.** The layered EXR preset (§7) adds the written channel list and types to the
  output semantic identity.

### 7. Output

- The flat OpenEXR export gains a layered variant: the RGBA process image as the root layer, plus
  the channel layers selected on the Composition Output.
- The Composition Output gains an optional `channels` input and a layer selection list.
- Channel layers are written at their source sample kind (half or float), uncompressed, with their
  roles recorded in channel names. The written file is reopened and verified exactly as flat EXR is
  today.

### 8. Migration and persistence

- Project schema minor 1.26 adds a named migration step. Every `bloom.image-source` v1 node
  becomes v2 with `part = ""` and `layer = ""`. Its `image` output keeps its edge, and the new
  `channels` output is unconnected.
- Existing projects therefore evaluate byte-identically; their decoded keys change only through the
  decoder identity bump.
- Saving writes v2. Opening an older minor runs the step. Opening a newer minor follows the
  existing forward-compatibility rules.

### 9. Platforms

- Decode, probe and every CPU reference run identically on Linux, macOS and Windows.
- GPU kernels follow the existing native coverage policy. A platform without the native backend
  reports the explicit supported CPU fallback, never an unlabelled CPU frame.

## Consequences

- The process-image contract is unchanged; every existing operation, kernel and fixture keeps its
  semantics.
- Data channels get a typed, loss-free path. Converting data into colour pixels is always an
  explicit, keyed Shuffle decision.
- Memory grows with the number of selected channels; budgets and ledgers charge it explicitly.
- The ChannelSet socket and image source v2 touch the document schema, node registry, compiler,
  both evaluators, GPU coverage and output. #363 lands them together, with migration fixtures,
  save/reopen tests and multi-layer, multipart and data-channel EXR fixtures.

## Resolved questions (owner 2026-10-23)

1. A data-only layer's `image` output is unavailable and reports a typed diagnostic; it never falls
   back to a raw shuffle.
2. The ChannelSet → Mask boundary uses the Mask kind owned by lane timeline-masking (#360, document
   schema 1.24). The exact interface is agreed with that lane before the Cryptomatte node (#364).
3. Output layers are selected by a per-output layer list on the Composition Output.

The document schema step for this record is minor 1.26 (timeline-masking 1.24, video-io 1.25).

## Related

- [`../architecture/evaluation-primitives.md`](../architecture/evaluation-primitives.md)
- [`../architecture/media-io.md`](../architecture/media-io.md)
- [`../architecture/frame-output.md`](../architecture/frame-output.md)
- [ADR 0025](0025-identity-verified-media-sources.md)
