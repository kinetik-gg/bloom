# zlib 1.3.2 Security Review

Reviewed: 2026-08-27

- Exposure: zlib inflates untrusted deflate streams from `.bloom` archives, behind libzip and
  Bloom's own bounded raw preflight, entry-count/size/ratio ceilings, and CRC verification.
- No published CVE affects 1.3.2 at review time (checked GitHub advisories and NVD; the 1.3.x
  line postdates the CVE-2022-37434 fix and the 2023 MiniZip issues, and MiniZip is not built).
- Mitigations regardless of library behavior: decompression output is bounded by the container
  contract's expansion ceilings before use; allocation routes through bounded project-I/O memory
  at the Bloom boundary.
- Disposition: accepted for intake at 1.3.2. Re-review on any zlib advisory or version change.
