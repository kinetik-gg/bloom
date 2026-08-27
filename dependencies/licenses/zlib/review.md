# zlib 1.3.2 License Review

Reviewed: 2026-08-27

- SPDX expression: `Zlib`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: the license requires that altered sources be marked and the notice retained in
  source distributions; Bloom ships the exact license text and does not alter the source
  (`modified: false`, empty patch set). `copyrightFiles`/`noticeFiles` are empty by review: the
  copyright line lives inside the license text and the zlib license imposes no separate notice
  file. `sourceObligation: none`.
- Contrib licenses inside the archive (dotzlib, minizip) do not apply: the recipe builds only the
  core library; contrib code is neither compiled nor installed.
