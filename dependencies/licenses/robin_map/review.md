# robin-map 1.4.0 review

Reviewed: 2026-09-17

The MIT license permits compilation into nanobind's static core with the original copyright and
license retained. Only the unmodified headers consumed by nanobind enter compilation; there is no
separate runtime library, tooling, tests or network dependency. The dependency graph records the
vendored direct edge from nanobind. The superbuild installs the license with nanobind's records.
