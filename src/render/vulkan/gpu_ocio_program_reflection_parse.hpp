#ifndef BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_PARSE_HPP
#define BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_PARSE_HPP

// Private bounded SPIR-V parser and interface predicates for the OCIO reflection validator.
// Owned by gpu_ocio_program_reflection_validate.hpp; not a compiler or general SPIR-V library.

#include "gpu_ocio_program_reflection.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bloom::render::ocio_program_detail {
namespace ocio_reflection_detail {

inline constexpr std::uint32_t kSpirvMagic = 0x07230203u;
inline constexpr std::uint32_t kOpEntryPoint = 15u;
inline constexpr std::uint32_t kOpExecutionMode = 16u;
inline constexpr std::uint32_t kOpTypeInt = 21u;
inline constexpr std::uint32_t kOpTypeFloat = 22u;
inline constexpr std::uint32_t kOpTypeVector = 23u;
inline constexpr std::uint32_t kOpTypeImage = 25u;
inline constexpr std::uint32_t kOpTypeSampledImage = 27u;
inline constexpr std::uint32_t kOpTypeArray = 28u;
inline constexpr std::uint32_t kOpTypeRuntimeArray = 29u;
inline constexpr std::uint32_t kOpTypeStruct = 30u;
inline constexpr std::uint32_t kOpTypePointer = 32u;
inline constexpr std::uint32_t kOpConstant = 43u;
inline constexpr std::uint32_t kOpVariable = 59u;
inline constexpr std::uint32_t kOpDecorate = 71u;
inline constexpr std::uint32_t kOpMemberDecorate = 72u;

inline constexpr std::uint32_t kExecutionModelGLCompute = 5u;
inline constexpr std::uint32_t kExecutionModeLocalSize = 17u;
inline constexpr std::uint32_t kDecorationBlock = 2u;
inline constexpr std::uint32_t kDecorationBufferBlock = 3u;
inline constexpr std::uint32_t kDecorationArrayStride = 6u;
inline constexpr std::uint32_t kDecorationBinding = 33u;
inline constexpr std::uint32_t kDecorationDescriptorSet = 34u;
inline constexpr std::uint32_t kDecorationOffset = 35u;

inline constexpr std::uint32_t kStorageClassUniformConstant = 0u;
inline constexpr std::uint32_t kStorageClassUniform = 2u;
inline constexpr std::uint32_t kStorageClassPushConstant = 9u;
inline constexpr std::uint32_t kStorageClassStorageBuffer = 12u;

inline constexpr std::uint32_t kDim1D = 0u;
inline constexpr std::uint32_t kDim2D = 1u;
inline constexpr std::uint32_t kDim3D = 2u;
inline constexpr std::uint32_t kImageSampled = 1u;
inline constexpr std::uint32_t kImageStorage = 2u;
inline constexpr std::uint32_t kImageFormatRgba32f = 1u;

inline constexpr std::uint32_t kStd140ArrayStride = 16u;

struct ImageType final {
    std::uint32_t sampledType = 0;
    std::uint32_t dim = 0;
    std::uint32_t depth = 0;
    std::uint32_t arrayed = 0;
    std::uint32_t ms = 0;
    std::uint32_t sampled = 0;
    std::uint32_t format = 0;
};
struct SampledImageType final {
    std::uint32_t image = 0;
};
struct StructType final {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> members;
};
struct PointerType final {
    std::uint32_t storageClass = 0;
    std::uint32_t pointee = 0;
};
struct RuntimeArrayType final {
    std::uint32_t element = 0;
};
struct ArrayType final {
    std::uint32_t element = 0;
    std::uint32_t length = 0;
};
struct VectorType final {
    std::uint32_t component = 0;
    std::uint32_t count = 0;
};
struct IntType final {
    std::uint32_t width = 0;
    std::uint32_t signedness = 0;
};
struct FloatType final {
    std::uint32_t width = 0;
};
struct Variable final {
    std::uint32_t pointer = 0;
    std::uint32_t storageClass = 0;
};
struct Decoration final {
    bool hasSet = false;
    bool hasBinding = false;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
};

struct Module final {
    std::unordered_map<std::uint32_t, ImageType> images;
    std::unordered_map<std::uint32_t, SampledImageType> sampledImages;
    std::unordered_map<std::uint32_t, StructType> structs;
    std::unordered_map<std::uint32_t, PointerType> pointers;
    std::unordered_map<std::uint32_t, RuntimeArrayType> runtimeArrays;
    std::unordered_map<std::uint32_t, ArrayType> arrays;
    std::unordered_map<std::uint32_t, VectorType> vectors;
    std::unordered_map<std::uint32_t, IntType> ints;
    std::unordered_map<std::uint32_t, FloatType> floats;
    std::unordered_map<std::uint32_t, Variable> variables;
    std::unordered_map<std::uint32_t, Decoration> decorations;
    std::unordered_map<std::uint32_t, std::uint32_t> arrayStrides;
    std::unordered_map<std::uint32_t, std::uint32_t> constantValues;
    std::unordered_map<std::uint32_t, std::uint32_t> constantTypes;
    std::unordered_set<std::uint32_t> blockTypes;
    std::unordered_set<std::uint32_t> bufferBlockTypes;
    std::unordered_map<std::uint64_t, std::uint32_t> memberOffsets;
    std::uint32_t mainEntryPoints = 0;
    std::uint32_t mainEntryId = 0;
    bool mainLocalSize = false;
    std::uint32_t localSizeX = 0;
    std::uint32_t localSizeY = 0;
    std::uint32_t localSizeZ = 0;
};

[[nodiscard]] inline std::uint64_t memberKey(const std::uint32_t type,
                                             const std::uint32_t member) noexcept {
    return (static_cast<std::uint64_t>(type) << 32U) | member;
}

// Bounded to the OpEntryPoint instruction span so a missing terminator can never read past it.
[[nodiscard]] inline bool nameIsMain(const std::span<const std::uint32_t> words,
                                     const std::size_t nameWord,
                                     const std::size_t instructionEnd) noexcept {
    constexpr char kMain[] = "main";
    constexpr std::size_t kMainLength = sizeof(kMain) - 1;
    std::size_t character = 0;
    for (std::size_t word = nameWord; word < instructionEnd; ++word) {
        const std::uint32_t packed = words[word];
        for (int byte = 0; byte < 4; ++byte) {
            const char value = static_cast<char>((packed >> (8 * byte)) & 0xFFU);
            if (value == '\0') {
                return character == kMainLength;
            }
            if (character >= kMainLength || kMain[character] != value) {
                return false;
            }
            ++character;
        }
    }
    return false;
}

// Parses the bounded subset one instruction at a time; every recognized opcode must have a shape
// this parser fully understands or the module is rejected, so a truncated terminal instruction can
// never be read out of bounds or silently misinterpreted.
[[nodiscard]] inline bool parse(const std::span<const std::uint32_t> words, Module& module) {
    if (words.size() < 5 || words[0] != kSpirvMagic) {
        return false;
    }
    std::size_t index = 5;
    while (index < words.size()) {
        const std::uint32_t instruction = words[index];
        const std::size_t wordCount = instruction >> 16U;
        const std::uint32_t opcode = instruction & 0xFFFFU;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return false;
        }
        const std::size_t end = index + wordCount;
        const auto operand = [&](const std::size_t offset) { return words[index + offset]; };
        switch (opcode) {
        case kOpTypeInt:
            if (wordCount != 4) {
                return false;
            }
            module.ints[operand(1)] = IntType{operand(2), operand(3)};
            break;
        case kOpTypeFloat:
            if (wordCount != 3) {
                return false;
            }
            module.floats[operand(1)] = FloatType{operand(2)};
            break;
        case kOpTypeVector:
            if (wordCount != 4) {
                return false;
            }
            module.vectors[operand(1)] = VectorType{operand(2), operand(3)};
            break;
        case kOpTypeImage:
            if (wordCount != 9 && wordCount != 10) {
                return false;
            }
            module.images[operand(1)] =
                ImageType{operand(2), operand(3), operand(4), operand(5),
                          operand(6), operand(7), operand(8)};
            break;
        case kOpTypeSampledImage:
            if (wordCount != 3) {
                return false;
            }
            module.sampledImages[operand(1)] = SampledImageType{operand(2)};
            break;
        case kOpTypeArray:
            if (wordCount != 4) {
                return false;
            }
            module.arrays[operand(1)] = ArrayType{operand(2), operand(3)};
            break;
        case kOpTypeRuntimeArray:
            if (wordCount != 3) {
                return false;
            }
            module.runtimeArrays[operand(1)] = RuntimeArrayType{operand(2)};
            break;
        case kOpTypeStruct: {
            if (wordCount < 2) {
                return false;
            }
            StructType type;
            type.id = operand(1);
            for (std::size_t member = 2; member < wordCount; ++member) {
                type.members.push_back(operand(member));
            }
            module.structs[operand(1)] = std::move(type);
            break;
        }
        case kOpTypePointer:
            if (wordCount != 4) {
                return false;
            }
            module.pointers[operand(1)] = PointerType{operand(2), operand(3)};
            break;
        case kOpConstant:
            // A 64-bit scalar constant has a valid extra word; record the 32-bit value only and
            // never reject an otherwise-valid interface for an unrelated constant width.
            if (wordCount < 4) {
                return false;
            }
            module.constantTypes[operand(2)] = operand(1);
            module.constantValues[operand(2)] = operand(3);
            break;
        case kOpVariable:
            if (wordCount != 4 && wordCount != 5) {
                return false;
            }
            module.variables[operand(2)] = Variable{operand(1), operand(3)};
            break;
        case kOpDecorate: {
            if (wordCount < 3) {
                return false;
            }
            const std::uint32_t kind = operand(2);
            if (kind == kDecorationDescriptorSet || kind == kDecorationBinding ||
                kind == kDecorationArrayStride) {
                if (wordCount != 4) {
                    return false;
                }
                if (kind == kDecorationArrayStride) {
                    module.arrayStrides[operand(1)] = operand(3);
                } else {
                    Decoration& decoration = module.decorations[operand(1)];
                    if (kind == kDecorationDescriptorSet) {
                        decoration.hasSet = true;
                        decoration.set = operand(3);
                    } else {
                        decoration.hasBinding = true;
                        decoration.binding = operand(3);
                    }
                }
            } else if (kind == kDecorationBlock || kind == kDecorationBufferBlock) {
                if (wordCount != 3) {
                    return false;
                }
                if (kind == kDecorationBlock) {
                    module.blockTypes.insert(operand(1));
                } else {
                    module.bufferBlockTypes.insert(operand(1));
                }
            }
            break;
        }
        case kOpMemberDecorate:
            if (wordCount < 4) {
                return false;
            }
            if (operand(3) == kDecorationOffset) {
                if (wordCount != 5) {
                    return false;
                }
                module.memberOffsets[memberKey(operand(1), operand(2))] = operand(4);
            }
            break;
        case kOpEntryPoint:
            if (wordCount < 4) {
                return false;
            }
            if (operand(1) == kExecutionModelGLCompute && nameIsMain(words, index + 3, end)) {
                ++module.mainEntryPoints;
                module.mainEntryId = operand(2);
            }
            break;
        case kOpExecutionMode:
            if (wordCount < 3) {
                return false;
            }
            if (operand(1) == module.mainEntryId && operand(2) == kExecutionModeLocalSize) {
                if (wordCount != 6) {
                    return false;
                }
                module.mainLocalSize = true;
                module.localSizeX = operand(3);
                module.localSizeY = operand(4);
                module.localSizeZ = operand(5);
            }
            break;
        default:
            break;
        }
        index = end;
    }
    return true;
}

[[nodiscard]] inline const PointerType* pointerOf(const Module& module,
                                                  const Variable& variable) noexcept {
    const auto pointer = module.pointers.find(variable.pointer);
    return pointer == module.pointers.end() ? nullptr : &pointer->second;
}

[[nodiscard]] inline const ImageType* pointeeImage(const Module& module,
                                                   const Variable& variable) noexcept {
    const PointerType* const pointer = pointerOf(module, variable);
    if (pointer == nullptr) {
        return nullptr;
    }
    const auto image = module.images.find(pointer->pointee);
    return image == module.images.end() ? nullptr : &image->second;
}

[[nodiscard]] inline const SampledImageType* pointeeSampled(const Module& module,
                                                            const Variable& variable) noexcept {
    const PointerType* const pointer = pointerOf(module, variable);
    if (pointer == nullptr) {
        return nullptr;
    }
    const auto sampled = module.sampledImages.find(pointer->pointee);
    return sampled == module.sampledImages.end() ? nullptr : &sampled->second;
}

[[nodiscard]] inline const StructType* pointeeStruct(const Module& module,
                                                     const Variable& variable) noexcept {
    const PointerType* const pointer = pointerOf(module, variable);
    if (pointer == nullptr) {
        return nullptr;
    }
    const auto structure = module.structs.find(pointer->pointee);
    return structure == module.structs.end() ? nullptr : &structure->second;
}

[[nodiscard]] inline bool isFloat32(const Module& module, const std::uint32_t id) noexcept {
    const auto found = module.floats.find(id);
    return found != module.floats.end() && found->second.width == 32;
}

[[nodiscard]] inline bool isInt32(const Module& module, const std::uint32_t id,
                                  const std::uint32_t signedness) noexcept {
    const auto found = module.ints.find(id);
    return found != module.ints.end() && found->second.width == 32 &&
           found->second.signedness == signedness;
}

[[nodiscard]] inline bool isStorage(const std::uint32_t storageClass) noexcept {
    return storageClass == kStorageClassUniformConstant ||
           storageClass == kStorageClassUniform || storageClass == kStorageClassStorageBuffer;
}

// A runtime-array<uint> storage buffer with exact std140 I/O layout: one member at offset 0 whose
// element array has stride 4 and 32-bit unsigned elements.
[[nodiscard]] inline bool isRuntimeUintBuffer(const Module& module,
                                              const Variable& variable) noexcept {
    if (variable.storageClass != kStorageClassStorageBuffer &&
        variable.storageClass != kStorageClassUniform) {
        return false;
    }
    const StructType* const structure = pointeeStruct(module, variable);
    if (structure == nullptr || structure->members.size() != 1) {
        return false;
    }
    if (module.blockTypes.count(structure->id) == 0 &&
        module.bufferBlockTypes.count(structure->id) == 0) {
        return false;
    }
    const std::uint32_t arrayId = structure->members.front();
    const auto array = module.runtimeArrays.find(arrayId);
    if (array == module.runtimeArrays.end() || !isInt32(module, array->second.element, 0)) {
        return false;
    }
    const auto stride = module.arrayStrides.find(arrayId);
    if (stride == module.arrayStrides.end() || stride->second != 4) {
        return false;
    }
    const auto offset = module.memberOffsets.find(memberKey(structure->id, 0));
    return offset != module.memberOffsets.end() && offset->second == 0;
}

// A 2D rgba32f storage image (Bloom input/output on the effect arm).
[[nodiscard]] inline bool isRgba32fStorageImage2D(const Module& module,
                                                  const Variable& variable) noexcept {
    if (variable.storageClass != kStorageClassUniformConstant) {
        return false;
    }
    const ImageType* const image = pointeeImage(module, variable);
    return image != nullptr && image->dim == kDim2D && image->depth == 0 && image->arrayed == 0 &&
           image->ms == 0 && image->sampled == kImageStorage &&
           image->format == kImageFormatRgba32f && isFloat32(module, image->sampledType);
}

[[nodiscard]] inline bool dimMatches(const std::uint32_t dim,
                                     const OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return dim == kDim1D;
    case OcioGpuTextureDimensions::TwoD:
        return dim == kDim2D;
    case OcioGpuTextureDimensions::ThreeD:
        return dim == kDim3D;
    }
    return false;
}

[[nodiscard]] inline const Variable* descriptor(const Module& module, const std::uint32_t set,
                                                const std::uint32_t binding) noexcept {
    const Variable* match = nullptr;
    for (const auto& [id, variable] : module.variables) {
        const auto decoration = module.decorations.find(id);
        if (decoration == module.decorations.end() || !decoration->second.hasSet ||
            !decoration->second.hasBinding) {
            continue;
        }
        if (decoration->second.set == set && decoration->second.binding == binding) {
            if (match != nullptr) {
                return nullptr;
            }
            match = &variable;
        }
    }
    return match;
}

// Byte extent of one std140 array member of the exact expected element count, or false when the
// reflected array form, length, element type, or stride does not match.
[[nodiscard]] inline bool arrayMemberBytes(const Module& module, const std::uint32_t typeId,
                                           const bool floatElement, const std::uint32_t count,
                                           std::uint32_t& bytes) noexcept {
    if (count == 0) {
        return false;
    }
    if (count == 1 &&
        (floatElement ? isFloat32(module, typeId) : isInt32(module, typeId, 1))) {
        bytes = 4;
        return true;
    }
    const auto array = module.arrays.find(typeId);
    if (array == module.arrays.end()) {
        return false;
    }
    const auto length = module.constantValues.find(array->second.length);
    if (length == module.constantValues.end() || length->second != count) {
        return false;
    }
    const auto lengthType = module.constantTypes.find(array->second.length);
    if (lengthType == module.constantTypes.end() ||
        !isInt32(module, lengthType->second, 0)) {
        return false;
    }
    if (floatElement ? !isFloat32(module, array->second.element)
                     : !isInt32(module, array->second.element, 1)) {
        return false;
    }
    const auto stride = module.arrayStrides.find(typeId);
    if (stride == module.arrayStrides.end() || stride->second != kStd140ArrayStride) {
        return false;
    }
    // Bytes actually reachable by the last element of the std140 array.
    bytes = stride->second * (count - 1) + 4;
    return true;
}

// Exact member kind/size for a reflected OCIO uniform.
[[nodiscard]] inline bool uniformMemberBytes(const Module& module, const std::uint32_t typeId,
                                             const OcioGpuUniformDesc& uniform,
                                             std::uint32_t& bytes) noexcept {
    switch (uniform.type) {
    case OcioGpuUniformType::Double:
    case OcioGpuUniformType::Bool:
        if (uniform.elementCount != 1 || !isFloat32(module, typeId)) {
            return false;
        }
        bytes = 4;
        return true;
    case OcioGpuUniformType::Float3: {
        const auto vector = module.vectors.find(typeId);
        if (vector == module.vectors.end() || vector->second.count != 3 ||
            !isFloat32(module, vector->second.component)) {
            return false;
        }
        bytes = 12;
        return true;
    }
    case OcioGpuUniformType::VectorFloat:
        return arrayMemberBytes(module, typeId, true, uniform.elementCount, bytes);
    case OcioGpuUniformType::VectorInt:
        return arrayMemberBytes(module, typeId, false, uniform.elementCount, bytes);
    case OcioGpuUniformType::Unknown:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool expectedDescriptor(const std::uint32_t set, const std::uint32_t binding,
                                             const OcioGpuProgramDesc& program) noexcept {
    if (set == 0) {
        if (binding == 0 && program.uniformBufferSize > 0) {
            return true;
        }
        for (const auto& texture : program.textures) {
            if (binding == texture.binding) {
                return true;
            }
        }
        return false;
    }
    return set == 1 && binding <= 2;
}

} // namespace ocio_reflection_detail
} // namespace bloom::render::ocio_program_detail

#endif // BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_REFLECTION_PARSE_HPP
