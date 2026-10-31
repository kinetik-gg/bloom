# ADR 0035: Media Provider Fallback Chain On Every Platform

Status: accepted

Date: 2026-10-31

Issues: #370 (Windows), #371 (macOS)

## Context

ADR 0020 admitted FFmpeg 8.1.2 only inside the Linux `bloom-media-worker` and left macOS and
Windows with "typed `Unavailable`" process backends. ADR 0033 turned that into
`PlatformNotSupported` ("Export runs on Linux in this version") for Start, Start All and Retry on
macOS and Windows. macOS gained a VideoToolbox/AVFoundation worker, but it was the only provider
there: MXF, DNxHR, WAV and TIFF were refused, and any VideoToolbox refusal was a failed export.

The owner's decisions (2026-10-31) replace that posture:

- Use the platform media framework when it is present and fall back to bundled FFmpeg when it is
  not. Degrade; do not show "unavailable". Artists do not install dependencies.
- Everything whose licence fits Bloom's boundary ships inside Bloom: the Apache-2.0 application,
  LGPL shared FFmpeg in the media worker, permissive libraries. What cannot be redistributed is a
  one-click in-app download, never a terminal step. GPL and nonfree components stay out.

## Decision

### One ordered provider chain per operating system

| Platform | Chain, in preference order |
| --- | --- |
| Windows | Media Foundation → bundled FFmpeg |
| macOS | VideoToolbox/AVFoundation → bundled FFmpeg |
| Linux | FFmpeg with VA-API (when hardware is requested) → FFmpeg software |

`MediaProviderChainV1` (`src/media/provider/provider_chain.hpp`, codec-free) selects **per
operation**: one direction (decode or encode) of one codec family, plus the container for encode.
A candidate is skipped when it is not installed (its worker executable or platform framework is
absent), when its handshake does not declare the operation, or when it fails the attempt with any
typed error other than cancellation, shutdown or a changed source. Those three end the operation;
every other failure lets the next provider try. When every provider is exhausted the last typed
error is returned.

Selection never substitutes a provider inside an attempt that may already have produced a product
(media-io.md, "Provider Selection, Failure, And Substitution"):

- **Encode.** `EncodeSessionV1::begin()` runs the chain. Each candidate is a complete attempt:
  launch, handshake and `EncodeBegin`. A refusal reaps that worker before the next candidate starts,
  so no frame has been sent. Once `begin()` succeeds the provider is pinned for the attempt. The
  session translates the shared settings into the serving provider's codec spelling (`prores_ks`
  for FFmpeg, `prores` for VideoToolbox).
- **Decode.** `VideoDecodeSession` asks the platform provider first for every operation it
  declares. The first failure retires it for that source (the same sticky, typed rule as the VA-API
  preview decoder of ADR 0027), and FFmpeg answers the same request.

### Provenance, not artist text

Every selection produces a `MediaProviderProvenanceV1`: the serving provider id (`videotoolbox`,
`media-foundation`, `ffmpeg-vaapi`, `ffmpeg`) and each skipped or failed provider with its cause
(`NotInstalled`, `NotDeclared`, `Failed` with the typed error and detail). It is recorded:

- on `SequenceExportResultV1::mediaProvider`, with a diagnostic log line when a fallback occurred;
- in the export QC evidence: `tool`, `version` and the pipeline digest come from the handshake of
  the provider that actually served the attempt, and a ProRes deliverable's implementation note
  names that implementation (Apple-native, or FFmpeg's non-authorization caveat);
- on `FrameDecodeProvenance::provider` and `nativeFallback` for decoded frames.

None of it is user-visible text. The engine no longer produces `PlatformNotSupported` (export
availability code 7, unavailable code 23); `ExportEncoderStatus::exportPlatform` is ignored. The
enum values and the field remain only until the desktop UI removes its references.

### Bundled FFmpeg on all three platforms

The FFmpeg worker is built and installed on every platform, LGPL shared, private to its worker
generation directory, and never linked or loaded by the desktop:

| Platform | Worker | Private libraries |
| --- | --- | --- |
| Linux | `libexec/bloom/media/ffmpeg-v1/bloom-media-worker` | `lib/` beside it, rpath `$ORIGIN/lib` |
| macOS | `Bloom.app/Contents/libexec/bloom/media/ffmpeg-v1/bloom-media-worker` | `lib/` beside it; dylibs keep `@rpath` install names, worker rpath `@loader_path/lib` |
| Windows | `bin\media\ffmpeg-v1\bloom-media-worker.exe` | the DLLs in the same directory (searched first by the loader) |

The macOS platform worker is `bloom-media-videotoolbox-worker` in `videotoolbox-v1`. The host
locates workers relative to the executable the operating system reports (`/proc/self/exe`,
`_NSGetExecutablePath`, `GetModuleFileNameW`), falling back to the build tree.

Windows process supervision (`process_supervisor_windows.cpp`) keeps the POSIX contract: byte-mode
named pipes with overlapped I/O share one deadline and honour cancellation, a startup gate releases
the worker, the child inherits only its pipe ends, the gate and `NUL`, and a job object carries the
committed-memory limit, forbids grandchildren and kills the worker when the supervisor goes away.
The inherited source snapshot and streamed staged output are POSIX descriptors; on Windows (and for
streamed output on macOS) the host uses the path and chunk-copy routes it already has.

The superbuild FFmpeg recipe builds on macOS (`--install-name-dir=@rpath`) and on Windows (MSVC
toolchain driven from an MSYS2 shell). VA-API stays Linux-only. (Amended by ADR 0036: OpenH264 is
now built from source and linked into `libavcodec` on every platform; there is no Cisco runtime
download.)

## Not done in this decision

- **Media Foundation provider.** The Windows chain has its slot (`media-foundation`), which reports
  `NotInstalled`; FFmpeg serves every Windows operation. Without it Windows has no H.264 or HEVC
  encoder until a bundled software encoder lands in the FFmpeg recipe.
- **Windows staged publication.** `StagedArtifactCoordinator` has no Windows provider
  (`staged_artifact_unsupported.cpp`), so a Windows export cannot publish its file yet.
- **Mid-stream encode fallback.** A platform encoder that fails after frames were sent fails that
  attempt; the chain covers refusals at `begin()`.
- **macOS HEVC encode.** The VideoToolbox provider declares HEVC decode only.

## Verification boundary

The chain, the encode/decode wiring and the host availability changes are built and tested on
Linux. The Windows supervisor, the Windows/macOS branches of the worker, its CMake layout and the
macOS/Windows superbuild branches have not been compiled or run on those systems.

## Consequences

ADR 0020's "typed `Unavailable`" fallback for macOS and Windows and ADR 0033's
`PlatformNotSupported` rows are superseded. Qualification remains per provider: a fallback to
FFmpeg carries FFmpeg's own evidence and caveats, never the platform provider's.
