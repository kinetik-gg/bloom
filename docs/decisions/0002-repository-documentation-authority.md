# ADR 0002: Repository Documentation Is Canonical

Status: accepted

Date: 2026-08-25

## Context

Bloom planning may also exist outside the source repository. Maintaining two complete,
independently edited documentation sets would make authority ambiguous.

## Decision

- The `docs/` directory in this repository is Bloom's canonical current documentation.
- External memory may retain routing, historical context, and concise status, but it is not the
  normative architecture source.
- External decisions are adopted selectively and explicitly in repository documents.
- Repository documentation distinguishes accepted decisions, working direction, proposals, and
  superseded material.

## Consequences

- Code and its governing documentation can change in the same review.
- External research remains useful without silently controlling implementation.
- The external Bloom memory corpus does not need an immediate full rewrite.
