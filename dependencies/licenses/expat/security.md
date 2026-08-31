# expat 2.8.3 Security Review

Reviewed: 2026-08-31

## Reachability

`grep -rl "expat.h\|XML_Parser" src ext` against the downloaded OpenColorIO 2.5.2 archive finds
expat used in exactly three file-format parsers compiled into `libOpenColorIO.a` unconditionally:
`fileformats/cdl/CDLParser.cpp` (ASC CDL `.cdl`/`.cc`/`.ccc`), `fileformats/FileFormatCTF.cpp`
(ACES CTF/CLF transform files), and `fileformats/FileFormatIridasLook.cpp` (`.look`). These
parsers run whenever an OCIO config references one of these file types — reachable input for the
project-relative `.ocioz`, external `.ocioz`, and external loose config locator kinds
`docs/architecture/color-management.md` defines, all of which execute inside the supervised,
killable `bloom-color-worker` helper per that document's "Supervised OCIO Execution" section
(hard 30-second deadline for "one config parse plus processor build"; expiry "terminates the
helper process ... and publishes no partial processor or frame"). The immutable Bloom Neutral v1
built-in is Bloom-authored, fixed payload content and never resolves an attacker-controlled
CDL/CTF/look file, so it is not exposed to this reachable surface.

## Known Vulnerabilities

- Checked the `libexpat/libexpat` GitHub Security Advisories API endpoint at review time: 0
  advisories. GitHub's global advisory search (`/advisories?affects=expat`) also returned 0 —
  neither source has ingested the finding below yet.
- NVD keyword search for "libexpat" surfaces **CVE-2026-66046** (published 2026-08-18, disclosed
  by VulnCheck, CVSS 3.1 base score 7.5/HIGH, CVSS 4.0 base score 8.7/HIGH): a denial-of-service
  vulnerability in `storeAtts()` in `xmlparse.c` — O(N²) quadratic-complexity attribute
  processing lets "a remote unauthenticated attacker ... supply a single well-formed XML document
  of a few megabytes ... to cause excessive CPU consumption." NVD's affected-version range is
  `<= 2.8.3` — **the locked version is within the affected range.**
- The fix (`libexpat/libexpat#1321`, "Migrate `.isCdata` lookup from a linear loop to a hash table
  lookup") merged to expat's `master` branch on 2026-08-18, but **no tagged release contains it**
  as of this review: `R_2_8_3` (2026-08-10) predates the merge, and no newer tag exists (checked
  the releases API; `R_2_8_3` remains the latest). There is currently no expat release this recipe
  could lock that fixes CVE-2026-66046 — 2.8.3 is simultaneously "current stable" and "affected."
- No separate GHSA advisory record exists for this CVE yet; disposition is based on the NVD entry
  and the linked, merged upstream PR directly.

## Disposition

**Accepted for intake at 2.8.3 with a tracked, mitigated exposure**, not "not-affected": this is a
real, currently-unpatched-in-any-release vulnerability that reaches Bloom's build.

- Mitigation: every reachable code path (CDL/CTF/look file parsing from a project-relative or
  external OCIO config) already runs inside `bloom-color-worker`, bounded by the supervisor's hard
  30-second parse+processor-build deadline and process-kill-on-expiry behavior
  (`docs/architecture/color-management.md`, "Supervised OCIO Execution"). A crafted file exploiting
  this quadratic-complexity DoS is bounded to `HelperDeadline`/`HelperTerminated` failure of one
  request, not an application-wide hang — the same isolation boundary that already has to hold for
  any hostile OCIO/expat input class.
- No mitigation exists for the CPU cost incurred before the deadline fires (up to ~30 seconds of
  CPU burn per malicious request); this is bounded, not eliminated.
- Residual risk: none for Bloom Neutral v1 (fixed, Bloom-authored payload, no external CDL/CTF/look
  references). Bounded-but-nonzero for project-relative/external configs until a patched expat
  release exists.
- **Re-review is mandatory** the moment libexpat cuts a release containing the `#1321` fix — this
  is expected to be the very next tagged release. This is an explicit incident-tracking entry per
  `docs/architecture/dependency-intake.md`'s Upgrade And Incident Policy ("records reachability,
  exposure to untrusted content, affected package versions, mitigation, and the replacement
  lock"); the lock's vulnerability disposition for this entry is the supervisor's call to make
  (this record recommends `mitigated`, given the bounded-by-deadline reasoning above), not
  fabricated here.
