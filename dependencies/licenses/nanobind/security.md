# nanobind 3.0.1 security review

Reviewed: 2026-09-17

Reviewed the official 3.0.1 changelog and the downloaded CMake/native-core sources. 3.0.1 includes
the fix for incorrect argument-annotation counts causing out-of-bounds writes. Bloom bindings must
keep callable signatures and annotations consistent. No network service, compiler invocation or
package installer is exposed by the binding core. Python scripts are trusted local code; this
bridge is not a sandbox. MCP validates external data before entering the host seam and never
accepts Python source as a tool argument.

No vulnerability-scan result is claimed by this source review. Native sanitizer coverage and
platform package qualification remain release gates. Arbitrary Python is excluded from render
kernels and the deterministic evaluation path.

Reference: <https://nanobind.readthedocs.io/en/latest/changelog.html>
