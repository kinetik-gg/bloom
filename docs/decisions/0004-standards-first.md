# ADR 0004: Standards-First Interchange

Status: accepted

Date: 2026-08-25

## Context

Bloom needs to participate in professional motion-graphics and VFX pipelines across Linux, macOS,
and Windows. Those pipelines already use open specifications and open-source ecosystem projects for
color, high-dynamic-range images, editorial interchange, image effects, scenes, and materials.
Inventing Bloom-only equivalents would increase artist friction, narrow pipeline compatibility, and
create long-term maintenance work.

At the same time, a library is not necessarily a standard, no interchange format represents Bloom's
entire authoring model, and adopting an ecosystem name does not guarantee semantic preservation.

## Decision

- Investigate and prefer an applicable, maintained open industry specification before defining a
  custom interchange format, schema, plug-in ABI, color convention, or pipeline protocol.
- Distinguish specifications, workflows, file formats, reference implementations, implementation
  libraries, and compatibility baselines in documentation and product claims.
- Keep `.bloom` as Bloom's schema-versioned authoring format; do not market it as a replacement for
  open interchange standards.
- Require importers and exporters to declare supported feature subsets and report preservation,
  approximation, omission, and missing external dependencies.
- Never silently degrade color, time, alpha, channels, effects, media references, or final-render
  quality to make an interchange operation appear successful.
- Use namespaced and versioned Bloom extensions only where the target standard supports extension,
  and preserve unknown extension data where practical and safe.
- Require a separate ADR for a custom interchange contract. It must document the standards evaluated,
  the unmet requirement, compatibility consequences, and an extension or migration path.
- Adopt a standard only when its owning product workflow enters scope; standards-first does not
  justify speculative subsystem breadth.

The current adoption map and review criteria live in
[`../standards/strategy.md`](../standards/strategy.md).

## Consequences

- Bloom projects can remain richer than interchange documents without misleading artists about what
  will survive a handoff.
- OCIO, OpenEXR, OIIO, OTIO, OpenFX, and other ecosystem projects receive precise roles rather than
  becoming interchangeable buzzwords.
- Adapters and dependency upgrades require conformance, preservation, cross-platform, security, and
  licensing work in addition to basic read/write tests.
- Some exports will require an explicit conversion or artist-approved loss report.
- Deferred standards integrations remain documented directions rather than premature dependencies.
