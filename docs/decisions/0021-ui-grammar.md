# ADR 0021: Mechanical UI grammar

Status: accepted

Date: 2026-09-14

## Context

The owner requested: "I need sensible or mechanical UI rules. I don't want to adjust each
component one by one. Clearly our system let arbitrary code design whatever the agent is
thinking. Layer control buttons are off, pixelated and unusable; dropdowns in panel headers have
different dimensions from the panel picker — they should have the same styling. Go and fix this
systemic problem."

Independent Qt controls, panel-switcher painting, header-collapse implementations and timeline
glyph painters allowed inconsistent sizes and fractional-DPR rasterization defects. A local
style fix cannot prevent recurrence.

## Decision

Adopt [the UI grammar](../ux/ui-grammar.md): one kit vocabulary, a token metric grid,
DPR-exact SVG icons, TypeRole-only Inter/Geist Mono typography, declared EditorArea-owned
chrome, shared list and property rows, canvas-only custom painting and whole-window goldens.
Repository checks reject new violations while a line-addressed allowlist makes existing debt
visible. Existing object names and document/command semantics remain stable.

## Consequences

Panels describe controls and actions rather than owning chrome layout or overflow behavior.
Kit changes affect every consumer and therefore require metric and whole-window validation.
Inter changes interface text metrics; render text retains its separate face. The remaining
allowlist is explicit GRAMMAR-2 work, not a competing design language. Linux, macOS and Windows
share Qt layout and SVG rasterization; platform qualification remains required.

## Extending the grammar

Before adding a control, identify the missing interaction and why an existing kit class cannot
represent it. Add the class inside the kit, bind its typography and dimensions to roles, test
keyboard/pointer behavior and fractional DPR, and update the vocabulary here and in the grammar.
Before adding a token, define its semantic owner and all intended consumers in the grammar's
metric table, add its single implementation in tokens.hpp, and test the mapping. Do not add
per-editor aliases for arbitrary dimensions. Deliberate visual changes regenerate references
with `--update-goldens`; the commit explains why. Never silently widen the debt allowlist.
