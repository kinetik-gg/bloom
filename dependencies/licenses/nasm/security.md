# NASM 2.16.03 security review

Reviewed 2026-10-23. NASM runs only on the build host against FFmpeg's own checked-in assembly
sources; it never processes untrusted input and does not ship with Bloom. No NASM vulnerability
affects that use, so none is listed in the lock and no local patches are applied.

Update policy: when the pinned version is replaced, refresh the archive digest, provenance,
licence, review, security record, lock, and recipe together.
