#pragma once

// Reusable Bloom wrapper generation for an extracted OCIO GPU program
// (render::OcioGpuProgramDesc). This is the production home of the wrapper structure that
// src/color/tests/ocio_gpu_wrapper_fixture.comp and the OCIO native tests proved: it is a pure,
// Qt-free and Vulkan-free string builder. It never parses OCIO config, never touches a device, and
// never string-rewrites the extracted OCIO shader text (that text is appended verbatim).
//
// The wrapper binds the exact descriptor layout bloom::render::GpuOcioProgram creates:
//  * set 1 binding 0: rgba32f readonly storage image input (premultiplied process pixels);
//  * set 1 binding 1: rgba32f writeonly storage image (ProcessEffect) OR std430 uint words
//    (DisplayPacking);
//  * set 1 binding 2: std430 uint flags status word (nonzero = the shader rejected the frame);
//  * push_constant: { uint pixelCount; uint width; uint height; }.
// OCIO's own resources occupy set 0 with the bindings the extraction declared; this generator never
// moves them. LUT sampling uses the shared versioned color::ocioGpuSamplingGlslFor adapter: its
// preamble is emitted before the extracted body and its definitions after, with the body left
// verbatim. The adapter version is recorded on the result and folded into the command identity.

#include <bloom/core/sha256.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace bloom::runtime {

// The exact entry point and workgroup size bloom::render::GpuOcioProgram dispatches.
inline constexpr std::string_view kGpuOcioWrapperEntryPoint = "main";
inline constexpr std::uint32_t kGpuOcioWrapperWorkgroupSize = 64;

enum class GpuOcioWrapperError : std::uint8_t {
    None,
    InvalidProgram,
    UnsupportedStage,
    InvalidFunctionName,
    UnsupportedDescriptorSet,
    // The wrapper generation itself could not allocate. The public function is noexcept, so an
    // allocation failure is reported as this typed error rather than terminating the process.
    AllocationFailure,
    // The supplied ViewAdjust is non-finite or outside its accepted domain.
    InvalidViewAdjust,
    // A non-neutral ViewAdjust was supplied for the ProcessEffect arm, which has no display stage.
    UnsupportedViewAdjust,
};

[[nodiscard]] std::string_view gpuOcioWrapperErrorName(GpuOcioWrapperError error) noexcept;

struct GpuOcioWrapperResult final {
    std::string source;
    std::string entryPoint;
    // The exact versioned per-sampler precise-sampling adapter used to build this wrapper
    // (color::kOcioGpuPreciseSamplingVersion). Folded into the prepared command identity so a
    // sampling-semantics change can never reuse an older artifact.
    std::string samplingVersion;
    core::Sha256Digest sourceDigest{};
    GpuOcioWrapperError error = GpuOcioWrapperError::None;

    [[nodiscard]] bool succeeded() const noexcept {
        return error == GpuOcioWrapperError::None && !source.empty();
    }
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Builds the complete Vulkan GLSL compute program (Bloom wrapper + the exact extracted OCIO shader
// text) for `program.stage`. Semantics, frozen against the accepted test wrapper:
//  * read finite premultiplied RGBA32F, un-premultiply straight RGB with one divide (on the display
//    arm alpha == 0 => straight +0 RGB; on the process-effect arm the whole pixel is preserved),
//    apply the OCIO function to straight RGB with the alpha lane 1.0, keep the original alpha;
//  * ProcessEffect publishes premultiplied RGBA32F and never clamps (HDR/negative survive), and
//    copies an alpha-zero source pixel through unchanged (exact CPU image-effect semantics);
//  * DisplayPacking clamps straight RGB to [0, 1], quantizes straight RGBA8 with
//    floor(clamp(v, 0, 1) * 255 + 0.5), and packs r | g << 8 | b << 16 | a << 24;
//  * DisplayPacking additionally applies the exact runtime::ViewAdjust post-display exposure/gamma
//    (`ViewAdjust::fromEncoded`) before quantization when the adjustment is non-neutral; a neutral
//    adjustment emits no adjust code, so neutral wrappers are unchanged. A non-neutral adjustment
//    on the ProcessEffect arm is refused (UnsupportedViewAdjust) and an invalid adjustment is
//    refused (InvalidViewAdjust);
//  * non-finite straight input or non-finite OCIO output raises the status word and writes no pixel
//    so the native consumer fails the frame rather than publishing a non-finite value.
[[nodiscard]] GpuOcioWrapperResult
buildGpuOcioWrapperGlsl(const render::OcioGpuProgramDesc& program,
                        ViewAdjust viewAdjust = {}) noexcept;

} // namespace bloom::runtime
