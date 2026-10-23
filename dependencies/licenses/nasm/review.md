# NASM 2.16.03 intake review

## Role and boundary

NASM is an x86 assembler used solely to assemble FFmpeg's hand-written x86 SIMD routines. The
superbuild builds it from the pinned source archive and installs it into a superbuild-private tools
directory (`<superbuild build dir>/tools`), never into the dependency prefix. FFmpeg's configure is
pointed at that binary with `--x86asmexe`. NASM is not linked into, loaded by, or shipped with any
Bloom artifact; the assembled object code in FFmpeg is the output of the tool, not NASM code.
The recipe defines the component only when the target processor is x86 or x86-64; other targets use
FFmpeg's inline and NEON assembly and need no assembler. CI images and developer machines need no
system `nasm` or `yasm`.

## Licence

The source is BSD-2-Clause (see `LICENSE`) and is unmodified. There is no source or notice
obligation for the build-only tool, so the lock records `sourceObligation: none`, an empty
`shippingRoles` array, and linkage `executable` (the schema's vocabulary for a non-shipping build
tool). The lock dependency from FFmpeg to NASM uses the `build` relationship.

## Qualification posture

No capability beyond building FFmpeg's x86 SIMD is claimed. Output correctness remains FFmpeg's
responsibility and is exercised through the FFmpeg-backed media tests.
