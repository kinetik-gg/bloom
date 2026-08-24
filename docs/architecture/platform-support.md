# Cross-Platform Support Contract

Status: working

Updated: 2026-08-25

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
