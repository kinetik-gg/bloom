# stb_image license and feature review

Reviewed: 2026-09-15

SPDX: MIT OR Unlicense. Bloom chooses MIT and ships the unchanged license and attribution.
No source disclosure obligation; no patches or transitive dependencies. This follows the
stb_truetype in-tree header precedent, without a dependency lock entry.

The portable C++ adapter enables only PNG and JPEG, disables stdio, HDR, linear-loader helpers
and SIMD, and uses a single private implementation translation unit. No stb type escapes the
Bloom-owned header. Third-party warnings are suppressed only on bloom_media_stb_image; Bloom
adapter code uses the normal warnings. Strict floating-point settings apply to both targets.
