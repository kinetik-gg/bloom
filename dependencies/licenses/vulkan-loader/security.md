# Vulkan-Loader vulkan-sdk-1.4.357.0 Security Review

Reviewed: 2026-09-19

## Reachability

This component is the end-user runtime loader (`libvulkan.so.1`). At runtime it discovers
driver/layer manifests from system and XDG paths, parses their JSON, and loads driver libraries.
That manifest-parsing surface is the loader's main exposure to files outside Bloom's control, and it
runs in-process with the future GPU service, not on the UI thread. In this slice only a bounded
standalone enumeration probe exercises it; no Bloom application path initializes it.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `KhronosGroup/Vulkan-Loader`
  at review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, dependency
  scanner, or fuzzing was performed, and the loader's manifest parsing was not exercised beyond
  device enumeration. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope; it is not a security assertion.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify the
  component for release and does not replace the dependency-intake qualification gates or the
  pending prefix manifest.
