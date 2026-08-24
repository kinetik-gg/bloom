# ADR 0003: Foundation Scope Follows The Vertical Proof

Status: accepted

Date: 2026-08-25

## Context

Bloom has broad professional goals spanning compositing, motion, editing, color, media, rendering,
and extensibility. Creating complete subsystem structures before proving the central artist workflow
would increase complexity without validating the product.

## Decision

- Build the first architecture around the vertical proof in `docs/product/foundation.md`.
- Create source boundaries when the proof needs them rather than adding empty speculative modules.
- Preserve professional correctness constraints early, while deferring unrelated feature breadth.
- Require each milestone to demonstrate an artist-visible workflow and durable automated behavior.

## Consequences

- The initial source tree remains small.
- Standards, concurrency, platform parity, and render correctness still influence foundations even
  when their full feature sets are deferred.
- A subsystem proposal must explain which accepted workflow or quality constraint it enables.
