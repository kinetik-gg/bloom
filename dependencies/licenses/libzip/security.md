# libzip 1.11.4 Security Review

Reviewed: 2026-08-27

- Exposure: libzip parses untrusted `.bloom` archive structure (central/local headers, ZIP64,
  attributes). It is the container-format boundary for hostile archives.
- No published CVE affects 1.11.4 at review time (checked GitHub advisories and NVD).
- Feature minimization removes the crypto backends (GnuTLS/OpenSSL/mbedTLS/Windows crypto),
  bzip2/lzma/zstd compression, tools, regression suite, and docs from the build: the permitted
  path is stored+deflate only, per the container contract's constrained ZIP profile.
- Bloom-side mitigations: the contract mandates a bounded raw preflight for any invariant libzip
  cannot enforce (overlap, trailing data, duplicate names, method/attribute restrictions,
  expansion ceilings), no extraction to filesystem, and bounded memory. Qualification must prove
  the exposure matrix in `project-format.md` ("Qualified Implementations").
- Disposition: accepted for intake at 1.11.4. Re-review on any libzip advisory or version change.
