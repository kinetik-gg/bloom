# ADR 0001: C++ And Qt Foundation

Status: accepted

Date: 2026-08-25

## Context

Bloom needs a mature native toolkit for a dense professional application surface and direct access
to a media and VFX ecosystem whose major libraries and standards expose mature C and C++ interfaces.

## Decision

- Implement Bloom in C++20.
- Use Qt 6 Widgets for the initial desktop shell.
- Use CMake as the build system.
- Keep Qt at the application and UI boundary rather than making Qt types part of the canonical
  document or render model.
- Implement custom creative editor surfaces inside the Qt shell as Bloom's workflows require.

## Consequences

- Bloom can use a mature cross-platform desktop toolkit immediately.
- Native C/C++ media and VFX integrations avoid a language bridge in the main implementation.
- C++ ownership and lifetime safety require explicit discipline, tooling, and tests.
- The renderer remains replaceable and headless rather than becoming a Qt painting subsystem.
