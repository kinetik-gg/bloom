# stb_truetype 1.26 Provenance (Vendored In-Tree Header)

Reviewed: 2026-09-12

## Source

stb_truetype is a single public-domain/MIT header. It is not acquired by the superbuild and
installs nothing into a qualified prefix: its bytes are checked into this repository and compiled
directly into `bloom_render`, exactly like the interface assets under
`src/ui/kit/third_party/`.

- Upstream project: <https://github.com/nothings/stb>
- Declared library version: `1.26`, read from the header's own first line
  (`// stb_truetype.h - v1.26 - public domain`). Upstream publishes no release tag or archive for
  the individual headers, so the immutable identity below is the repository commit, not a tag.
- Pinned commit: `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` — the most recent upstream commit that
  touches `stb_truetype.h` (committed 2024-07-15). Both files below were fetched from that one
  immutable tree, never from `master`.
- Header URL:
  `https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h`
- License URL:
  `https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/LICENSE`

## Acquisition Record

Acquired 2026-09-12 over HTTPS. Every digest below was computed with `sha256sum` over the exact
bytes that came back from those two URLs, before they were copied into the repository — never read
from an upstream publication and never typed from memory. The vendored files are those bytes
unchanged.

| Vendored path | Bytes | SHA-256 |
| --- | --- | --- |
| `src/render/third_party/stb_truetype/stb_truetype.h` | 199192 | `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab` |
| `src/render/third_party/stb_truetype/LICENSE` | 2510 | `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |
| `dependencies/licenses/stb_truetype/LICENSE` | 2510 | `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |

The byte-identical copy fetched from `master` on the same day has the same header digest, which
confirms the pinned commit is the content upstream currently serves rather than a stale revision.

- Modified: No. The header is byte-identical to the pinned upstream file; Bloom applies no patch.
  Every Bloom-owned decision (the `STB_TRUETYPE_IMPLEMENTATION` translation unit, its allocator
  macros, and its warning suppression) lives in Bloom's own files beside it, never inside this one.
- `provenancePolicy`: not published. Upstream publishes no detached signature, Sigstore bundle, or
  signed tag for these files at this commit.
- Archive inspection does not apply: nothing was extracted. Two regular files were downloaded
  directly, no archive, no symbolic links.

## License

Dual-licensed at the recipient's choice: MIT (copyright 2017 Sean Barrett) **or** public domain
(Unlicense). `LICENSE` beside this file holds the exact bytes of the upstream `LICENSE` at the
pinned commit, and the same bytes sit beside the vendored header at
`src/render/third_party/stb_truetype/LICENSE`. See `review.md` for the chosen expression.

## Status

NOT QUALIFIED, and deliberately not a lock component. `docs/architecture/dependency-intake.md`
requires a component record for every dependency reached *through the superbuild's*
`acquire`/`build`/`qualify`/`consume` pipeline — the lock's `components[]` entries each carry a
recipe, `profileBuilds`, a `consumerAbi`, and installed prefix artifacts validated by the prefix
manifest. An in-tree header has none of those and installs nothing, so it cannot be expressed in
the lock v1 shape without inventing a recipe that does not exist. The repository's existing
precedent for checked-in vendored content is `src/ui/kit/third_party/{phosphor-icons,dejavu-sans,
geist-mono}`: records beside or about the files, an entry in `THIRD_PARTY_NOTICES.md`, and no lock
entry. This record follows that precedent while using the component-record shape of
`dependencies/licenses/<component>/` (license text + provenance + review + security) so the
"Open Source Licenses…" catalog, which discovers `dependencies/licenses/*/LICENSE`, ships this
license text with the application. Re-review on any upstream change to the pinned commit.
