# Bloom

Bloom is a native motion, compositing, and editing application from Kinetik, built with C++ and Qt.

Status: foundation implementation. The application launches a replaceable-panel compositing
workspace backed by a canonical document, commands, an asynchronous task runtime, exact animation
sampling, and snapshot compilation. Its deterministic CPU path evaluates process frames and
prepares separately identified display frames off the UI thread for the Viewer. Project
persistence foundations are underway; complete archive I/O and standards-backed color/output
adapters are the next major vertical checkpoints.

## Build

Requirements:

- A C++20 compiler
- CMake 3.25 or newer
- Ninja
- Qt 6.8 or newer with Qt Widgets

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/apps/bloom/Bloom
```

## Documentation

Repository documentation is Bloom's canonical source of truth. Begin with:

1. [`docs/README.md`](docs/README.md)
2. [`docs/product/foundation.md`](docs/product/foundation.md)
3. [`docs/product/quality-bar.md`](docs/product/quality-bar.md)
4. [`docs/standards/strategy.md`](docs/standards/strategy.md)
5. [`docs/ux/compositing-workspace.md`](docs/ux/compositing-workspace.md)
6. [`docs/architecture/overview.md`](docs/architecture/overview.md)
7. [`docs/architecture/module-system.md`](docs/architecture/module-system.md)
8. [`docs/roadmap.md`](docs/roadmap.md)

Architecture decisions are recorded in [`docs/decisions/`](docs/decisions/).

## License

Bloom is licensed under the [Apache License, Version 2.0](LICENSE). Third-party dependencies and
assets retain their own licenses; release packages include the corresponding notices and source
information required by the components they ship.
