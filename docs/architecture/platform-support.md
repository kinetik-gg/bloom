# Cross-Platform Support Contract

Status: working

Implementation status: the portable staged-artifact interface has a tested Linux foundation with
checked writer close, no-follow exact-stage reopen, bounded verifier reads, explicit acceptance or
rejection, and publication blocked until acceptance. Non-Linux providers currently fail explicitly
as unsupported. Format-specific semantic validators, cancellation/progress integration, macOS and
Windows providers, and three-platform parity qualification remain pending.

Updated: 2026-10-13

## Objective

Linux, macOS, and Windows are first-class Bloom platforms. Cross-platform means the same project can
be authored, opened, evaluated, and rendered with equivalent user-visible semantics on each target;
it does not mean forcing every platform to use the same low-level implementation.

A feature is not complete when it works on only one target. A platform-specific optimization is
acceptable only behind a portable contract with an equivalent implementation or an explicit,
tested fallback on the other supported platforms.

## Product Parity Rules

- Project and render semantics are platform-independent.
- Core authoring workflows, commands, keyboard actions, panel types, and diagnostics exist on all
  three platforms.
- Platform conventions may change labels, modifier keys, menu placement, file dialogs, and other
  presentation details without changing capability.
- No project may require an operating-system-only API merely to open or preserve its editable data.
- An optional capability that is genuinely unavailable must be discovered at runtime, shown before
  use, saved without data loss, and reported clearly when evaluation depends on it.
- A feature backed by a native optimization retains a portable path. If no honest equivalent or
  fallback exists, the feature remains experimental rather than entering the supported product.

“Works on my platform” is not acceptance. Feature reviews and milestone acceptance identify Linux,
macOS, and Windows behavior explicitly.

## Portability Layers

Bloom uses three implementation layers:

```text
Product and domain code
        |
        v
Portable Bloom service interfaces
        |
        +------ Qt/platform adapter
        +------ render backend adapter
        +------ media/VFX library adapter
        |
        v
Operating-system and device APIs
```

Domain code depends on Bloom interfaces, the C++ standard library, and deliberately adopted
cross-platform libraries. It does not call Win32, Cocoa/AppKit, Metal, Direct3D, X11, Wayland, or
other native APIs directly.

Qt 6 owns the common desktop shell and provides the default adapters for windows, menus, input,
clipboard, drag and drop, accessibility, display scaling, and standard dialogs. Narrow native
adapters are allowed where Qt cannot provide the required quality, but they stay below a portable
interface and do not leak native handles or types into document, command, project, runtime, or
render semantics.

Conditional compilation is localized to platform and backend implementation files. Scattered
`#ifdef` branches in product logic are a design warning.

## Files, Paths, And Project Portability

- Canonical project data stores stable asset IDs and portable locators, not raw native handles.
- Serialized paths use a defined representation and never depend on native separator syntax.
- Filesystem access preserves Unicode names and does not assume case sensitivity, case folding,
  drive letters, executable bits, symlink behavior, or a fixed maximum path length.
- Project-relative media remains relative when a project moves between supported systems.
- Missing or inaccessible media produces a relinkable asset state rather than corrupting project
  structure.
- Atomic save uses a platform adapter with tested crash and replacement behavior on every target.
- Cache, configuration, logs, temporary files, and user data use platform-appropriate locations and
  are never embedded into portable project truth.

Projects and fixtures saved on one platform are round-tripped and evaluated on the other platforms
in automated tests.

## Media, Color, And Interchange Dependencies

Open standards and established cross-platform implementations are preferred for media, color, and
interchange. Adopting a standard does not bypass the parity contract:

- the selected library and required feature set must build and be tested on every supported target
- library-specific objects stay behind Bloom-owned interfaces
- unavailable optional codecs, transforms, or plugins report capability diagnostics rather than
  changing interpretation silently
- unsupported data is preserved where the project format promises round-trip safety
- interchange tests use shared fixtures across platforms

Platform media frameworks may be optional accelerators, importers, or exporters. They cannot be the
only implementation of a core project or evaluation semantic unless equivalent support exists on
the other platforms.

## GPU And Display Portability

Feature code targets Bloom's render interfaces rather than a native graphics API. The chosen GPU
strategy may use one portable backend or multiple per-platform backends, but it must provide:

- the same declared node and parameter semantics
- explicit capability discovery and limits
- equivalent color intent and alpha behavior
- consistent cache and invalidation rules
- device-loss handling without loss of project truth
- a CPU reference path for correctness and diagnostics where defined by the render architecture

GPU results may use documented numerical tolerances where hardware arithmetic differs. A tolerance
is not permission for visible, temporal, color-management, or alpha-compositing divergence.
Presentation interop is separate from canonical render resources so changing a window-system or
swapchain integration does not redefine an image.

## Input, Layout, And Artist Workflows

- Every supported command has an action-level identity independent of its displayed shortcut.
- Default shortcuts follow platform conventions where appropriate and remain remappable.
- Mouse, pen/tablet, high-resolution wheel, keyboard, text input, and input-method events enter
  through normalized UI adapters.
- Logical layout is independent of device pixels; mixed-DPI and display-scale changes are tested.
- Panel replacement, workspace restore, focus, drag and drop, and direct manipulation have the same
  user model on all targets.
- Accessibility names, keyboard focus, and non-color status cues are preserved across platform
  adapters.

A native convention may improve familiarity, but platform presentation must not fork Bloom into
three different workflow models.

## Concurrency And Process Behavior

Bloom follows the non-blocking contract in `task-system.md` on every platform. Code must not rely on
one operating system's scheduler, thread priority semantics, filesystem notification behavior, or
process lifecycle behavior for correctness.

Background work uses Bloom task and process services. Platform adapters normalize cancellation,
exit status, path quoting, environment handling, and diagnostics. The UI thread never waits for an
external process or a platform-specific GPU operation.

## Build, Test, And Release Matrix

CMake describes targets and feature discovery. Supported builds use the native production-quality
toolchain for each platform while keeping warning policy and tests semantically aligned.

Continuous integration must cover Linux, macOS, and Windows before a feature becomes supported. The
matrix grows in proportion to implementation risk and includes:

- configure, compile, unit tests, and headless integration tests
- project-format and cross-platform fixture compatibility
- deterministic CPU reference renders and tolerance-based GPU comparisons where applicable
- UI smoke tests for panel lifecycle and asynchronous result delivery
- installation, launch, resource discovery, and clean uninstall/upgrade checks
- representative path, locale, scale-factor, and missing-capability cases

Platform-specific tests supplement the shared suite; they do not replace it. A platform may be
temporarily red during development, but a release cannot call a feature complete while its required
workflow is absent or knowingly broken on another first-class target.

## Capability Reporting And Graceful Degradation

Runtime capability checks produce structured data that both the UI and headless tools can inspect.
Checks cover relevant GPU features, display behavior, codecs, color processors, plugins, and other
optional integrations.

When a capability is missing, Bloom chooses one declared outcome:

1. use a validated equivalent implementation
2. use a visible preview-only degradation that cannot affect final output silently
3. disable the operation with an actionable diagnostic while preserving project data

The application does not crash, hide controls without explanation, mutate a project to fit the
current machine, or silently render a different result.

## Definition Of Cross-Platform Complete

A user-facing feature is cross-platform complete only when:

- its semantics and fallback policy are documented
- it is implemented through portable boundaries
- Linux, macOS, and Windows pass the relevant shared tests
- projects using it survive cross-platform save/open round trips
- unsupported runtime capabilities fail visibly and preserve authoring data
- packaging includes its required dependencies and licenses on each target
- known presentation differences are intentional and documented

This definition applies from the first vertical proof onward, not as a cleanup phase before release.


## Focused GPU And Explicit CPU Fallback Gate

The manual [`gpu-qualification.yml`](../../.github/workflows/gpu-qualification.yml) workflow builds
an explicit Vulkan-OFF CPU fallback on Linux, then runs the bounded image,
source-revision, CPU evaluation, processing-budget and host-lifetime tests. It builds selected targets
and does not repeat the repository-wide quality sweep. Locked superbuild archive hashes and library
versions are reused and Linux uses the locked Clang 22 family. The current issue qualification scope
is explicitly Linux. The helper retains portable Apple/MSVC configuration support, but those jobs are
not in the default matrix and no macOS/Windows execution is claimed.

Before the new workflow is on the default branch, the existing `ci.yml` dispatch entry calls it
from the selected branch with `focused_gpu_gate=true`. That explicit manual mode skips the four
normal CI jobs; pull-request/push behavior is unchanged. A concrete branch run is:

```sh
gh workflow run ci.yml --ref <branch> -f focused_gpu_gate=true -f run_native=false
```

Native execution additionally requires `run_native=true` and explicit `native_runner_labels`, for
example `["self-hosted","linux","gpu"]`. No workflow is dispatched by saving these files.

Dependency provenance remains separate from functional test results. The current production lock
contains only a Linux Clang profile. A future explicit macOS/Windows run consumes its exact
source-built prefix in `developer-system` mode and reports **Unqualified**, even if its CPU tests pass. No accepted
Apple/MSVC ABI profile, release qualification or native GPU capability is fabricated. Linux's existing
`Qualified-pending-manifest` state is preserved rather than upgraded by this focused gate.

The reusable [`gpu_qualification.py`](../../tools/ci/gpu_qualification.py) runner writes JSON with
separate CPU fallback, native, dependency-provenance and qualified-native fields. The fallback gate
requires Vulkan explicitly OFF, rejects an internally skipped required CPU test, verifies that the
optional resident-display binary reports its typed CPU fallback, and verifies that its
`--require-device` invocation fails. The optional device skip is recorded as **NotRun native**, never
as a passing native render. Test processes have inherited display/Wayland bindings removed and use
Qt's offscreen platform; this gate launches no desktop application.

For an existing configured fallback build:

```sh
python3 tools/ci/gpu_qualification.py --build-dir build/gpu-cpu-fallback \
  --mode fallback --build --output build/gpu-evidence/fallback.json
```

The workflow's optional native job takes explicit JSON runner labels, builds the locked Vulkan
bootstrap closure, and runs ten bounded native suites: processing-buffer, image-allocation,
display-transfer, resident-display, operation-buffer, presentation-allocation, OCIO-image storage,
queue timing, allocator memory and operation batches. Every suite receives `--require-device`.
The batch suite requires actual multi-kernel submissions, status rejection and bounded retirement;
its optional unavailable-device result cannot count as native qualification.
Device diagnostic `device_type` must identify
an integrated, discrete or virtual GPU. Missing capability and CPU/software Vulkan produce
**NotRun** and a failing exit status in native mode, including without the explicit requirement
marker. An invariant failure remains a
failure and is never relabelled device unavailability. The native gate does not qualify GUI
presentation, all production render routes, sustained playback, platform packaging or full prefix
manifests; those remain independent gates.

```sh
python3 tools/ci/gpu_qualification.py --build-dir build/gpu-native-evidence \
  --mode native --build --require-device --output build/gpu-evidence/native.json
```

An available Windows Vulkan driver may support a native probe, but a prepared workflow is not proof
that a particular Windows machine has that device or that Bloom's Windows GPU routes are qualified.
macOS additionally needs an explicitly supplied, compatible MoltenVK/ICD setup; a system Metal GPU
alone does not establish a Vulkan capability. The optional job probes the actual selected runner and
retains unavailable/failure evidence. As of 2026-10-13, this continuation has local Linux CPU fallback
evidence; macOS/Windows execution remains **NotRun** and outside the current Linux-focused
qualification. Actual target evidence is required before platform parity is claimed.

The immutable still-source seam uses native `std::filesystem::path` file opening, UTF-8 locator/base
identity, fixed-width digest/counters and `steady_clock` timings. Decoder input is one owned byte
vector on every platform, with independent EXR/stb streams and subtraction-checked request budgets.
A filename suffix is appended to the native path without narrowing its existing Unicode spelling.
The source ticket does not depend on filesystem timestamps, watcher semantics or an operating-system
file handle surviving external mutation. The shared fallback tests cover same-size/same-timestamp
replacement, concurrent decode budgets, cancellation, refresh generations, gesture read-only access,
final revalidation and charged worker retirement; execution on each platform is still required.

Linux video source preparation now owns a disk-backed, SHA-256-verified anonymous read-only file.
The process supervisor installs that typed host capability on a dedicated inherited descriptor; the
FFmpeg provider consumes the exact descriptor through custom AVIO. Source-byte quotas include live
captures and pending retirement until the worker closes the backing. The macOS video provider keeps
its existing fresh-verification path and does not advertise retained-video hash elision; Windows
keeps the typed unavailable worker fallback. Portable descriptor plumbing alone is not Apple video
qualification. The Linux source fixture covers same-size/same-mtime replacement, removed originals,
uncached frame parity from retained bytes, cancellation, quota, final revalidation, refresh/reconnect,
gesture read-only behavior and charged worker retirement.
