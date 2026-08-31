# OpenColorIO 2.5.2 Security Review

Reviewed: 2026-08-31

## Parsing-Surface Posture

OCIO parses `.ocio` (YAML) configs, `.ocioz` (ZIP) archives, and a range of third-party LUT
formats (Spi1D, Spi3D, IridasCube, Discreet 1DL, CLF/CTF, ICC, etc.). This review's scope is
intake — acquiring, building, and minimizing this component — not qualifying it to process
arbitrary untrusted configuration input; that qualification is `docs/architecture/color-management.md`'s
"Supervised OCIO Execution" and "Deterministic Fixtures And Gates" sections, tracked separately
under Batch 7 implementation.

- Bloom's v1 process/display split (color-management.md) routes every non-built-in config through
  a supervised, killable `bloom-color-worker` helper with hard deadlines, a memory ceiling, and no
  network access — the untrusted-input isolation boundary is Bloom-owned process supervision, not
  a claim about OCIO's own parser robustness.
- The immutable **Bloom Neutral v1** built-in is explicitly called out as executing in-process
  "only when its immutable payload, exact dependency build, parser/build latency, transform
  latency, allocator behavior, and hostile-fixture suite are all qualified" — a separate,
  not-yet-complete qualification gate this intake does not satisfy on its own.

## Known Vulnerabilities

- Checked the `AcademySoftwareFoundation/OpenColorIO` GitHub Security Advisories API endpoint at
  review time: 4 advisories exist against this repository, all `high` severity, all stack buffer
  overflows via unbounded `sscanf %s` in LUT-file parsers:
  - GHSA-66xr-9rgw-v6m8 (Spi1D `.spi1d`)
  - GHSA-rxp3-rrgx-f547 (Spi3D `.spi3d`)
  - GHSA-28jr-x9w2-5pc4 (IridasCube `.cube`)
  - GHSA-fgx7-35rr-5mx2 (Discreet 1DL `.lut`)
- Every one of those advisories' `patched_versions` is exactly `2.5.2` (`vulnerable_version_range:
  <= 2.5.1`) — the locked version **is** the fix; there is no newer patched release to prefer and
  no known unpatched advisory affects it at review time.
- No separate NVD CVE search was needed beyond the GitHub advisory sweep above, matching the
  precedent set in `dependencies/licenses/openexr/security.md`.

## Disposition

Accepted for intake at 2.5.2. All four known LUT-parser advisories are fixed in the locked
version. Re-review on any OpenColorIO advisory or version change, and mandatorily before any
config/LUT input from an untrusted source is qualified for in-process (non-helper) execution.
