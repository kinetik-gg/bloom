# Vulkan-Headers vulkan-sdk-1.4.357.0 Security Review

Reviewed: 2026-09-19

## Reachability

Vulkan-Headers ships C/C++ headers, the Vulkan XML registry, and CMake package config only. No
translation unit from this component is compiled, no executable runs, and no parser or I/O surface
is introduced by intake alone. It is consumed later as build-time include paths for Bloom's own
Vulkan backend.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for
  `KhronosGroup/Vulkan-Headers` at review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, dependency
  scanner, or fuzzing was performed for this slice, and the source tree is not compiled or executed
  here. The lock's empty `vulnerabilities` list records only that no advisory was identified within
  this bounded scope; it is not a security assertion.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify the
  component for release and does not replace the dependency-intake qualification gates.
