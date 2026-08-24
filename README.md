# Bloom

Bloom is a native motion, compositing, and editing application from Kinetik, built with C++ and Qt.

Status: foundation scaffold. The application currently launches a placeholder compositing
workspace; it does not yet contain a document model or renderer.

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
7. [`docs/roadmap.md`](docs/roadmap.md)

Architecture decisions are recorded in [`docs/decisions/`](docs/decisions/).

## License

The project license is an open decision. This repository does not claim a license until a license
file is intentionally added.
