# minizip-ng 4.2.2 Security Review

Reviewed: 2026-08-31

## Reachability

minizip-ng implements OCIO's `.ocioz` archive read/write path (`OCIOZArchive.cpp`). Per
`docs/architecture/color-management.md`, opening a project-relative or external `.ocioz` archive
runs inside the supervised `bloom-color-worker` helper, under the same bounded-deadline model
discussed in `dependencies/licenses/expat/security.md`. Bloom's own `.ocioz` reader contract
(`docs/architecture/color-management.md`'s "Hostile Configuration And Resource Limits") separately
imposes its own bounded-size, no-symlink, streaming-only limits ahead of any OCIO/minizip-ng call,
per that document's "An `.ocioz` reader accepts only regular, non-executable files..." paragraph —
this recipe's review is limited to minizip-ng's own build/version posture, not a re-derivation of
that Bloom-owned reader contract.

## Known Vulnerabilities

- Checked the `zlib-ng/minizip-ng` GitHub Security Advisories API endpoint at review time: 0
  advisories.
- NVD keyword search for "minizip-ng" returns 2 CVEs, both heap-buffer-overflow findings against
  **minizip-ng v4.0.2** specifically: CVE-2023-48106 (`mz_path_resolve` in `mz_os.c`) and
  CVE-2023-48107 (`mz_path_has_slash` in `mz_os.c`). Both trace to upstream issues
  `zlib-ng/minizip-ng#740` and `#739`, filed and closed in November 2023 against the 4.0.2 release
  line. The locked version, 4.2.2, is released June 2026 — roughly two and a half years and
  multiple minor releases (4.0.x through 4.2.x) past the affected 4.0.2 release; no advisory names
  a version at or above 4.0.3 as affected, and no current unpatched advisory affects 4.2.2 at
  review time.

## Disposition

Accepted for intake at 4.2.2. Both known advisories are version-floored out by the locked release.
Re-review on any minizip-ng advisory or version change.
