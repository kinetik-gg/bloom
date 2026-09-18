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
| FFmpeg | 8.1.2 | LGPL-2.1-or-later | Shared libraries for the future Linux `bloom-media-worker` only; ProRes is non-authorized preview output | `dependencies/licenses/ffmpeg/`, `dependencies/dependencies.lock.json` |
| OpenH264 | 2.6.0 | BSD-2-Clause plus Cisco binary terms | Private source-built FFmpeg link-time stub; Cisco `libopenh264.so.8` is fetched by the end user's machine after consent and is never bundled | `dependencies/licenses/openh264/`, `dependencies/dependencies.lock.json` |

FFmpeg's corresponding-source obligation is the exact locked official release archive. The lock,
recipe, detached-signature evidence, and license/review/security records are the authority for the
configuration and source offer; the desktop `bloom` target does not link FFmpeg.

OpenH264's corresponding source is the locked Cisco v2.6.0 source archive. The redistributed
runtime binary is downloaded only from Cisco's HTTPS endpoint, verified against the lock's archive
and decompressed-library digests, and installed in a per-user directory. Bloom does not bundle or
load the private source-built stub at runtime. Bloom's separate `libopenh264.so.8` loader shim is
Bloom-owned code with six re-declared public ABI signatures, no Cisco implementation, and no
third-party runtime contents; it keeps the worker loadable for decode when the Cisco binary is
absent.

## Vendored interface assets

| Component | Version | License | Files | Records |
| --- | --- | --- | --- | --- |
| Phosphor Icons (core assets) | v2.0.8 | MIT | `src/ui/kit/third_party/phosphor-icons/` | `LICENSE`, `provenance.md` |
| Inter | v4.1 | SIL Open Font License 1.1 | `src/ui/kit/third_party/inter/` | `LICENSE`, `manifest.json`, `provenance.md` |
| DejaVu Sans | 2.37 | Bitstream Vera Fonts Copyright + Arev Fonts Copyright, DejaVu changes public domain | `src/ui/kit/third_party/dejavu-sans/` | `LICENSE`, `provenance.md` |
| Geist Mono | v1.7.2 | SIL Open Font License 1.1 | `src/ui/kit/third_party/geist-mono/` | `LICENSE`, `provenance.md` |

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
Bloom bundles unmodified Regular, Medium and SemiBold static TTFs from release `v4.1`.
The manifest and provenance beside the files record computed archive and file digests.
The render module embeds those same three vendored TTFs at configure time for its Qt-free CPU text
rasterizer; it does not read font files at runtime or use Qt's font database. The embedded payloads
are guarded by per-face byte-count assertions in `src/render/CMakeLists.txt`.

### Geist Mono

Copyright 2024 The Geist Project Authors (<https://github.com/vercel/geist-font>). Licensed under
the SIL Open Font License, Version 1.1. The complete license text is at
`src/ui/kit/third_party/geist-mono/LICENSE`, byte-identical to the upstream `OFL.txt`.

Bloom vendors the Geist Mono Regular and Medium static TTFs, unmodified, from the pinned `v1.7.2`
release. The Geist sans family, the Geist Pixel family, the variable fonts, and the italic faces
are not vendored.

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
