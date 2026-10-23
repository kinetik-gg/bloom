# NASM 2.16.03 provenance

- Upstream source release: NASM 2.16.03 (stable release line, BSD-2-Clause).
- Canonical source URL: <https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz>
- Source archive retrieval date: 2026-10-23 (UTC date)
- Source archive size: 1,032,388 bytes
- Source archive SHA-256: `sha256:1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148`
- Retrieval path actually used: the canonical host was unreachable at the intake date, so the
  byte-identical archive was retrieved from the Debian `nasm_2.16.03.orig.tar.xz` upstream-source
  mirror. Its SHA-256 and size equal the upstream release values recorded above; the recipe still
  names the canonical upstream URL, and the pinned digest is what every build verifies.
- Signature status: no upstream detached signature or other signed provenance was verified for this
  intake. The lock therefore records provenance policy `not-published` with no evidence entries; the
  trust anchor is the pinned SHA-256 of the unmodified release tarball.
- `LICENSE` in this directory is the file of the same name from the top level of that archive,
  copied byte for byte.

The archive is used only to build the NASM assembler on the build host. No NASM file is installed
into the dependency prefix, linked, or shipped.
