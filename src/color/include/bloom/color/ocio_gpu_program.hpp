#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <cstdint>
#include <span>
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

// File transform extraction boundary. Arbitrary `.cube`/`.clf`/`.spi1d`/`.spi3d` bytes are never
// parsed in-process; the isolated bloom-color-worker intake must produce the OCIO processor first.
// Until that arm exists this returns ExternalLutBoundaryRequired, and the LUT digest/format/
// interpolation/direction are part of the eventual content identity.
[[nodiscard]] render::OcioGpuProgramResult buildOcioGpuProgramForFileTransform(
    const ResolvedBloomNeutralConfig& resolved, std::string_view processSpaceId,
    const core::Sha256Digest& sourceLutDigest, std::uint32_t lutFormat,
    LutInterpolation interpolation, LutDirection direction, std::string_view workingSpaceId,
    const render::OcioGpuProgramLimits& limits = {}) noexcept;

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
