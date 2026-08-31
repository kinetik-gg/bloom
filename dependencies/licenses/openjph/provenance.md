# OpenJPH 0.31.0 Provenance (Vendored Inside OpenEXR)

Reviewed: 2026-08-31

## Source

OpenJPH is not an independently downloaded component. OpenEXR 3.4.15's `OpenEXRCore` library
unconditionally compiles an HTJ2K (JPEG 2000-derived) codec whose implementation is
`external/OpenJPH` inside the OpenEXR release archive itself — there is no CMake option to omit
it (`src/lib/OpenEXRCore/CMakeLists.txt` calls `add_subdirectory(${OPENJPH_SOURCE_DIR} ...
EXCLUDE_FROM_ALL)` unconditionally whenever no external `openjph` package is used, and no such
package is provided in Bloom's prefix). `dependencies/superbuild/projects/openexr.cmake` sets
`OPENEXR_FORCE_INTERNAL_OPENJPH:BOOL=ON` so this outcome is deterministic and hermetic rather than
depending on a host `openjph` package happening to be absent from the build machine — see
`dependencies/licenses/openexr/review.md`.

- Upstream project (as vendored): <https://github.com/aous72/OpenJPH>
- Vendored version: `0.31.0`, read from
  `external/OpenJPH/src/core/openjph/ojph_version.h` (`OPENJPH_VERSION_MAJOR/MINOR/PATCH`) inside
  the OpenEXR 3.4.15 archive.
- Source archive: identical to the OpenEXR 3.4.15 archive already recorded in
  `dependencies/licenses/openexr/provenance.md`
  (`https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.4.15.tar.gz`,
  SHA-256 `445ed5b0ea4d9cf98be3a4f219e419628b123b61dec65ccb743ab9b07fbebdaa`). OpenJPH is not
  separately downloaded, hashed, or cached; its bytes are covered by that one digest.

## Acquisition Record

- No separate acquisition: OpenJPH enters the build only as a subtree of the already
  digest-verified OpenEXR archive (see above). It compiles as an `OBJECT` library
  (`OJPH_LIB_TYPE=OBJECT`, `OJPH_INSTALL=OFF`, `OJPH_BUILD_EXECUTABLES=OFF` — all forced by
  OpenEXR's own CMake) and its object files are linked directly into `libOpenEXRCore-3_4.a`. It
  is never installed as its own package, has no independent CMake package config in the prefix,
  and provides no separately exported target.

## License

- BSD-2-Clause, copyright Aous Naman, Kakadu Software Pty Ltd, and The University of New South
  Wales. `LICENSE` beside this file holds the exact bytes from `external/OpenJPH/LICENSE` inside
  the OpenEXR 3.4.15 archive.

## Status

- NOT QUALIFIED. Documented here because it is vendored, compiled, statically linked code shipped
  inside `libOpenEXRCore-3_4.a`, and `docs/architecture/dependency-intake.md` requires every
  reached dependency — "including vendored code" — to have its own component record with its own
  license disclosure. This is not a superbuild recipe (no separate `ExternalProject_Add`; nothing
  to point at a URL or hash independently of OpenEXR's own archive).
