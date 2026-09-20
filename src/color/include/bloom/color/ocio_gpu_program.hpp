#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

// Colour-owned OCIO GPU program extraction. This is the only new OCIO include site; it returns the
// OCIO/Vulkan/Qt-free render::OcioGpuProgramDesc. Extraction is read-only and bounded: it never
// parses arbitrary LUT bytes in-process and never rewrites OCIO shader text. The three entry points
// mirror the existing CPU resolver/qualified-transform policy exactly (same empty Context, same
// DisplayViewTransform / ColorSpaceTransform build) so the CPU processor remains the parity oracle.

namespace bloom::color {

inline constexpr std::string_view kOcioGpuDisplaySemanticsId = "bloom.color.ocio-display-gpu.v1";
inline constexpr std::string_view kOcioGpuCstSemanticsId = "bloom.color.ocio-process-cst.v1";
inline constexpr std::string_view kOcioGpuFileTransformSemanticsId =
    "bloom.color.ocio-process-file-transform.v1";
inline constexpr std::string_view kOcioGpuExposureContrastSemanticsId =
    "bloom.color.ocio-process-exposure-contrast.v1";
inline constexpr std::string_view kOcioGpuLut3dSemanticsId = "bloom.color.ocio-process-lut3d.v1";

// Versioned precise-sampling and CPU-math-parity GLSL adapter. OCIO's generated body samples its
// LUT resources with the built-in `texture()` function, whose hardware linear filtering quantizes
// the sub-texel weight on some devices (a real NVIDIA device reconstructs an 8-sample 1D LUT with
// up to ~8e-5 absolute error against OCIO's CPU interpolation). OCIO's reflected `sampler` metadata
// names each texture's sampler and its expected filtering (linear for 1D/2D LUTs; nearest for the
// 3D tetrahedral body, which samples exact texel centers and weights in the shader). The adapter
// generates one GLSL function per reflected texture that reproduces that texture's own semantics
// with `texelFetch` at full float precision, and a token-paste macro that rewrites the generated
// body's `texture(sampler, coord)` calls to the matching function.
//
// The adapter also reproduces the unchanged CPU oracle's exact arithmetic for the OCIO ops whose
// GPU and SSE-CPU paths diverge: a GammaOp's fast minimax power, a LogOp's `sseExp2(x*log2(base))`
// anti-log decomposition, and a MatrixOp's SSE add order. Whether the linked OCIO uses those SSE
// paths is qualified at runtime from its own default CPU processor, never guessed from the host
// architecture. Specialization is per OCIO op region, keyed on the pinned OCIO's deterministic op
// comments, so a program that mixes a GammaOp with an ExponentOp or a fixed-function op never has
// its unrelated `pow` calls rewritten.
//
// The generated OCIO transform body stays byte-for-byte intact in the extracted
// render::OcioGpuProgramDesc (and therefore its shaderTextDigest/contentIdentity are preserved);
// the specialization is a wrapper-level body override plus preprocessor redirection, mixed
// nearest/linear programs are handled per sampler, and the same API is what the production runtime
// wrapper must consume.
inline constexpr std::string_view kOcioGpuPreciseSamplingVersion =
    "bloom.color.ocio-gpu-sampling.v5";

// Portable correctly-rounded binary32 division GLSL helper (no float64). Some Vulkan devices do not
// round `a / b` to nearest-even, which the wrapper's un-premultiply needs to match the unchanged
// CPU oracle's scalar `pixel / alpha` exactly. The helper recomputes the exact residual with FMA
// and folds it back; both the production runtime wrapper and the native oracle harness emit this
// same definition and call `bloom_ocio_cpu_div`.
[[nodiscard]] std::string_view ocioGpuPreciseDivisionGlsl() noexcept;
struct OcioGpuSamplingGlsl final {
    // Emitted before the generated OCIO body: one forward declaration per texture plus the
    // `texture` dispatch macro, the CPU-parity helper declarations, and (for a GammaOp-only
    // program) the `pow` parity declarations and dispatch macro.
    std::string preamble;
    // Emitted after the generated OCIO body: the per-texture definitions and the CPU-parity helper
    // definitions. Empty alongside preamble.
    std::string definitions;
    // When non-empty, the caller MUST emit this body instead of the descriptor's `shaderText`. It
    // is byte-identical to the generated body outside the identified OCIO op regions that were
    // specialized. The descriptor itself (and its digest/content identity) is never modified.
    std::string shaderBody;

    [[nodiscard]] bool dispatches() const noexcept { return !preamble.empty(); }
};
[[nodiscard]] OcioGpuSamplingGlsl ocioGpuSamplingGlslFor(const render::OcioGpuProgramDesc& program);

// Display/view extraction for a resolved config, using the same display/view pair the CPU path
// selects. Any enumerated non-data pair is accepted; the caller must still gate on the native
// lane's capability report.
[[nodiscard]] render::OcioGpuProgramResult
buildOcioGpuProgramForDisplay(const ResolvedBloomNeutralConfig& resolved, std::string_view display,
                              std::string_view view,
                              const render::OcioGpuProgramLimits& limits = {}) noexcept;

// Colour space transform extraction. Both ids must resolve to non-data spaces in the exact config;
// an equal source/destination is reported as IdentityTransform (the caller keeps its CPU identity
// path rather than compiling a fabricated passthrough).
[[nodiscard]] render::OcioGpuProgramResult
buildOcioGpuProgramForCst(const ResolvedBloomNeutralConfig& resolved, std::string_view fromId,
                          std::string_view toId,
                          const render::OcioGpuProgramLimits& limits = {}) noexcept;

// File transform extraction. Arbitrary `.cube`/`.clf`/`.spi1d`/`.spi3d` bytes are never parsed
// in-process: the exact bytes and their digest are handed to the isolated bloom-color-worker, which
// rebuilds the OCIO FileTransform processor with the requested interpolation/direction, reflects
// the shader/resources/uniforms, and returns them over the bounded typed IPC. The host validates
// the transported payload against the same limits before accepting it. `workingSpaceId` and
// `processSpaceId` must resolve to non-data colour spaces in the exact resolved config; they and
// the digest/format/interpolation/direction are part of the program's content identity. A target
// without the confinement/process-supervision primitive returns ExternalLutBoundaryRequired.
[[nodiscard]] render::OcioGpuProgramResult buildOcioGpuProgramForFileTransform(
    const ResolvedBloomNeutralConfig& resolved, const LutFile& lutFile,
    LutInterpolation interpolation, LutDirection direction, std::string_view processSpaceId,
    std::string_view workingSpaceId, const render::OcioGpuProgramLimits& limits = {},
    const std::function<bool()>& cancellation = {}) noexcept;

// In-memory OCIO transform extraction. These build the OCIO transform from already-trusted
// parameters (never from parsed file bytes) against the exact resolved config, so the native lane
// can exercise real OCIO uniform declarations/values and a real 3D LUT without the isolated file
// boundary. `buildOcioGpuProgramForExposureContrast` emits OCIO's own dynamic exposure/contrast
// uniforms; `buildOcioGpuProgramForLut3d` emits a real 3D LUT texture. Samples are RGB per grid
// point in OCIO's (r,g,b) order and must be finite.
[[nodiscard]] render::OcioGpuProgramResult
buildOcioGpuProgramForExposureContrast(const ResolvedBloomNeutralConfig& resolved,
                                       std::string_view sourceId, double exposure, double contrast,
                                       const render::OcioGpuProgramLimits& limits = {}) noexcept;

[[nodiscard]] render::OcioGpuProgramResult
buildOcioGpuProgramForLut3d(const ResolvedBloomNeutralConfig& resolved, std::string_view sourceId,
                            std::uint32_t edge, render::OcioGpuInterpolation interpolation,
                            std::span<const float> samples,
                            const render::OcioGpuProgramLimits& limits = {}) noexcept;

} // namespace bloom::color
