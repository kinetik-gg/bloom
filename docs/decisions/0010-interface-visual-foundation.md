# ADR 0010: Interface Visual Foundation

Status: accepted

Date: 2026-08-25

## Context

Bloom's dense editor shell needs consistent icons and typography before individual panels grow
their own visual conventions. The application is native C++/Qt software, so using an icon or font
family must not introduce a JavaScript runtime or unrelated web build pipeline.

## Decision

- Adopt Phosphor Icons as Bloom's default interface icon family.
- Use curated, pinned raw SVG assets in the main application rather than framework-specific icon
  packages or the complete catalog.
- Reserve a separately compiled Qt resource pack for a future workflow that genuinely needs the
  complete catalog.
- Adopt Plus Jakarta Sans as Bloom's primary interface typeface.
- Adopt Geist Mono as Bloom's monospaced interface typeface.
- Vendor pinned native font assets and their SIL Open Font License 1.1 texts.
- Centralize icon and typography access behind semantic UI roles in `src/ui`.
- Keep accessible names, labels, scale behavior, and safe platform fallbacks independent of the
  chosen glyph or font asset.

The implementation and usage contract is defined in
[`../ux/visual-language.md`](../ux/visual-language.md).

## Consequences

- Bloom's source and runtime remain C++/Qt-only for interface assets.
- The main binary and ordinary rebuilds do not absorb thousands of unused icon files.
- Third-party license texts and upstream provenance become required distribution artifacts.
- Qt SVG and application-font loading become UI foundation capabilities when the first real icon
  and font assets are integrated.
- Visual regressions need checks at dense control sizes, high-DPI scale factors, disabled states,
  and on Linux, macOS, and Windows.
- Platform-managed application chrome may use the operating-system font while Bloom-controlled
  surfaces use the selected families.
