# pystring 1.2.0 Security Review

Reviewed: 2026-08-31

## Reachability

pystring is a small string-manipulation helper compiled into `libOpenColorIO.a` and used
throughout config parsing and path handling. It has no independent input-parsing surface of its
own (no file format, no network protocol); its functions are called with strings already produced
by OCIO's own YAML/XML/LUT parsers.

## Known Vulnerabilities

- Checked the `imageworks/pystring` GitHub Security Advisories API endpoint at review time: 0
  advisories.
- NVD keyword search for "pystring" returns 2 CVEs (CVE-2008-1887, CVE-2017-1000158), both against
  CPython's unrelated internal `PyString`/`PyString_DecodeEscape` C API — a keyword false-positive,
  not the `imageworks/pystring` project. No advisory affects this component.

## Disposition

Accepted for intake at 1.2.0. No known vulnerabilities. Re-review on any pystring advisory or
version change.
