# libdeflate 1.26 Provenance

Reviewed: 2026-08-31

## Source

- Upstream project: <https://github.com/ebiggers/libdeflate>
- Release tag: `v1.26` (current stable tag at review time; the newest of
  `v1.26, v1.25, v1.24, ...` per the repository's tag list. Not named by
  `docs/architecture/dependency-intake.md`'s Initial Qualification Candidates table because that
  table predates the zlib-vs-libdeflate determination recorded in `review.md` — libdeflate enters
  as OpenEXR 3.4.15's required DEFLATE backend, not a VFX Reference Platform-listed component.)
- Archive: `https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.26.tar.gz`
- SHA-256 of the acquired archive:
  `bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85`

## Acquisition Record

- Acquired 2026-08-31 over HTTPS; digest computed from the exact downloaded bytes before
  extraction. Archive inspection: 112 entries (94 regular files, 18 directories), zero symbolic
  links, hardlinks, devices, or FIFOs.
- This is GitHub's tag-generated archive, for which upstream publishes no signature
  (`provenancePolicy: not-published`). No signed release artifact is published separately for
  libdeflate.

## License

- MIT, copyright Eric Biggers and Google LLC (both copyright lines appear in the archive's
  `COPYING` file; Bloom does not assert why both holders are named). `LICENSE` beside this file
  holds the exact bytes from the acquired archive (`COPYING` in the archive root).

## Status

- NOT QUALIFIED. Acquisition provenance only; qualification follows the intake contract gates.
