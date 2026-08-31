# yaml-cpp 0.9.0 Security Review

Reviewed: 2026-08-31

## Reachability

yaml-cpp parses every OCIO `.ocio` config's YAML body (`src/OpenColorIO/OCIOYaml.cpp`). Per
`docs/architecture/color-management.md`, an untrusted (non-built-in) config's parse runs inside the
supervised, killable `bloom-color-worker` helper under the same 30-second parse+processor-build
deadline discussed in `dependencies/licenses/expat/security.md`. The immutable Bloom Neutral v1
built-in's YAML body is fixed, Bloom-authored content.

## Known Vulnerabilities

- Checked the `jbeder/yaml-cpp` GitHub Security Advisories API endpoint at review time: 0
  advisories.
- NVD keyword search for "yaml-cpp" returns 6 CVEs, all denial-of-service findings (stack
  exhaustion / infinite loop via crafted YAML) against **yaml-cpp 0.5.3 or 0.6.2** specifically:
  CVE-2017-5950, CVE-2017-11692, CVE-2018-20573, CVE-2018-20574, CVE-2019-6285, CVE-2019-6292. The
  locked version, 0.9.0, is many releases past the affected 0.5.x/0.6.x line (0.6.3, 0.7.0, 0.8.0,
  0.9.0 all released after these 2017–2019 findings); no advisory names a version at or above 0.7.0
  as affected, and no current unpatched advisory affects 0.9.0 at review time.

## Disposition

Accepted for intake at 0.9.0. All six known advisories are version-floored out by the locked
release. Re-review on any yaml-cpp advisory or version change.
