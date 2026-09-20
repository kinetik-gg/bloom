# SPIRV-Tools Security Review

Reviewed: 2026-09-19

## Reachability

SPIRV-Tools is used only as an offline, build-time validator. It is not linked into the Bloom
application and is not shipped to end users. In this pass it validates Bloom-authored SPIR-V from a
small compute shader; it does not process untrusted third-party SPIR-V. A future OCIO-generated
shader path would still feed it only build/qualify-time generated modules.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `KhronosGroup/SPIRV-Tools` at
  review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed, and the tool was exercised only on a trusted shader. The lock's empty
  `vulnerabilities` list records only that no advisory was identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified, build-only tool component. This does not
  qualify it for release and does not replace the dependency-intake qualification gates.
