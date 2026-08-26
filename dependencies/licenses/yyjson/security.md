# yyjson 0.12.0 Security Review

Reviewed: 2026-08-26

## Scope

yyjson parses untrusted `.bloom` container JSON entries after Bloom's own bounded preflight
scanner accepts them. It is the highest-exposure dependency in the project-format pipeline and is
selected partly for its security posture: single-file C library, no dynamic dependencies, no
network or filesystem access of its own, extensive fuzzing in upstream CI (OSS-Fuzz integrated).

## Findings

- No published CVE affects yyjson 0.12.0 at review time (checked GitHub security advisories and
  the NVD by product name). The `vulnerabilities` array is empty accordingly.
- Bloom-side mitigations that hold regardless of parser behavior: the strict preflight scanner
  bounds input size, depth, value counts, and string bytes before yyjson sees a byte; all
  allocation is routed through Bloom's bounded project-I/O memory; and duplicate-key rejection is
  enforced by Bloom above the parser.
- The build minimizes surface: library only, no tools, tests, fuzzers, or miscellaneous code
  enter the prefix.

## Disposition

Accepted for intake at 0.12.0 with the mitigations above. Re-review triggers: any yyjson
security advisory, any Bloom decision to expose yyjson to bytes that bypass the preflight
scanner, or a version change (which is a lock change requiring this record's renewal).
