# VulkanMemoryAllocator v3.4.0 Security Review

Reviewed: 2026-09-19

## Reachability

VulkanMemoryAllocator is a single header (`include/vk_mem_alloc.h`) plus CMake package config. No
Bloom translation unit compiles it in this slice, no executable runs, and no parser or I/O surface
is introduced by intake alone. When a future backend defines `VMA_IMPLEMENTATION`, the
implementation would compile into a Bloom-owned Vulkan device translation unit; that reachability
review belongs with the backend slice.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for
  `GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator` at review time: the API returned an empty
  advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, dependency
  scanner, or fuzzing was performed for this slice, and the header is not compiled or executed
  here. The lock's empty `vulnerabilities` list records only that no advisory was identified within
  this bounded scope; it is not a security assertion.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify the
  component for release and does not replace the dependency-intake qualification gates.
