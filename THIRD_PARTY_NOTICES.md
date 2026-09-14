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

## Vendored interface assets

| Component | Version | License | Files | Records |
| --- | --- | --- | --- | --- |
| Phosphor Icons (core assets) | v2.0.8 | MIT | `src/ui/kit/third_party/phosphor-icons/` | `LICENSE`, `provenance.md` |
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

Bloom vendors the DejaVu Sans Book, Bold, and Oblique TTFs, unmodified, from the pinned `2.37`
release. DejaVu Serif, DejaVu Sans Mono, the Condensed and ExtraLight cuts, and DejaVu Math TeX
Gyre are not vendored.

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
