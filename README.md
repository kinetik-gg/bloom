# Bloom

Bloom is a motion graphics and visual effects workspace designed to carry an idea from first
keyframe to final frame without sacrificing creative flow or production confidence.

Responsive interaction, reversible work, and explicit image and interchange intent shape every
part of the product.

## Design Commitments

- **Creative work stays responsive.** Decode, evaluation, rendering, project I/O, import, export,
  and other potentially heavy work run outside the UI thread with bounded resources, cancellation,
  progress, diagnostics, and stale-result rejection.
- **Every frame is accountable.** Color, alpha, HDR, time, preview quality, and final-output intent
  remain explicit. A deterministic reference path defines correctness, and acceleration cannot
  silently change project meaning.
- **Work is predictable and recoverable.** Persistent edits are reversible, saves are validated and
  atomic, and unavailable dependencies remain visible and diagnosable instead of silently changing
  or discarding project intent.
- **The workspace adapts to the artist.** Creative surfaces are replaceable editor panels that can
  follow shared context or serve a focused task. Layout choices remain separate from project and
  render truth.
- **Production handoffs are honest.** Bloom adopts established specifications and maintained
  implementations where they fit. Each integration declares its supported subset and reports loss
  or approximation instead of hiding it.
- **Growth has owned boundaries.** In-tree systems use direct typed dependencies. Focused extension
  points can add editors, automation, pipeline capabilities, adapters, and render services without
  turning ordinary application code into a collection of plug-ins.
- **Portability is semantic, not cosmetic.** Linux, macOS, and Windows are product targets. A
  feature is not considered cross-platform merely because it compiles; equivalent behavior,
  explicit capability reporting, and tested fallbacks are part of qualification.

## Architecture

Bloom separates durable project truth, replaceable session/UI state, and rebuildable runtime state.
Panels read shared state and issue commands; immutable document snapshots flow through bounded task
services into CPU or GPU render backends and explicit color/output adapters.

The implementation uses C++20 with a Qt 6.8 Widgets shell. Qt remains at the application and UI
boundary; deterministic document, project, runtime, and rendering systems remain independently
testable native modules.

The repository is organized by ownership rather than by screens or third-party libraries. Start
with the [architecture overview](docs/architecture/overview.md) for module boundaries and dependency
direction, then follow the focused contracts linked from it. Significant cross-cutting decisions are
recorded as [architecture decision records](docs/decisions/).

## Build and Test

The desktop development build requires:

- A C++20 compiler
- CMake 3.25 or newer
- Ninja
- Qt 6.8 or newer with Qt Widgets

Every build selects an explicit dependency mode. The `dev` preset uses `developer-system` mode
(host packages, labeled Unqualified); the CI preset uses `qualified` mode, which validates the
reviewed dependency lock and resolves dependency packages only from the superbuild prefix:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Linux, the development executable is written to `build/dev/apps/bloom/Bloom`.

The repository also provides a Qt-free Linux CI preset for core and quality checks. It expects
the dependency prefix at `build/dependency-prefix` (a one-time step per checkout, rerun after a
lock change) and the toolchain named by the dependency lock:

```sh
cmake -S dependencies/superbuild -B build/dependency-superbuild \
    -DBLOOM_DEPENDENCY_PREFIX="$PWD/build/dependency-prefix" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/dependency-superbuild

CC=clang CXX=clang++ cmake --preset ci-linux-native
cmake --build --preset ci-linux-native
ctest --preset ci-linux-native
```

## Contributing

Repository documentation is Bloom's canonical source of truth. Before proposing a change, read the
[documentation index](docs/README.md), [product foundation](docs/product/foundation.md), and the
focused architecture or UX contract that owns the behavior. Changes to module boundaries or durable
project semantics should update the owning document and, when cross-cutting, include an ADR.

Keep contributions as small vertical slices with tests for durable document, command, project I/O,
and rendering behavior where applicable. UI code reads state and issues commands; it must not own
project truth or perform potentially heavy work on the UI thread.

## License

Bloom's original source and documentation are licensed under the
[Apache License, Version 2.0](LICENSE). Third-party dependencies and assets retain their own
licenses. Distributed builds must include the corresponding notices, source information, and other
obligations of the components they ship.
