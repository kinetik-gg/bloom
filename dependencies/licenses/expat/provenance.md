# expat 2.8.3 Provenance

Reviewed: 2026-08-31

## Source

- Upstream project: <https://github.com/libexpat/libexpat> (libexpat.github.io)
- Release tag: `R_2_8_3` (libexpat's GitHub tag naming convention; current stable release at
  review time per the upstream GitHub Releases API, and satisfies OpenColorIO 2.5.2's
  `MIN_VERSION 2.6.0` / `RECOMMENDED_VERSION 2.7.2` requirement — see
  `dependencies/licenses/opencolorio/review.md`'s "Mandatory Transitive Closure")
- Archive: `https://github.com/libexpat/libexpat/archive/refs/tags/R_2_8_3.tar.gz`
- SHA-256 of the acquired archive:
  `533659a16e0184035a99fd8e783f1ad61a887a7bf8586a8681740b9d7ed42389`

## Acquisition Record

- Acquired 2026-08-31 over HTTPS; digest computed from the exact downloaded bytes before
  extraction. Archive inspection: 241 entries (21 directories, 201 regular files, 18
  executable-bit regular files, **1 symbolic link**).
- The one symbolic link is `libexpat-R_2_8_3/README.md -> expat/README.md`: a relative,
  in-archive link at the repository root. It resolves within the archive root to
  `libexpat-R_2_8_3/expat/README.md`, a regular file confirmed present in the acquired archive.
  This recipe's `ExternalProject_Add` configures with `SOURCE_SUBDIR expat` — only the `expat/`
  subdirectory (the C library's own `CMakeLists.txt`, sources, and headers) is consumed as a
  build tree; the root-level `README.md` symlink and its target both sit outside that consumed
  subtree (`expat/README.md` is itself a plain file the build never references — it is not
  `expat/CMakeLists.txt`, `expat/lib/*`, or any file `expat/CMakeLists.txt` installs). Per the
  acquisition rule's provenance-recorded symbolic-link tolerance: relative target, resolves inside
  the archive root, subtree outside every locked build's consumed sources. The reader validates
  this entry against this record and skips it — nothing is materialized. Any other link in a
  future archive revision is a hard failure until reviewed here.
- This is GitHub's tag-generated archive, for which upstream publishes no signature
  (`provenancePolicy: not-published`). libexpat does not publish a detached signature or Sigstore
  bundle for its GitHub tag archives at this release.

## License

- MIT, copyright Thai Open Source Software Center Ltd, Clark Cooper, and the Expat maintainers.
  `LICENSE` beside this file holds the exact bytes from the acquired archive's `expat/COPYING`.

## Status

- NOT QUALIFIED. Acquisition provenance only; qualification follows the intake contract gates.
  The symbolic-link finding above is resolved by the recorded acquisition-time skip.
