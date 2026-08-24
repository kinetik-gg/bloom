# Open Standards And Interchange Strategy

Status: working

Updated: 2026-08-25

## Accepted Policy

Bloom is standards-first. Before defining a file format, interchange schema, color convention,
plug-in ABI, or pipeline protocol, investigate the relevant open industry specification and its
maintained implementations. Adopt the existing contract when it can represent Bloom's semantics
without compromising project correctness.

Standards-first does not mean standards-only:

- Bloom still needs a native, schema-versioned project format for authoring state that no interchange
  standard represents.
- A standard may be incomplete, ambiguous, unsafe for untrusted input, encumbered in some markets,
  or unsuitable for an interactive runtime.
- Supporting a standard does not require exposing every optional feature in the first release.
- Implementing a deferred product area early merely because a standard exists is not useful.

Any custom interchange contract requires an ADR that names the standards evaluated, the concrete
gap, the compatibility cost, and a migration or extension strategy.

## Standard, Format, And Library Are Different Claims

Bloom documentation and UI must describe integrations precisely:

| Kind | Meaning | Examples |
| --- | --- | --- |
| Specification or interchange contract | Defines semantics that independent applications may implement | OpenEXR file format, OpenFX API, MaterialX specification |
| Standardized workflow or encoding | Defines color, timing, or production meaning, sometimes through multiple published documents | ACES and related SMPTE specifications |
| Open-source implementation and schema | Ships code plus an ecosystem interchange contract | OpenColorIO, OpenTimelineIO, OpenUSD |
| Implementation library | Helps Bloom read, write, decode, or process data but does not itself make every result interoperable | OpenImageIO, FFmpeg, Qt |
| Compatibility baseline | Coordinates dependency versions; it is not an interchange format | VFX Reference Platform |

Using a library is not evidence of conformance to every format it can access. Conversely, Bloom may
implement or use a standard through a different conforming library. Product claims must name the
supported specification/version, feature subset, and tested limitations.

## Adoption Map

The map records intent, not immediate scope. Each integration enters the roadmap only when its
owning workflow is active.

| Domain | Contract or implementation | Bloom direction | Timing |
| --- | --- | --- | --- |
| Color management | [OpenColorIO][ocio] (OCIO) configuration and C++ implementation | Foundational choice. Use project-selected configurations for file interpretation, working transforms, and display/view transforms. Do not hard-code one studio's color pipeline. | Required before claiming a production color-managed viewer or render path |
| Color workflow | [ACES][aces] encodings and transforms | Support through validated OCIO configurations as an option. ACES is not forced as every project's working space. | Alongside production color management |
| HDR/VFX images | [OpenEXR][openexr] format and reference libraries | Primary high-dynamic-range image interchange. Preserve windows, channels, alpha convention, pixel aspect, compression intent where supported, and relevant attributes. | Early image import/output; flat frames first, advanced multipart/deep features declared separately |
| General image I/O | [OpenImageIO][oiio] (OIIO) | Preferred C++ library candidate for format-agnostic image access, metadata, and processing utilities. OIIO is a library, not Bloom's interchange standard or canonical image model. | Evaluate when still and sequence I/O enters the vertical proof |
| Editorial interchange | [OpenTimelineIO][otio] (OTIO) | Adopt for editorial cut structure and metadata. OTIO references external media rather than embedding audio or video, so media relinking remains a separate responsibility. | When sequence/editorial work enters scope |
| Image-effect plug-ins | [OpenFX][openfx] API | Adopt for third-party image-effect hosting rather than inventing a Bloom-only native effect ABI. Host capability reporting, isolation, threading, color, and failure containment need dedicated design. | Deferred until the internal effect/runtime contract is stable |
| Audio/video containers and codecs | Published container/codec specifications; [FFmpeg][ffmpeg] as an implementation candidate | Treat each container, codec, pixel format, time-base, licensing, and metadata behavior explicitly. FFmpeg is a library suite, not one media standard. | When video/audio ingest and delivery enter scope |
| 3D scene interchange | [OpenUSD][openusd] | Preferred ecosystem to evaluate for future composed 3D scene data; it is not Bloom's 2D composition graph or project format. | Deferred until a concrete 3D workflow exists |
| Materials and looks | [MaterialX][materialx] | Preferred open specification to evaluate for future renderer-independent material/look exchange. Do not map Bloom's 2D node vocabulary onto it artificially. | Deferred with 3D/look-development scope |
| Asset resolution | [OpenAssetIO][openassetio] | Evaluate for future integration with external asset-management systems while retaining stable internal asset identity. | Deferred until managed-pipeline integration is required |
| Build compatibility | [VFX Reference Platform][vfx-platform] | Use as an important Linux/VFX ecosystem compatibility input, especially for C++ and ASWF library alignment. It does not replace Bloom's macOS and Windows dependency policy. | Review during dependency upgrades and release planning |

Additional candidates such as Alembic, OpenVDB, OpenSubdiv, SMPTE timecode, ICC profiles, and
platform media frameworks should be assessed when a concrete feature needs them. Listing a project
here does not commit Bloom to shipping it.

## Color Contract

Color management is a correctness boundary shared by import, viewer presentation, cache identity,
effects, and output.

- Store a stable identifier for the selected OCIO configuration and enough provenance to diagnose a
  missing or changed configuration. Do not rely only on an ambient environment variable.
- Store source color interpretation and alpha association explicitly. File-name rules may propose an
  interpretation, but an artist can inspect and override it.
- Keep scene/process pixels separate from display-referred presentation. A viewer transform never
  bakes into project truth merely because it is visible.
- Include all color-transform inputs that affect pixels in render and cache identity.
- Validate configurations and report missing roles, color spaces, displays, views, LUTs, or context
  variables as actionable diagnostics.
- A `data` channel or source bypasses color transforms by declared intent, not by a filename guess
  hidden from the artist.

OCIO is a complete motion-picture/VFX color-management solution with versioned configuration
syntax; see its [configuration documentation][ocio-config]. ACES defines encodings and a broader
color workflow. They complement one another and must not be described as synonyms.

## Interchange Preservation And Loss Reporting

Importers and exporters are adapters between two models, not simple file dialogs. Every adapter must
define what it can preserve.

### Import

- Validate structure and resource bounds before constructing trusted document state.
- Record source format/version, adapter version, external media references, time-rate interpretation,
  and color interpretation where they affect meaning.
- Preserve stable source metadata and unknown extension data losslessly when the source contract and
  security model make that practical.
- Keep unsupported but non-fatal constructs diagnosable. Do not silently flatten, quantize, clamp,
  retime, drop channels, reassociate alpha, replace fonts/effects, or guess missing media.
- If Bloom cannot edit a construct without loss, prefer a read-only/linked representation or require
  an explicit conversion.

### Export

Before writing, produce a machine-readable and artist-readable interchange report classifying each
relevant construct as:

- preserved exactly
- translated equivalently
- approximated with a stated consequence
- omitted or unsupported
- externally referenced or missing

The report must cover time and rate conversion, color and alpha, image channels and windows, effects,
animation/interpolation, nested structure, media references, and metadata as applicable. A lossy
export requires explicit confirmation unless the chosen preset already documents and accepts that
loss. Final render/export must never silently fall back to a lower quality, different color path, or
missing effect.

Use namespaced, versioned Bloom extensions only when the target standard provides an extension
mechanism. Preserve unknown extensions on read/write where feasible, and never claim portable
interchange for data that only Bloom understands.

### Round-Trip Claims

“Imports,” “exports,” and “round-trips” are separate capabilities. Bloom claims round-trip support
only for a declared feature subset proven by tests. Tests should use upstream conformance material
or official examples where available, adversarial malformed inputs, Bloom-generated fixtures, and
at least one independent reader/writer when practical.

OpenEXR, for example, supports arbitrary channels, extra attributes, tiled and scanline images,
multipart images, and deep data; see the official [technical introduction][openexr-technical]. A
flat RGBA implementation must not present itself as complete OpenEXR support. Likewise, OIIO notes
that writers may drop metadata a destination format cannot represent, reinforcing the need for
Bloom's explicit report; see [OIIO image output][oiio-output].

## Adoption Review

Before accepting a dependency or compatibility claim, record:

1. The artist workflow and interchange boundary it serves.
2. The governing specification, schema, or upstream API and the supported version range.
3. Required and optional features, known semantic gaps, and degradation behavior.
4. Upstream maintenance, governance, release cadence, and security response.
5. License, redistribution obligations, optional components, codec/patent considerations, and
   platform packaging constraints.
6. Linux, macOS, Windows, compiler, architecture, and GPU implications.
7. Threading, cancellation, resource-limit, sandboxing, and untrusted-input behavior.
8. ABI/versioning strategy and how project files remain readable after upgrades.
9. Conformance, round-trip, performance, and independent-implementation tests.
10. Removal or replacement cost if the dependency no longer meets Bloom's needs.

Dependency versions are pinned reproducibly and upgraded intentionally. The current
[VFX Reference Platform][vfx-platform] is an input to that choice, not permission to let the three
target platforms diverge semantically.

## Project Format Boundary

The `.bloom` format is Bloom's durable authoring model, not a new general VFX interchange standard.
It may reference or embed standard payloads where that improves portability, but it must retain the
project semantics needed for deterministic evaluation, commands, animation, and future migration.
Standard import/export remains an explicit boundary with a capability report.

[ocio]: https://opencolorio.org/
[ocio-config]: https://opencolorio.readthedocs.io/en/latest/guides/authoring/overview.html
[aces]: https://docs.acescentral.com/background/overview/
[openexr]: https://openexr.com/en/latest/about.html
[openexr-technical]: https://openexr.com/en/latest/TechnicalIntroduction.html
[oiio]: https://openimageio.readthedocs.io/en/stable/
[oiio-output]: https://openimageio.readthedocs.io/en/stable/imageoutput.html
[otio]: https://opentimelineio.readthedocs.io/en/latest/
[openfx]: https://openfx.readthedocs.io/en/main/
[ffmpeg]: https://www.ffmpeg.org/about.html
[openusd]: https://openusd.org/release/
[materialx]: https://materialx.org/Specification.html
[openassetio]: https://docs.openassetio.org/
[vfx-platform]: https://vfxplatform.com/
