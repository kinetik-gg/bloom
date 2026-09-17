# nanobind 3.0.1 review

Reviewed: 2026-09-17

BSD-3-Clause permits Bloom's static bridge distribution with the original copyright, license and
non-endorsement terms retained. No upstream code is modified. The superbuild compiles the native
core once with position-independent code, using the archive's own CMake library function, and
installs a relocatable imported target and public headers. Normal Bloom builds compile only Bloom's
binding code; they perform no dependency acquisition or third-party source compilation.

Tests, wheels, stub generation, split-mode backends, free-threaded Python and stable ABI builds are
not enabled. The consuming interpreter must match the superbuild's exact SOABI. CPython is a host
prerequisite, not a new bundled dependency or a qualified release interpreter. Linux CI installs
python3-dev. Windows and macOS use the same CMake recipe with matching development packages;
release qualification for those platforms remains pending their native gates.
