# ADR 0014: Apache-2.0 And Dynamic Qt Distribution

Status: accepted

Date: 2026-08-25

## Context

Bloom needs an explicit source license before accepting outside contributions or distributing
builds. The license should support open development, studio pipeline integration, commercial and
open add-ons, and future native extension boundaries without creating uncertainty merely because a
tool links to the public SDK.

Qt is available under commercial and open-source terms. The Qt modules Bloom currently uses are
available under LGPLv3. Some optional Qt modules are GPL-only for open-source users, so choosing one
Qt module can change distribution obligations.

## Decision

- License original Bloom source and documentation under the Apache License, Version 2.0.
- Retain the full license in the repository root and ship `LICENSE` and `NOTICE` with source and
  binary distributions.
- Treat every third-party library, font, icon, generated artifact, and bundled tool as separately
  licensed. Keep exact version, source, license, modification, and distribution records.
- Use dynamically linked LGPLv3 Qt libraries for community release packages unless a later decision
  selects commercial Qt terms.
- Bundle the qualified Qt libraries and plugins with native Bloom packages. Artists never install a
  development SDK, select a Qt version, or compile Bloom as part of normal installation.
- Preserve the LGPL notices and exact corresponding Qt source or durable source offer, document how
  the shipped libraries can be replaced, and do not impose technical restrictions that remove the
  recipient's LGPL rights.
- Reject a GPL-only Qt module from a community build unless an accepted decision explicitly changes
  the dependency or product-license strategy. License qualification is part of dependency review.
- Generate a third-party notice inventory and software bill of materials for release artifacts.

## Platform Packaging Direction

- Windows packages ship the required Qt DLLs and plugins beside Bloom through a signed installer.
- macOS packages embed the required dynamic Qt frameworks and plugins in a signed and notarized
  application bundle.
- Linux packages bundle a qualified Qt runtime in a self-contained application package and avoid an
  uncontrolled dependency on the distribution's system Qt.

Packaging stays reproducible and offline-capable after downloading the release artifact. Dynamic
linking is not a request for an artist to install dependencies or build from source.

## Consequences

- Bloom remains permissively licensed and includes an explicit contributor patent grant.
- Proprietary and open pipeline tools or add-ons may use Bloom's public interfaces under their own
  terms, subject to the licenses of code they actually copy or modify.
- Release engineering must retain notices, corresponding-source information, build provenance, and
  replacement instructions for shipped LGPL components.
- New Qt and third-party dependencies cannot enter a release based only on technical suitability;
  their exact distribution terms are an acceptance gate.

Primary references:

- [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0.html)
- [Applying the Apache License](https://www.apache.org/legal/apply-license.html)
- [Qt licensing](https://doc.qt.io/qt-6/licensing.html)
- [Qt GPL and LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
- [Qt deployment](https://doc.qt.io/qt-6/deployment.html)
