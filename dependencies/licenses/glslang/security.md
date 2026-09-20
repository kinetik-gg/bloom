# glslang Security Review

Reviewed: 2026-09-19

## Reachability

glslang is used only as an offline, build/qualify-time compiler. It is not linked into the Bloom
application and is not shipped to end users. In this pass it compiles a small Bloom-authored compute
shader; it does not process untrusted third-party GLSL. A future OCIO shader-emission path would
still invoke it only at build time on Bloom-generated, bounded shader text.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `KhronosGroup/glslang` at
  review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed, and the compiler was exercised only on trusted input. The lock's empty
  `vulnerabilities` list records only that no advisory was identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified, build-only tool component. This does not
  qualify it for release and does not replace the dependency-intake qualification gates.
