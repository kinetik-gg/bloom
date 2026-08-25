# Unicode 15.1.0 Character Database Provenance

Reviewed: 2026-08-26

## Source

- Publisher: Unicode Consortium
- Archive: `https://www.unicode.org/Public/15.1.0/ucd/UCD.zip`
- SHA-256 of the acquired archive:
  `cb1c663d053926500cd501229736045752713a066bd75802098598b7a7056177`
- Acquired 2026-08-26 over HTTPS; digests computed from the exact downloaded bytes.

## Checked-In Files

The five files under `15.1.0/` are the exact bytes extracted from the archive above, in the fixed
order required by the dependency-intake contract's `unicodeProfile`:

| Path | Bytes | SHA-256 |
| --- | ---: | --- |
| `UnicodeData.txt` | 1914200 | `2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86` |
| `CompositionExclusions.txt` | 8888 | `59d2d9e3dfdf0a999cf9dae11d594f053631222679a2f5710315ea07f7fe82af` |
| `DerivedNormalizationProps.txt` | 1355530 | `8875dccee2bc1a7c1fe568a3b502a9e78c9e0495afd96b6568b4294d0ed1f7e1` |
| `CaseFolding.txt` | 84870 | `4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf` |
| `NormalizationTest.txt` | 2625136 | `871238e37e3be0696ec2bd0891119a041b052da1a84485eda05a5438724b223e` |

The repository `.gitattributes` marks this tree `-text` so the digest-bound bytes are never
line-ending normalized.

## Status

These records provide the reviewed official bytes required by the contract's Unicode 15.1
bootstrap. The dependency-artifact checker's bootstrap allowlist constants and golden self-tests
that consume them land with the production lock validator; until that two-source equality check
exists and passes, no production lock may claim these tables.
