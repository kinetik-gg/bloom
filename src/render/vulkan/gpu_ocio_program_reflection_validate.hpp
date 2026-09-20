#ifndef BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_VALIDATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_VALIDATE_HPP

// Definition of the OCIO shader interface validator. Include this header to call it.

#include "gpu_ocio_program_reflection_parse.hpp"

namespace bloom::render::ocio_program_detail {
// Validates the compiled module's descriptor interface against the immutable program descriptor and
// the Bloom I/O contract. `expectedWorkgroupSizeX` is the workgroup size the dispatcher plans with.
// Never throws; any allocation failure or malformed input is a typed refusal.
[[nodiscard]] inline OcioShaderInterfaceCheck
validateOcioShaderInterface(const OcioGpuProgramDesc& program,
                            const std::span<const std::uint32_t> spirv,
                            const std::uint32_t expectedWorkgroupSizeX) noexcept {
    using namespace ocio_reflection_detail;
    try {
        Module module;
        if (!parse(spirv, module)) {
            return {OcioShaderInterfaceError::MalformedModule, 0};
        }
        // A pointer's storage class must agree with the variable that names it.
        for (const auto& [id, variable] : module.variables) {
            const PointerType* const pointer = pointerOf(module, variable);
            if (pointer == nullptr || pointer->storageClass != variable.storageClass) {
                return {OcioShaderInterfaceError::MalformedModule, id};
            }
        }
        if (module.mainEntryPoints != 1) {
            return {OcioShaderInterfaceError::MissingEntryPoint, module.mainEntryPoints};
        }
        if (!module.mainLocalSize || module.localSizeX != expectedWorkgroupSizeX ||
            module.localSizeY != 1 || module.localSizeZ != 1) {
            return {OcioShaderInterfaceError::WorkgroupSizeMismatch, module.localSizeX};
        }
        const std::uint32_t ocioSet = program.descriptorSetIndex;
        if (ocioSet != 0) {
            return {OcioShaderInterfaceError::UnsupportedInterface, ocioSet};
        }

        // Every variable in a descriptor storage class must carry a set+binding inside the declared
        // interface. This rejects set >= 2 and partially decorated variables, not only set 0/1.
        for (const auto& [id, variable] : module.variables) {
            if (!isStorage(variable.storageClass)) {
                continue;
            }
            const auto decoration = module.decorations.find(id);
            if (decoration == module.decorations.end() || !decoration->second.hasSet ||
                !decoration->second.hasBinding) {
                return {OcioShaderInterfaceError::UnexpectedDescriptor, id};
            }
            if (!expectedDescriptor(decoration->second.set, decoration->second.binding, program)) {
                return {OcioShaderInterfaceError::UnexpectedDescriptor, decoration->second.binding};
            }
        }

        // Set 0: one combined-image-sampler per declared texture.
        for (const auto& texture : program.textures) {
            const Variable* const variable = descriptor(module, ocioSet, texture.binding);
            if (variable == nullptr || variable->storageClass != kStorageClassUniformConstant) {
                return {OcioShaderInterfaceError::DescriptorMismatch, texture.binding};
            }
            const SampledImageType* const sampled = pointeeSampled(module, *variable);
            if (sampled == nullptr) {
                return {OcioShaderInterfaceError::DescriptorMismatch, texture.binding};
            }
            const auto image = module.images.find(sampled->image);
            if (image == module.images.end() || image->second.sampled != kImageSampled ||
                image->second.arrayed != 0 || image->second.ms != 0 || image->second.depth != 0 ||
                !dimMatches(image->second.dim, texture.dimensions) ||
                !isFloat32(module, image->second.sampledType)) {
                return {OcioShaderInterfaceError::DescriptorMismatch, texture.binding};
            }
        }

        // Set 0 binding 0: exact reflected UBO block layout and byte extent.
        if (program.uniformBufferSize > 0) {
            const Variable* const uniform = descriptor(module, ocioSet, 0);
            if (uniform == nullptr || uniform->storageClass != kStorageClassUniform) {
                return {OcioShaderInterfaceError::DescriptorMismatch, 0};
            }
            const StructType* const structure = pointeeStruct(module, *uniform);
            if (structure == nullptr || module.blockTypes.count(structure->id) == 0 ||
                structure->members.size() != program.uniforms.size()) {
                return {OcioShaderInterfaceError::DescriptorMismatch, 0};
            }
            for (std::uint32_t member = 0; member < structure->members.size(); ++member) {
                const OcioGpuUniformDesc& field = program.uniforms[member];
                const auto offset = module.memberOffsets.find(memberKey(structure->id, member));
                if (offset == module.memberOffsets.end() || offset->second != field.bufferOffset) {
                    return {OcioShaderInterfaceError::DescriptorMismatch, field.bufferOffset};
                }
                std::uint32_t memberBytes = 0;
                if (!uniformMemberBytes(module, structure->members[member], field, memberBytes) ||
                    static_cast<std::uint64_t>(offset->second) + memberBytes >
                        program.uniformBufferSize) {
                    return {OcioShaderInterfaceError::DescriptorMismatch, field.bufferOffset};
                }
            }
        }

        // Set 1: Bloom input/output/status with exact I/O layout.
        const Variable* const input = descriptor(module, 1, 0);
        if (input == nullptr || !isRgba32fStorageImage2D(module, *input)) {
            return {OcioShaderInterfaceError::DescriptorMismatch, 0};
        }
        const Variable* const output = descriptor(module, 1, 1);
        if (output == nullptr) {
            return {OcioShaderInterfaceError::DescriptorMismatch, 1};
        }
        if (program.stage == OcioGpuProgramStage::DisplayPacking) {
            if (!isRuntimeUintBuffer(module, *output)) {
                return {OcioShaderInterfaceError::DescriptorMismatch, 1};
            }
        } else if (!isRgba32fStorageImage2D(module, *output)) {
            return {OcioShaderInterfaceError::DescriptorMismatch, 1};
        }
        const Variable* const status = descriptor(module, 1, 2);
        if (status == nullptr || !isRuntimeUintBuffer(module, *status)) {
            return {OcioShaderInterfaceError::DescriptorMismatch, 2};
        }

        // push_constant: exactly three uint32 members at offsets 0/4/8.
        const Variable* pushConstant = nullptr;
        std::size_t pushConstantCount = 0;
        for (const auto& [id, variable] : module.variables) {
            if (variable.storageClass == kStorageClassPushConstant) {
                pushConstant = &variable;
                ++pushConstantCount;
            }
        }
        if (pushConstantCount != 1 || pushConstant == nullptr) {
            return {OcioShaderInterfaceError::PushConstantMismatch, 0};
        }
        const StructType* const pushStructure = pointeeStruct(module, *pushConstant);
        if (pushStructure == nullptr || module.blockTypes.count(pushStructure->id) == 0 ||
            pushStructure->members.size() != 3) {
            return {OcioShaderInterfaceError::PushConstantMismatch, 0};
        }
        for (std::uint32_t member = 0; member < pushStructure->members.size(); ++member) {
            if (!isInt32(module, pushStructure->members[member], 0)) {
                return {OcioShaderInterfaceError::PushConstantMismatch,
                        pushStructure->members[member]};
            }
            const auto offset = module.memberOffsets.find(memberKey(pushStructure->id, member));
            if (offset == module.memberOffsets.end() ||
                offset->second != static_cast<std::uint32_t>(member * 4U)) {
                return {OcioShaderInterfaceError::PushConstantMismatch, member};
            }
        }
        return {OcioShaderInterfaceError::None, 0};
    } catch (...) {
        return {OcioShaderInterfaceError::MalformedModule, 0};
    }
}
} // namespace bloom::render::ocio_program_detail

#endif // BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_VALIDATE_HPP
