# OpenH264 2.6.0 intake review

## Source and link-time boundary

The source component is BSD-2-Clause and is unmodified. The lock records a corresponding-source
obligation because Cisco's binary terms point end users to the matching source. The superbuild compiles it into a private
`libopenh264.so.8` link-time stub for FFmpeg's LGPL-compatible `libopenh264` wrapper. The private
library remains below the superbuild build tree; the qualified dependency prefix receives the
headers and a linker import script only. No OpenH264 library is installed into a distributable
runtime path.

The FFmpeg worker's resulting `DT_NEEDED` entry is `libopenh264.so.8` with no rpath to the private
stub. At launch, the host supplies a separately verified Cisco binary directory, and the worker
uses that directory before loading FFmpeg. The source-built library is never loaded at runtime.

The worker also installs a separate Bloom-owned `libopenh264.so.8` loader shim beside its private
FFmpeg libraries. The shim re-declares the six public C ABI entry points without including Cisco
or OpenH264 headers, exports no codec implementation, returns creator failure/null, and reports
version 0.0.0 with a shim marker. It is used for worker startup, decode, and fixture generation;
only software H.264 encode can shadow it with the host-verified Cisco binary.

## Runtime-fetched binary licence

Cisco's `BINARY_LICENSE.txt` is retained beside this record and shown before first installation.
The Cisco binary URL is HTTPS and is pinned in the reviewed lock to both the compressed archive
digest and the decompressed `libopenh264.so.8` digest. Bloom stores the end-user consent version
in `Bloom.conf`; it never fetches at startup or without explicit consent. The UI states that the
binary is separately fetched from Cisco and that the user can disable or remove it.

The BSD-2-Clause source licence does not itself grant the patent rights described in Cisco's
binary terms. The installed binary is therefore accepted only when its Cisco licence text is
shown, its URL and digest are visible in the consent flow, and the runtime asset is present only
in the user's per-user Bloom data directory.

## Qualification posture

Software H.264 is an export capability only when the host verifies the Cisco binary on every worker
launch. VA-API H.264/HEVC hardware encode is a separate capability and is advertised only after
the worker initializes an encode context. Neither capability is Apple authorization or archival
qualification.
