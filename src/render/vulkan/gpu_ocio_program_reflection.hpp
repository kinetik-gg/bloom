#ifndef BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_HPP
#define BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_HPP

// Bounded SPIR-V interface reflection for the runtime-compiled OCIO GPU program. It is the render
// side of the accepted "SPIR-V structural validation and reflection exactly match the declared
// entry point, bindings, descriptor types, dimensions, and limits" gate in
// docs/architecture/gpu-backend.md. It is deliberately NOT a compiler or a general SPIR-V library:
// it walks the instruction stream once, rejects any recognized instruction whose shape it cannot
// fully trust, and validates only the exact descriptor interface this program binds, before any
// native allocation or submission:
//
//   * set `descriptorSetIndex` (0): the OCIO uniform buffer at binding 0 when declared, and one
//     combined-image-sampler per `OcioGpuTextureDesc` at its binding, with matching image
//     dimensionality, Sampled == 1, non-arrayed, non-MS, non-depth, float 32-bit;
//   * set 1: Bloom input storage image (binding 0, 2D, storage-sampled, rgba32f), Bloom output
//     (binding 1: storage image for ProcessEffect, runtime-array<uint> storage buffer for
//     DisplayPacking), and the status storage buffer (binding 2), each with exact member offset 0
//     and ArrayStride 4;
//   * the OCIO uniform buffer block: member count, per-member offset, member kind (scalar float,
//     vec3 float, or a std140 array of the exact reflected element count) and byte extent against
//     `uniformBufferSize`;
//   * no descriptor outside the declared interface, including any set >= 2 and any variable in a
//     descriptor storage class that is only partially decorated;
//   * a single GLCompute "main" entry point with LocalSize x == expected, y == z == 1;
//   * pointer storage class agrees with each variable's storage class.
//
// Every mismatch is a typed refusal. The header has no Vulkan/OCIO/Qt dependency so the native
// render translation unit and a pure unit test can share it unchanged.

#include <bloom/render/ocio_gpu_program.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bloom::render::ocio_program_detail {

enum class OcioShaderInterfaceError : std::uint8_t {
    None,
    MalformedModule,
    MissingEntryPoint,
    WorkgroupSizeMismatch,
    DescriptorMismatch,
    UnexpectedDescriptor,
    PushConstantMismatch,
    UnsupportedInterface,
};

struct OcioShaderInterfaceCheck final {
    OcioShaderInterfaceError error = OcioShaderInterfaceError::MalformedModule;
    std::uint32_t detail = 0;

    [[nodiscard]] bool valid() const noexcept { return error == OcioShaderInterfaceError::None; }
};

[[nodiscard]] inline const char*
ocioShaderInterfaceErrorName(const OcioShaderInterfaceError error) noexcept {
    switch (error) {
    case OcioShaderInterfaceError::None:
        return "none";
    case OcioShaderInterfaceError::MalformedModule:
        return "malformed-module";
    case OcioShaderInterfaceError::MissingEntryPoint:
        return "missing-entry-point";
    case OcioShaderInterfaceError::WorkgroupSizeMismatch:
        return "workgroup-size-mismatch";
    case OcioShaderInterfaceError::DescriptorMismatch:
        return "descriptor-mismatch";
    case OcioShaderInterfaceError::UnexpectedDescriptor:
        return "unexpected-descriptor";
    case OcioShaderInterfaceError::PushConstantMismatch:
        return "push-constant-mismatch";
    case OcioShaderInterfaceError::UnsupportedInterface:
        return "unsupported-interface";
    }
    return "unknown";
}

// Validates the compiled module's descriptor interface against the immutable program descriptor and
// the Bloom I/O contract. `expectedWorkgroupSizeX` is the workgroup size the dispatcher plans with.
// Never throws; any allocation failure or malformed input is a typed refusal. Defined in
// gpu_ocio_program_reflection_validate.hpp.
[[nodiscard]] OcioShaderInterfaceCheck validateOcioShaderInterface(
    const OcioGpuProgramDesc& program, std::span<const std::uint32_t> spirv,
    std::uint32_t expectedWorkgroupSizeX) noexcept;

} // namespace bloom::render::ocio_program_detail

#endif // BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_HPP
