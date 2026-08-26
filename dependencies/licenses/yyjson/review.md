# yyjson 0.12.0 License Review

Reviewed: 2026-08-26

## License Identification

- SPDX expression: `MIT`
- License text: `LICENSE` beside this record — exact bytes extracted from the acquired release
  archive, digest-bound by the production lock.
- Copyright holder: YaoYuan (ibireme), 2020, stated inside the MIT license text itself.

## Obligation Review

- **Attribution/notice**: MIT requires the copyright notice and permission notice to accompany
  copies or substantial portions of the Software. Shipping the bound `LICENSE` bytes with
  distributed builds satisfies this. yyjson publishes no separate `COPYRIGHT` or `NOTICE` file,
  so the lock's `copyrightFiles` and `noticeFiles` arrays are empty by review: the copyright line
  lives inside the license text and no additional notice obligation exists.
- **Source obligation**: none. MIT imposes no corresponding-source requirement
  (`sourceObligation: none`).
- **Modification**: the locked source is unmodified (`modified: false`, empty patch set).
- **Compatibility**: MIT is compatible with Bloom's Apache-2.0 distribution and with static
  linking into community builds under ADR 0014's obligations.

## Toolchain Identity Note

The locked profile `bloom.linux-x86-64-clang-release` records the reviewed environment that
produced and smoke-verified the staging prefix on 2026-08-26 (clang 22.1.8, cmake 4.4.2,
ninja 1.13.2, GNU ld 2.47), targeting the CI floor of Ubuntu 24.04 (glibc 2.39, libstdc++ from
GCC 13/14 era, pinned LLVM 22 toolchain). Consume-phase enforcement in v1 verifies the
ABI-relevant subset (compiler family and major version, C++ standard, standard library); exact
tool-identity capture tightens when the qualify phase writes the production prefix manifest.
