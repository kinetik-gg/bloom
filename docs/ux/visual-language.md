# Visual Language

Status: accepted

Updated: 2026-08-25

## Purpose

Bloom needs a coherent visual foundation for a dense professional interface. Icons and typography
must remain legible at small sizes, work across Linux, macOS, and Windows, and be usable without a
web runtime or JavaScript toolchain.

This document owns Bloom's current iconography and interface-type decisions. Component styling,
color tokens, spacing, and motion behavior will be added here as they become concrete.

## Iconography

Phosphor Icons is Bloom's default interface icon family.

- Use the official raw SVG assets. Bloom does not depend on a JavaScript, Node, npm, or web runtime
  to obtain or render icons.
- Vendor a curated set of icons used by the product, pinned to an upstream release or commit. Do
  not use a Git submodule or download interface assets while configuring, building, or launching
  Bloom.
- Retain the upstream MIT license and record asset provenance beside the vendored files.
- Preserve vendored upstream SVGs unchanged and enumerate them explicitly with `qt_add_resources`
  under a Bloom-owned resource prefix.
- Use `regular` as the default visual weight and `fill` for selected or toggled states. Add another
  weight only when testing shows a concrete legibility need at Bloom's supported control sizes.
- Access icons through a typed, semantic C++ API such as `IconId::SplitHorizontal`. Product code
  must not spread upstream filenames or resource paths through widgets.
- Render and tint SVGs through Qt, with caching that accounts for icon identity, size, state,
  palette color, and device-pixel ratio. Resolve Phosphor's `currentColor` from the applicable Qt
  palette role; do not assume the raw SVG automatically follows the application palette.
- An icon does not replace an accessible name. Icon-only controls require a tooltip and accessible
  text; destructive or ambiguous actions should include a visible label where space permits.
- Do not communicate an important state through icon color alone.

The complete Phosphor catalog is not embedded in Bloom's main executable by default. If a concrete
workflow later needs the entire catalog, package it as a pinned external Qt `.rcc` resource and load
it through `QResource`. This preserves a native C++/Qt build and avoids making every application
build compile thousands of unused assets.

Official sources:

- [Phosphor core assets](https://github.com/phosphor-icons/core)
- [Phosphor MIT license](https://github.com/phosphor-icons/core/blob/main/LICENSE)
- [Qt resource system](https://doc.qt.io/qt-6/resources.html)

## Typography

Plus Jakarta Sans is Bloom's primary interface typeface. Geist Mono is Bloom's monospaced
typeface.

Use Plus Jakarta Sans for:

- menus, editor headers, controls, labels, dialogs, properties, and timeline text
- headings and ordinary artist-facing documentation rendered inside the application
- numeric controls when proportional text is appropriate

Use Geist Mono for:

- code, expressions, scripts, logs, console output, and technical identifiers
- timecode, frame counters, channel values, and aligned numeric readouts where fixed character
  widths materially improve scanning

Do not use the monospaced family as a decorative substitute for hierarchy. Weight, spacing, and
layout should carry hierarchy in the normal interface.

## Font Packaging And Loading

- Vendor native font assets from pinned upstream releases; do not use a Git submodule, install them
  through npm, or fetch them from a network while configuring, building, or launching Bloom.
- Retain each upstream SIL Open Font License 1.1 file and record the exact version or commit.
- Prefer upstream variable TTF assets when they behave consistently through the supported Qt
  version on all three platforms. Keep a tested static-font fallback if variable-font behavior or
  packaging differs.
- Load bundled fonts through Qt's application font facilities. A missing or invalid bundled font
  must produce a diagnostic and fall back to the platform sans-serif or monospace family rather
  than preventing Bloom from opening.
- Let Qt provide glyph fallback for writing systems not covered by the bundled families.
- Use device-independent font sizes and verify the interface at common fractional and high-DPI
  scale factors. Do not rasterize interface text into image assets.
- Platform-owned chrome may retain its operating-system typeface when the platform does not allow
  application font control. Bloom does not introduce platform-specific menu or window chrome solely
  to force typography.

Initial interface weights are Regular, Medium, and SemiBold for Plus Jakarta Sans, and Regular and
Medium for Geist Mono. Additional weights or italics should enter the shipped set only when an
implemented component uses them.

Official sources:

- [Plus Jakarta Sans](https://github.com/tokotype/PlusJakartaSans)
- [Geist and Geist Mono](https://github.com/vercel/geist-font)

## Ownership Boundary

Icon IDs, font roles, palette roles, and rendering helpers belong to `src/ui`. UI components consume
these semantic roles rather than owning independent asset-loading or font-selection logic.
Optional editor modules use the same host-owned visual roles for standard controls. A module may
provide domain-specific artwork through an explicit registered resource boundary, but it must not
silently replace Bloom's global visual language.

Project documents never store an interface font or icon choice as render-affecting state. Fonts
selected by artists for composition content are project assets and follow a separate media,
licensing, substitution, and missing-dependency workflow.
