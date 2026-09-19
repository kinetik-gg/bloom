# Bloom GPU Shaders

Offline generator for the first fixed Bloom display compute shader: **Bloom Neutral v1 display**
(`OcioDisplayV1` / `BloomNeutralV1Display`), built from the immutable
`assets/ocio/neutral-v1/config.ocio` payload.

This project is configured and built out-of-tree as a standalone tool. It performs no runtime
shader compilation and no GPU execution, and produces a durable offline artifact only.

## What the generator does

1. Reads exactly one explicitly-passed asset path.
2. Verifies the raw file SHA-256 against the configure-time pin and independently rebuilds the
   `BloomOcioRevision` v1 envelope digest (the two digests are distinct values).
3. Parses that exact byte sequence with OCIO using a freshly created, never-environment-loaded
   empty `Context`, and validates it.
4. Builds the same default `DisplayViewTransform` the CPU display path builds
   (`lin_rec709_scene` → `srgb_rec709_display` / view `srgb_rec709_display`, forward direction).
5. Extracts the shader with `OCIO::GpuShaderDesc`, `GPU_LANGUAGE_GLSL_VK_4_6`.
6. Rejects any texture, 3D texture, uniform, uniform buffer, or dynamic property. This bounded
   first shader accepts none.
7. Emits a deterministic `#version 460` compute shader plus a manifest. No machine paths or
   timestamps are written.

## Build

Use the same locked dependency prefix as the application dependency build (it owns both
OpenColorIO 2.5.2 and the pinned `glslangValidator`/`spirv-val`). System OpenColorIO is refused.

```sh
cmake -S tools/gpu-shaders -B build/gpu-shaders -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DBLOOM_DEPENDENCY_PREFIX="$PWD/build/dependency-prefix"
cmake --build build/gpu-shaders
```

The generator reuses the tested `bloom::core::Sha256Hasher` from `src/core/sha256.cpp` (compiled
read-only into this standalone target); it does not carry a private hash implementation. It
compiles as strict C++20 (`-Wall -Wextra -Wpedantic -Werror`).

OpenColorIO is resolved at exactly `2.5.2` from the locked prefix only. The configure step mirrors
the qualified-mode search restriction in `cmake/BloomDependencyPrefix.cmake` (function-scoped):
package registries, environment search paths, CMake system paths, and the install-prefix root are
disabled, while `CMAKE_PREFIX_PATH` and `ZLIB_ROOT` point at the prefix. OpenColorIO's transitive
`find_dependency` calls cannot fall through to host `expat`, `Imath`, `pystring`, `yaml-cpp`,
`ZLIB`, or `minizip-ng`. The resolved `OpenColorIO` and dependency library locations are validated
under the prefix, and the standalone link line is expected to reference prefix libraries only.

## Regenerate the shader + manifest

```sh
build/gpu-shaders/bloom_gpu_neutral_display_generator \
    --asset assets/ocio/neutral-v1/config.ocio \
    --output-dir <dir>
```

Writes `<dir>/neutral_display.comp` and `<dir>/neutral_display.manifest`. Running it twice into
two directories produces byte-identical files (verified by the target below).

## Verify (offline, no GPU execution)

```sh
build/gpu-shaders/bloom_gpu_neutral_display_generator --asset assets/ocio/neutral-v1/config.ocio --output-dir /tmp/a
build/gpu-shaders/bloom_gpu_neutral_display_generator --asset assets/ocio/neutral-v1/config.ocio --output-dir /tmp/b
diff /tmp/a/neutral_display.comp /tmp/b/neutral_display.comp
diff /tmp/a/neutral_display.manifest /tmp/b/neutral_display.manifest

build/dependency-prefix/bin/glslangValidator --target-env vulkan1.2 -V \
    /tmp/a/neutral_display.comp -o /tmp/a/neutral_display.spv
build/dependency-prefix/bin/spirv-val --target-env vulkan1.2 /tmp/a/neutral_display.spv
```

The same sequence is available as a CMake target when the prefix tools are present:

```sh
cmake --build build/gpu-shaders --target bloom_gpu_neutral_display_verify
```

`spirv-val` validation is structural SPIR-V validation only. It is **not** a GPU execution claim.

## Shader interface contract

| Binding | Kind | Type | Role |
| ---: | --- | --- | --- |
| 0 | `std430` storage buffer, `readonly` | `vec4` | premultiplied `RGBA32F` input |
| 1 | `std430` storage buffer, `writeonly` | `uint` | packed output word |
| 2 | `std430` storage buffer, read/write | `uint` | error flag (`atomicOr`) |

- Push constant: `uint pixelCount`.
- `layout(local_size_x = 256) in;`, and every invocation with `gl_GlobalInvocationID.x >= pixelCount`
  returns before touching memory.
- Packed output word: `R | (G << 8) | (B << 16) | (A << 24)`. The consumer unpacks with shifts;
  no endianness assumption is baked in.
- Error bits (atomic `or`): `1` non-finite premultiplied input, `2` non-finite un-premultiply,
  `4` non-finite OCIO output, `8` any nonzero subnormal input lane. On any error the shader writes
  a `0` placeholder word; the consumer **must not publish a frame while the flag word is nonzero**.
  A whole frame containing a nonzero subnormal input lane is rejected (the intended route is a CPU
  fallback) until denormal preservation is qualified.

## Numeric contract

- **Alpha is exact to the CPU rule.**
  CPU: `(uint8_t)floor(clamp((double)alpha, 0, 1) * 255.0 + 0.5)`, where the `double` value is the
  exact binary64 expansion of the binary32 alpha. The shader reproduces this with `frexp` plus
  integer arithmetic (`bloom_neutral_v1_quantize_byte`) and **does not require `shaderFloat64`**.
  Because a binary32 significand has at most 24 bits and `255` is odd, `alpha * 255` is exactly
  representable in binary64; `frexp` + integer floor/half-up reproduces the binary64 result. The
  only exact tie is `alpha == 0.5` and it rounds up to `128`, matching the CPU. A host emulation of
  the integer path matched the CPU binary64 rule on all `1,065,353,217` representable binary32
  values in `[0, 1]` (0 mismatches), while a naive `float` `floor(x*255.0+0.5)` disagreed on 128
  inputs — so the naive form was rejected.
- **GPU execution must stay within one 8-bit code of the CPU.** OCIO GPU and CPU float rounding may differ, so RGB
  uses the same clamp/quantize rule with a documented one-code tolerance; alpha is exact.
- **Canonical zero alpha.** `alpha == 0.0` produces straight `RGB = +0.0`; otherwise the shader
  divides the premultiplied RGB by alpha with one binary32 division. Finite alpha outside `[0,1]`
  clamps exactly as the CPU rule does.
- **OCIO affects RGB only.** The transform is called on the straight RGB with an alpha lane of
  `1.0`; its alpha result is discarded and the original alpha is carried unchanged.
- **Non-finite values are never published.** Non-finite input, un-premultiply, or output sets an
  error bit and publishes a placeholder only.
- **Subnormal inputs reject the whole frame.** Any nonzero subnormal input lane sets error bit `8`
  before any arithmetic. Denormal preservation is not yet qualified, so the GPU path must not
  process such a frame; a CPU fallback is the intended route.

## Frozen input pin

`CMakeLists.txt` pins the raw asset bytes at configure time:

- raw `SHA-256(config.ocio bytes) = 6cb2d1460218958ffede3fb747f3a7d46d355ac0d3d5e6452eb9a5a47d63373f`
- `BloomOcioRevision` v1 envelope digest `= a6fc07c95cea14897b725992a179d35055cccce2ffaf719ca73b4a289c89eacf`
  (SHA-256 of `"BloomOcioRevision\0" || u16(1) || u8(1) || u64(632) || payload`)

These are distinct values; the configure check compares the raw `file(SHA256)` result against the
raw constant, and the generator rebuilds and checks the envelope at runtime. Any content change
requires a new built-in URI, never an edit of this payload. The host environment is neutralized
(`OCIO`, `OCIO_ACTIVE_DISPLAYS`, `OCIO_ACTIVE_VIEWS`, `OCIO_INACTIVE_COLORSPACES`,
`OCIO_USER_CATEGORIES`, `OCIO_OPTIMIZATION_FLAGS`, `OCIO_LOGGING_LEVEL` are cleared for the
generator process), so generator output does not depend on the ambient OCIO environment.

## Out of scope

Vulkan device creation, pipeline creation, descriptor-set layout code, LUT/SSBO upload, runtime
compilation, and application integration are not part of this tool. It only produces the offline
shader text, manifest, and SPIR-V validation evidence.
