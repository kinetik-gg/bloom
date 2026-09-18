# OpenH264 2.6.0 provenance

- Upstream source release: OpenH264 v2.6.0, released 2025-02-12.
- Selection rationale: v2.6.0 is the newest Cisco OpenH264 release with a reachable Cisco
  `linux64.8` redistributed binary at this intake date. v2.5.1 is newer than earlier releases
  but does not publish a reachable Cisco linux64 binary at the selected ABI; Bloom therefore
  selects the exact v2.6.0 source/binary pair rather than guessing a compatible ABI.
- Source archive URL: <https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz>
- Source archive retrieval date: 2026-09-18 (UTC date)
- Source archive size: 60,302,243 bytes
- Source archive SHA-256: `sha256:558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`
- Cisco binary archive URL: <https://ciscobinary.openh264.org/libopenh264-2.6.0-linux64.8.so.bz2>
- Cisco binary archive retrieval date: 2026-09-18 (UTC date)
- Cisco binary archive size: 634,264 bytes
- Cisco binary archive SHA-256: `sha256:27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b`
- Decompressed Cisco library SHA-256: `sha256:2f0cde7c6a6abcf5cae76942894ea42897fa677bce4ed6c91a24dd1b041d5f04`
- Decompressed library identity: ELF SONAME `libopenh264.so.8`, matching the FFmpeg wrapper's
  dynamic `DT_NEEDED` name. The decompressed library is never checked into the repository,
  prefix, or distributable; it is fetched directly to the user's per-user Bloom data directory.
- Binary licence URL: <https://www.openh264.org/BINARY_LICENSE.txt>
- Binary licence retrieval date: 2026-09-18 (UTC date)
- Binary licence SHA-256: `sha256:bd9f363c5ea11ef723d0304cddacb5273c43c0e1194097c7a045d05273635418`

The source archive is used only to build the private link-time OpenH264 stub needed to configure
and link FFmpeg. The runtime binary is Cisco's separately downloaded artifact; Bloom does not
rebuild, bundle, or substitute it.

The worker's always-installed loader shim is not an OpenH264 build or substitute codec. It is
Bloom-owned loader-boundary code with re-declared public ABI signatures only; it exists so FFmpeg
can load for native H.264/ProRes decode and fixture generation before a user installs Cisco's
encoder binary.
