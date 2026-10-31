# Third-Party Notices

Bloom itself is licensed under the Apache License, Version 2.0 (see `LICENSE`). This file is the
repository-level inventory of third-party content vendored into this repository -- content whose
bytes are checked in here and shipped inside the Bloom binary or its resources.

Third-party libraries Bloom builds from pinned upstream source archives rather than vendoring are
inventoried separately in `dependencies/dependencies.lock.json`, with their license texts,
provenance, review, and security records under `dependencies/licenses/`. One vendored component --
`stb_truetype` -- keeps its records in that same shape without a lock entry, because its bytes are
checked in here rather than acquired by the superbuild; it is listed below like every other vendored
component, and its own records explain why the lock cannot represent it.

## Lock-managed dependency intake

| Component | Version | License | Distribution boundary | Records |
| --- | --- | --- | --- | --- |
| nanobind | 3.0.1 | BSD-3-Clause | Precompiled static core for the optional Python bridge | `dependencies/licenses/nanobind/` |
| robin-map | 1.4.0 | MIT | Unmodified headers inside the nanobind archive and static core | `dependencies/licenses/robin_map/` |
| FFmpeg | 8.1.2 | LGPL-2.1-or-later | Shared libraries private to `bloom-media-worker` on Linux, macOS and Windows; ProRes is non-authorized preview output | `dependencies/licenses/ffmpeg/`, `dependencies/dependencies.lock.json` |
| OpenH264 | 2.6.0 | BSD-2-Clause | H.264 encoder built from source as a static archive inside the worker's `libavcodec`; ships with Bloom, nothing is downloaded | `dependencies/licenses/openh264/`, `dependencies/dependencies.lock.json` |
| nv-codec-headers | 12.1.14.1 | MIT | Header-only NVIDIA Video Codec SDK interface compiled into the worker's `libavcodec` for NVENC; the NVIDIA driver is loaded at run time and never shipped | `dependencies/licenses/nv-codec-headers/`, `dependencies/dependencies.lock.json` |
| Kvazaar | 2.3.2 | BSD-3-Clause | HEVC encoder built from source as a static archive inside the worker's `libavcodec`; ships with Bloom | `dependencies/licenses/kvazaar/`, `dependencies/dependencies.lock.json` |
| NASM | 2.16.03 | BSD-2-Clause | Build-host-only assembler for FFmpeg's x86 SIMD; installed outside the dependency prefix, never linked or shipped | `dependencies/licenses/nasm/`, `dependencies/dependencies.lock.json` |
| glslang | 16.4.0 | BSD-3-Clause AND BSD-2-Clause AND MIT AND Apache-2.0 AND GPL-3.0-or-later WITH Bison-exception-2.2 | Runtime `glslangValidator` staged beside the desktop/CLI/MCP executables under the private `bloom-gpu-tools/` directory, with its full `LICENSES/` set | `dependencies/licenses/glslang/`, `dependencies/dependencies.lock.json` |
| SPIRV-Tools | b707790a898e44038547df54580022fc1cf89c3d | Apache-2.0 | Runtime `spirv-val` staged beside the desktop/CLI/MCP executables under the private `bloom-gpu-tools/` directory, with the Apache-2.0 text | `dependencies/licenses/spirv-tools/`, `dependencies/dependencies.lock.json` |

glslang and SPIRV-Tools ship as Bloom's GPU colour-compilation tooling: they are staged by
`cmake/BloomGpuShaderTools.cmake` from the qualified prefix into the executable-relative
`bloom-gpu-tools/` directory, and each staged binary travels with its reviewed license texts and a
generated digest inventory. The glslang Bison Exception 2.2 covers the Bison-generated
`glslang_tab.cpp`/`.h` content; an independent counsel review of that exception remains a release
gate and no approval is asserted here.

FFmpeg's corresponding-source obligation is the exact locked official release archive plus the
reviewed patch under `dependencies/patches/ffmpeg/`. The lock, recipe, detached-signature evidence,
and license/review/security records are the authority for the configuration and source offer; the desktop `bloom` target does not link FFmpeg.

The bundled codec libraries (OpenH264, Kvazaar) are compiled from their locked source archives into
the worker's LGPL `libavcodec`; each archive is part of that library's corresponding source, and each
licence text is part of Bloom's third-party licence catalogue. Bloom uses no Cisco-distributed
binary.

### OpenH264

Copyright (c) 2013, Cisco Systems. All rights reserved. Redistribution and use in source and
binary forms, with or without modification, are permitted under the BSD-2-Clause terms in
`dependencies/licenses/openh264/LICENSE`.

### Kvazaar

Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors. All rights reserved.
Redistribution and use in source and binary forms, with or without modification, are permitted
under the BSD-3-Clause terms in `dependencies/licenses/kvazaar/LICENSE`. The compiled library
includes Colin Plumb's public-domain MD5 implementation; Windows builds also include Kvazaar's
thread wrapper (Copyright 2019 Tampere University, ISC licence) and NetBSD's `getopt` (Copyright
(c) 2000 The NetBSD Foundation, Inc., BSD-2-Clause), whose texts are in the Kvazaar source archive.

### nv-codec-headers

Copyright (c) 2010-2023 NVIDIA Corporation (and, for the dynamic loader, the FFmpeg contributors).
Permission is granted under the MIT terms in `dependencies/licenses/nv-codec-headers/LICENSE`.

## Vendored interface assets

| Component | Version | License | Files | Records |
| --- | --- | --- | --- | --- |
| Phosphor Icons (core assets) | v2.0.8 | MIT | `src/ui/kit/third_party/phosphor-icons/` | `LICENSE`, `provenance.md` |
| Inter | v4.1 | SIL Open Font License 1.1 | `src/ui/kit/third_party/inter/` | `LICENSE`, `manifest.json`, `provenance.md` |
| DejaVu Sans | 2.37 | Bitstream Vera Fonts Copyright + Arev Fonts Copyright, DejaVu changes public domain | `src/ui/kit/third_party/dejavu-sans/` | `LICENSE`, `provenance.md` |
| Geist Mono | v1.7.2 | SIL Open Font License 1.1 | `src/ui/kit/third_party/geist-mono/` | `LICENSE`, `manifest.json`, `provenance.md` |
| SND01 "sine" (UI sounds, processed) | SND01_sine.zip | snd.dev Terms of Use (snd-lib source code is MIT) | `src/ui/sound/assets/` | `src/ui/kit/third_party/snd01-sine/LICENSE`, `src/ui/sound/assets/provenance.md` |

Each component directory retains its upstream license text unmodified and a `provenance.md` that
records the exact upstream release, the source archive URL, the SHA-256 of the archive the assets
were taken from, and a SHA-256 for every vendored file.

### Phosphor Icons

Copyright (c) 2023 Phosphor Icons. Licensed under the MIT License. The complete license text is at
`src/ui/kit/third_party/phosphor-icons/LICENSE`.

Bloom vendors a curated 48-icon subset in the `regular`, `fill`, and `bold` weights, unmodified,
from the pinned `v2.0.8` release of <https://github.com/phosphor-icons/core>. The complete catalog
is not embedded.

### DejaVu Sans

Bitstream Vera Fonts Copyright (c) 2003 by Bitstream, Inc. Arev Fonts Copyright (c) 2006 by
Tavmjong Bah. DejaVu's own changes are in the public domain
(<https://github.com/dejavu-fonts/dejavu-fonts>). The Bitstream Vera and Arev licenses are
MIT-style permissive grants that allow reproduction and distribution, including inside a larger
software package, provided their notices travel with the fonts. The complete license text is at
`src/ui/kit/third_party/dejavu-sans/LICENSE`, byte-identical to the upstream `LICENSE`.

Bloom retains only the unmodified DejaVu Sans Book TTF from release `2.37`, solely for the
render module's text source. Interface typography uses Inter.

### Inter

Copyright 2016 The Inter Project Authors (<https://rsms.me/inter/>). Licensed under the
SIL Open Font License 1.1; complete text: `src/ui/kit/third_party/inter/LICENSE`.
Bloom bundles the unmodified upright variable font `InterVariable.ttf` and the unmodified
Regular, Medium and SemiBold static TTFs from release `v4.1`. The manifest and provenance beside
the files record computed archive and file digests. Interface typography uses the variable face
through Qt's application font database. The static faces are text-layer content only: the render
module embeds them at configure time for its Qt-free CPU text rasterizer; it does not read font
files at runtime or use Qt's font database. The embedded payloads are guarded by per-face
byte-count assertions in `src/render/CMakeLists.txt`.

### Geist Mono

Copyright 2024 The Geist Project Authors (<https://github.com/vercel/geist-font>). Licensed under
the SIL Open Font License, Version 1.1. The complete license text is at
`src/ui/kit/third_party/geist-mono/LICENSE`, byte-identical to the upstream `OFL.txt`.

Bloom vendors the upright Geist Mono variable TTF (`GeistMono[wght].ttf`, stored as
`GeistMono-Variable.ttf`), unmodified, from the pinned `v1.7.2` release. The Geist sans family,
the Geist Pixel family, the static faces, and the italic faces are not vendored.

### SND01 "sine" UI sounds

UI sounds: SND01 "sine" by Yasuhiro Tsuchiya, from SND (<https://snd.dev/>) by Dentsu Inc. CX
Creative Center. Licence: snd.dev Terms of Use (free use including commercial; credit
appreciated; no redistribution of the unprocessed materials by themselves; no use as a trademark
sound logo). The snd-lib project that distributes these sounds publishes its source code under the
MIT licence.

Bloom bundles 13 sounds from the pack, each processed (trimmed, resampled to 48 kHz,
level-matched, peak-limited, converted to 16-bit mono WAV) and embedded as application resources
for interface feedback. The original archive and unprocessed files are not redistributed. The
archive digest, the selection, the processing steps and every file digest are in
`src/ui/sound/assets/provenance.md`; the notice shown in the Open Source Licenses window is
`src/ui/kit/third_party/snd01-sine/LICENSE`.

## Vendored source libraries

| Component | Version | License | Files | Records |
| --- | --- | --- | --- | --- |
| stb_truetype | 1.26 (commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760`) | MIT OR Unlicense | `src/render/third_party/stb_truetype/` | `dependencies/licenses/stb_truetype/` |
| dr_wav | 0.14.6 (commit `019e75ba8e9ac47222dfb87671029812069f68dc`) | MIT-0 OR public domain | `src/media/third_party/dr_wav/` | `dependencies/licenses/dr_wav/` |
| minimp3 | 2026-07-27 snapshot (commit `ea99364f61c14656440e8d77e9c233ccf3124633`) | CC0 1.0 | `src/media/third_party/minimp3/` | `dependencies/licenses/minimp3/` |
| miniaudio | 0.11.25 (commit `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`) | MIT-0 OR public domain | `src/media/third_party/miniaudio/` | `dependencies/licenses/miniaudio/` |

| stb_image | 2.30 (commit `013ac3beddff3dbffafd5177e7972067cd2b5083`) | MIT OR Unlicense | `src/media/third_party/stb_image/` | `dependencies/licenses/stb_image/` |

### stb_truetype

Copyright (c) 2017 Sean Barrett (<https://github.com/nothings/stb>). Dual-licensed at the
recipient's choice under the MIT License or released into the public domain (Unlicense); Bloom
distributes under the MIT alternative and ships the notice it requires. The complete license text is
at `src/render/third_party/stb_truetype/LICENSE`, byte-identical to the upstream `LICENSE` at the
pinned commit, and again at `dependencies/licenses/stb_truetype/LICENSE`.

Bloom vendors the single `stb_truetype.h` header, unmodified, and compiles it into `bloom_render`
for Qt-free CPU glyph rasterization on the reference evaluation path. It is not a superbuild
dependency and has no entry in `dependencies/dependencies.lock.json`: nothing is downloaded by a
build and nothing is installed into a qualified prefix. Its acquisition provenance, license review,
and security review live under `dependencies/licenses/stb_truetype/`; the security review records
that this library is qualified only for the font bytes Bloom itself pins, never for an untrusted
font file.

### Audio headers

Bloom vendors `dr_wav`, `minimp3`, and `miniaudio` as unmodified single-header sources for the v0
audio engine. Each is compiled through one Bloom-owned translation unit and private object-library
target; none is a superbuild dependency or has a `dependencies/dependencies.lock.json` entry. The
component records under `dependencies/licenses/{dr_wav,minimp3,miniaudio}/` retain the exact license
bytes, per-file SHA-256 provenance, license review, and in-process security disposition. The audio
security boundary is deliberately narrow: only WAV/MP3 paths reach the bounded file decoders, and
the device backend receives decoded samples rather than media paths.
