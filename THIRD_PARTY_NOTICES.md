# Third-Party Notices

Bloom itself is licensed under the Apache License, Version 2.0 (see `LICENSE`). This file is the
repository-level inventory of third-party content vendored into this repository -- content whose
bytes are checked in here and shipped inside the Bloom binary or its resources.

Third-party libraries Bloom builds from pinned upstream source archives rather than vendoring are
inventoried separately in `dependencies/dependencies.lock.json`, with their license texts,
provenance, review, and security records under `dependencies/licenses/`.

## Vendored interface assets

| Component | Version | License | Files | Records |
| --- | --- | --- | --- | --- |
| Phosphor Icons (core assets) | v2.0.8 | MIT | `src/ui/kit/third_party/phosphor-icons/` | `LICENSE`, `provenance.md` |
| Plus Jakarta Sans | 2.7.1 | SIL Open Font License 1.1 | `src/ui/kit/third_party/plus-jakarta-sans/` | `LICENSE`, `provenance.md` |
| Geist Mono | v1.7.2 | SIL Open Font License 1.1 | `src/ui/kit/third_party/geist-mono/` | `LICENSE`, `provenance.md` |

Each component directory retains its upstream license text unmodified and a `provenance.md` that
records the exact upstream release, the source archive URL, the SHA-256 of the archive the assets
were taken from, and a SHA-256 for every vendored file.

### Phosphor Icons

Copyright (c) 2023 Phosphor Icons. Licensed under the MIT License. The complete license text is at
`src/ui/kit/third_party/phosphor-icons/LICENSE`.

Bloom vendors a curated 43-icon subset in the `regular` and `fill` weights, unmodified, from the
pinned `v2.0.8` release of <https://github.com/phosphor-icons/core>. The complete catalog is not
embedded.

### Plus Jakarta Sans

Copyright 2020 The Plus Jakarta Sans Project Authors
(<https://github.com/tokotype/PlusJakartaSans>). Licensed under the SIL Open Font License,
Version 1.1. The complete license text is at
`src/ui/kit/third_party/plus-jakarta-sans/LICENSE`, byte-identical to the upstream `OFL.txt`.

Bloom vendors the Regular, Medium, and SemiBold static TTFs, unmodified, from the pinned `2.7.1`
release. The variable font and the italic faces are not vendored.

### Geist Mono

Copyright 2024 The Geist Project Authors (<https://github.com/vercel/geist-font>). Licensed under
the SIL Open Font License, Version 1.1. The complete license text is at
`src/ui/kit/third_party/geist-mono/LICENSE`, byte-identical to the upstream `OFL.txt`.

Bloom vendors the Geist Mono Regular and Medium static TTFs, unmodified, from the pinned `v1.7.2`
release. The Geist sans family, the Geist Pixel family, the variable fonts, and the italic faces
are not vendored.
