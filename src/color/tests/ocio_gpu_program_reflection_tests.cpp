#include "ocio_gpu_program_resources_test_support.hpp"
#include "vulkan/gpu_ocio_program_reflection_validate.hpp"

#include <initializer_list>

namespace bloom::color::ocio_resources_test {

// --- Deterministic malformed/mutation reflection tests (no device, no OCIO, no compiler) ---------

namespace spirv_test {
constexpr std::uint32_t kOpEntryPoint = 15u;
constexpr std::uint32_t kOpExecutionMode = 16u;
constexpr std::uint32_t kOpTypeInt = 21u;
constexpr std::uint32_t kOpTypeFloat = 22u;
constexpr std::uint32_t kOpTypeImage = 25u;
constexpr std::uint32_t kOpTypeRuntimeArray = 29u;
constexpr std::uint32_t kOpTypeStruct = 30u;
constexpr std::uint32_t kOpTypePointer = 32u;
constexpr std::uint32_t kOpVariable = 59u;
constexpr std::uint32_t kOpDecorate = 71u;
constexpr std::uint32_t kOpMemberDecorate = 72u;

void emit(std::vector<std::uint32_t>& words, const std::uint32_t opcode,
          const std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(1 + operands.size()) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

// Minimal, structurally self-consistent display-stage module matching the Bloom I/O + a one-float
// set-0 UBO. `entryNameTerminated == false` produces an OpEntryPoint whose name never terminates
// inside the instruction.
[[nodiscard]] std::vector<std::uint32_t> buildDisplayModule(const bool entryNameTerminated) {
    std::vector<std::uint32_t> module{0x07230203u, 0x00010000u, 0u, 100u, 0u};
    // EntryPoint GLCompute %1 "main"; name is "main\0" or an unterminated "mainmain".
    emit(module, kOpEntryPoint, {5u, 1u, 0x6E69616Du, entryNameTerminated ? 0u : 0x6E69616Du});
    emit(module, kOpExecutionMode, {1u, 17u, 64u, 1u, 1u});
    emit(module, kOpTypeInt, {10u, 32u, 0u});                       // %10 uint
    emit(module, kOpTypeFloat, {11u, 32u});                         // %11 float
    emit(module, kOpTypeImage, {12u, 11u, 1u, 0u, 0u, 0u, 2u, 1u}); // input 2D storage rgba32f
    emit(module, kOpTypePointer, {13u, 0u, 12u});
    emit(module, kOpVariable, {13u, 20u, 0u});
    emit(module, kOpTypeRuntimeArray, {14u, 10u});
    emit(module, kOpTypeStruct, {15u, 14u});
    emit(module, kOpTypePointer, {16u, 12u, 15u});
    emit(module, kOpVariable, {16u, 21u, 12u});
    emit(module, kOpTypeRuntimeArray, {17u, 10u});
    emit(module, kOpTypeStruct, {18u, 17u});
    emit(module, kOpTypePointer, {19u, 12u, 18u});
    emit(module, kOpVariable, {19u, 22u, 12u});
    emit(module, kOpTypeStruct, {23u, 10u, 10u, 10u});
    emit(module, kOpTypePointer, {24u, 9u, 23u});
    emit(module, kOpVariable, {24u, 25u, 9u});
    emit(module, kOpTypeStruct, {26u, 11u}); // UBO { float }
    emit(module, kOpTypePointer, {27u, 2u, 26u});
    emit(module, kOpVariable, {27u, 28u, 2u});
    for (const auto decoration : {33u, 34u}) {
        emit(module, kOpDecorate, {20u, decoration, decoration == 34u ? 1u : 0u});
        emit(module, kOpDecorate, {21u, decoration, decoration == 34u ? 1u : 1u});
        emit(module, kOpDecorate, {22u, decoration, decoration == 34u ? 1u : 2u});
        emit(module, kOpDecorate, {28u, decoration, decoration == 34u ? 0u : 0u});
    }
    emit(module, kOpDecorate, {14u, 6u, 4u});
    emit(module, kOpDecorate, {17u, 6u, 4u});
    emit(module, kOpDecorate, {15u, 2u});
    emit(module, kOpDecorate, {18u, 2u});
    emit(module, kOpDecorate, {23u, 2u});
    emit(module, kOpDecorate, {26u, 2u});
    emit(module, kOpMemberDecorate, {15u, 0u, 35u, 0u});
    emit(module, kOpMemberDecorate, {18u, 0u, 35u, 0u});
    emit(module, kOpMemberDecorate, {26u, 0u, 35u, 0u});
    emit(module, kOpMemberDecorate, {23u, 0u, 35u, 0u});
    emit(module, kOpMemberDecorate, {23u, 1u, 35u, 4u});
    emit(module, kOpMemberDecorate, {23u, 2u, 35u, 8u});
    return module;
}

[[nodiscard]] bloom::render::OcioGpuProgramDesc displayDescriptor(
    const std::uint32_t uniformBufferSize = 8u, const std::uint32_t uniformOffset = 0u,
    const bloom::render::OcioGpuUniformType uniformType = bloom::render::OcioGpuUniformType::Double,
    const std::uint32_t elementCount = 1u) {
    bloom::render::OcioGpuProgramDesc desc;
    desc.descriptorSetIndex = 0;
    desc.stage = bloom::render::OcioGpuProgramStage::DisplayPacking;
    desc.uniformBufferSize = uniformBufferSize;
    desc.uniforms.push_back(
        bloom::render::OcioGpuUniformDesc{"param", uniformType, uniformOffset, elementCount});
    return desc;
}

void changeFirstArrayStride(std::vector<std::uint32_t>& words, const std::uint32_t stride) {
    for (std::size_t index = 5; index < words.size();) {
        const std::size_t wordCount = words[index] >> 16U;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return;
        }
        if ((words[index] & 0xFFFFU) == kOpDecorate && wordCount == 4 && words[index + 2] == 6U) {
            words[index + 3] = stride;
            return;
        }
        index += wordCount;
    }
}

void changeFirstOffset(std::vector<std::uint32_t>& words, const std::uint32_t offset) {
    for (std::size_t index = 5; index < words.size();) {
        const std::size_t wordCount = words[index] >> 16U;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return;
        }
        if ((words[index] & 0xFFFFU) == kOpMemberDecorate && wordCount == 5 &&
            words[index + 3] == 35U) {
            words[index + 4] = offset;
            return;
        }
        index += wordCount;
    }
}
} // namespace spirv_test

void testShaderInterfaceMalformed(Expectations& expectations) {
    using bloom::render::ocio_program_detail::validateOcioShaderInterface;
    using Error = bloom::render::ocio_program_detail::OcioShaderInterfaceError;
    constexpr std::uint32_t kWorkgroup = 64;

    const auto base = spirv_test::buildDisplayModule(true);
    const auto baseDesc = spirv_test::displayDescriptor();
    expectations.expect(validateOcioShaderInterface(baseDesc, base, kWorkgroup).valid(),
                        "the synthetic display module reflects the declared interface");

    // Terminal one-word OpTypeStruct appended after a valid module.
    {
        auto words = base;
        words.push_back((1U << 16U) | spirv_test::kOpTypeStruct);
        const auto check = validateOcioShaderInterface(baseDesc, words, kWorkgroup);
        expectations.expect(check.error == Error::MalformedModule,
                            "a terminal short OpTypeStruct is rejected as malformed");
    }
    // Truncated (unterminated) entry-point name.
    {
        const auto words = spirv_test::buildDisplayModule(false);
        const auto check = validateOcioShaderInterface(baseDesc, words, kWorkgroup);
        expectations.expect(check.error == Error::MissingEntryPoint,
                            "an unterminated entry-point name is rejected");
    }
    // Extra descriptor at set 2 (and an unexpected binding) is rejected.
    {
        auto words = base;
        spirv_test::emit(words, spirv_test::kOpVariable, {13u, 30u, 0u});
        spirv_test::emit(words, spirv_test::kOpDecorate, {30u, 33u, 0u});
        spirv_test::emit(words, spirv_test::kOpDecorate, {30u, 34u, 2u});
        const auto check = validateOcioShaderInterface(baseDesc, words, kWorkgroup);
        expectations.expect(check.error == Error::UnexpectedDescriptor,
                            "a set >= 2 descriptor is rejected");
    }
    // Partially decorated descriptor-storage variable (set without binding).
    {
        auto words = base;
        spirv_test::emit(words, spirv_test::kOpVariable, {13u, 31u, 0u});
        spirv_test::emit(words, spirv_test::kOpDecorate, {31u, 34u, 1u});
        const auto check = validateOcioShaderInterface(baseDesc, words, kWorkgroup);
        expectations.expect(check.error == Error::UnexpectedDescriptor,
                            "a partially decorated descriptor is rejected");
    }
    // Wrong runtime-buffer ArrayStride and wrong member offset.
    {
        auto words = base;
        spirv_test::changeFirstArrayStride(words, 8u);
        expectations.expect(validateOcioShaderInterface(baseDesc, words, kWorkgroup).error ==
                                Error::DescriptorMismatch,
                            "an ArrayStride other than 4 is rejected");
    }
    {
        auto words = base;
        spirv_test::changeFirstOffset(words, 4u);
        expectations.expect(validateOcioShaderInterface(baseDesc, words, kWorkgroup).error ==
                                Error::DescriptorMismatch,
                            "a runtime-buffer member offset other than 0 is rejected");
    }
    // UBO wrong member type, wrong offset, and extent beyond the bound bytes.
    expectations.expect(
        validateOcioShaderInterface(
            spirv_test::displayDescriptor(8u, 0u, bloom::render::OcioGpuUniformType::Float3, 3u),
            base, kWorkgroup)
                .error == Error::DescriptorMismatch,
        "a UBO member whose kind does not match the descriptor is rejected");
    expectations.expect(
        validateOcioShaderInterface(spirv_test::displayDescriptor(8u, 4u), base, kWorkgroup)
                .error == Error::DescriptorMismatch,
        "a UBO member whose offset does not match the descriptor is rejected");
    expectations.expect(
        validateOcioShaderInterface(spirv_test::displayDescriptor(2u, 0u), base, kWorkgroup)
                .error == Error::DescriptorMismatch,
        "a UBO member extent beyond uniformBufferSize is rejected");
    // Pointer storage class disagreeing with the variable.
    {
        auto words = base;
        for (std::size_t index = 5; index < words.size();) {
            const std::size_t wordCount = words[index] >> 16U;
            if (wordCount == 0 || index + wordCount > words.size()) {
                break;
            }
            if ((words[index] & 0xFFFFU) == spirv_test::kOpVariable && index + 4 <= words.size() &&
                words[index + 2] == 20u) {
                words[index + 3] = 3u; // Output, while its pointer type stays UniformConstant
                break;
            }
            index += wordCount;
        }
        const auto check = validateOcioShaderInterface(baseDesc, words, kWorkgroup);
        expectations.expect(check.error == Error::MalformedModule,
                            "a pointer/variable storage-class disagreement is malformed");
    }
}

// --- SPIR-V interface reflection (Finding 2) -----------------------------------------------------

// Rewrites the first OpDecorate ... Binding operand to a deliberately wrong value.
[[nodiscard]] bool mutateFirstBinding(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kOpDecorate = 71u;
    constexpr std::uint32_t kBinding = 33u;
    for (std::size_t index = 5; index < words.size();) {
        const std::uint32_t instruction = words[index];
        const std::size_t wordCount = instruction >> 16U;
        const std::uint32_t opcode = instruction & 0xFFFFU;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return false;
        }
        if (opcode == kOpDecorate && wordCount >= 4 && words[index + 2] == kBinding) {
            words[index + 3] ^= 0x100U;
            return true;
        }
        index += wordCount;
    }
    return false;
}

// Rewrites the first storage-image (Sampled == 2) type's dimensionality to 3D.
[[nodiscard]] bool mutateFirstStorageImageDim(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kOpTypeImage = 25u;
    for (std::size_t index = 5; index < words.size();) {
        const std::uint32_t instruction = words[index];
        const std::size_t wordCount = instruction >> 16U;
        const std::uint32_t opcode = instruction & 0xFFFFU;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return false;
        }
        if (opcode == kOpTypeImage && wordCount >= 9 && words[index + 7] == 2U) {
            words[index + 3] = 2U; // force 3D
            return true;
        }
        index += wordCount;
    }
    return false;
}

// Rewrites the LocalSize x operand of the compute execution mode.
[[nodiscard]] bool mutateLocalSizeX(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kOpExecutionMode = 16u;
    constexpr std::uint32_t kLocalSize = 17u;
    for (std::size_t index = 5; index < words.size();) {
        const std::uint32_t instruction = words[index];
        const std::size_t wordCount = instruction >> 16U;
        const std::uint32_t opcode = instruction & 0xFFFFU;
        if (wordCount == 0 || index + wordCount > words.size()) {
            return false;
        }
        if (opcode == kOpExecutionMode && wordCount >= 6 && words[index + 2] == kLocalSize) {
            words[index + 3] = 32U;
            return true;
        }
        index += wordCount;
    }
    return false;
}

// Real extracted artifacts must reflect the declared interface; a mutated descriptor binding/type,
// a changed workgroup size, or a foreign compiled module must be refused typed ShaderRejected
// before any native pipeline, and the untouched valid artifact must still create and run.
void testShaderInterfaceReflection(Expectations& expectations, GpuDevice& device,
                                   const bloom::color::ResolvedBloomNeutralConfig& aces) {
    using bloom::render::GpuOcioProgramDiagnosticCode;
    using bloom::render::ocio_program_detail::validateOcioShaderInterface;
    constexpr std::uint32_t kWorkgroup = 64;

    auto desc = bloom::color::buildOcioGpuProgramForDisplay(
        aces, "Rec.2100-PQ - Display", "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)");
    expectations.expect(desc.succeeded(), "the reflection display program extracts");
    if (!desc.succeeded()) {
        return;
    }
    const auto validSpirv = compileGlsl(buildWrapperGlsl(*desc.program(), true));
    expectations.expect(validSpirv.has_value(), "the reflection display wrapper compiles");
    if (!validSpirv.has_value()) {
        return;
    }
    expectations.expect(
        validateOcioShaderInterface(*desc.program(), *validSpirv, kWorkgroup).valid(),
        "the real extracted artifact reflects the declared interface");

    const auto refusedByCreate = [&](const std::vector<std::uint32_t>& spirv) {
        auto created = GpuOcioProgram::create(device, *desc.program(), spirv, {});
        return !created && created.diagnostic.code == GpuOcioProgramDiagnosticCode::ShaderRejected;
    };

    auto bindingMutated = *validSpirv;
    expectations.expect(mutateFirstBinding(bindingMutated),
                        "a binding decoration is present to mutate");
    expectations.expect(
        !validateOcioShaderInterface(*desc.program(), bindingMutated, kWorkgroup).valid(),
        "a mutated descriptor binding is refused by reflection");
    expectations.expect(refusedByCreate(bindingMutated),
                        "a mutated binding is refused ShaderRejected before the native pipeline");

    auto typeMutated = *validSpirv;
    expectations.expect(mutateFirstStorageImageDim(typeMutated),
                        "a storage-image type is present to mutate");
    expectations.expect(
        !validateOcioShaderInterface(*desc.program(), typeMutated, kWorkgroup).valid(),
        "a mutated image dimensionality is refused by reflection");
    expectations.expect(
        refusedByCreate(typeMutated),
        "a mutated image type is refused ShaderRejected before the native pipeline");

    auto sizeMutated = *validSpirv;
    expectations.expect(mutateLocalSizeX(sizeMutated), "a LocalSize execution mode is present");
    expectations.expect(
        !validateOcioShaderInterface(*desc.program(), sizeMutated, kWorkgroup).valid(),
        "a changed workgroup size is refused by reflection");
    expectations.expect(refusedByCreate(sizeMutated),
                        "a changed workgroup size is refused ShaderRejected");

    auto effectDesc = bloom::color::buildOcioGpuProgramForCst(aces, "ACES2065-1", "ACEScg");
    if (effectDesc.succeeded()) {
        const auto foreign = compileGlsl(buildWrapperGlsl(*effectDesc.program(), false));
        expectations.expect(foreign.has_value(), "the foreign effect wrapper compiles");
        if (foreign.has_value()) {
            expectations.expect(
                !validateOcioShaderInterface(*desc.program(), *foreign, kWorkgroup).valid(),
                "a foreign compiled module is refused by reflection");
            expectations.expect(refusedByCreate(*foreign),
                                "a foreign module is refused ShaderRejected");
        }
    }

    // The untouched valid artifact still creates and runs to a published RGBA8 display output.
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(),
                        "the valid artifact still creates after the refusals");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the reflection input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = program->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                        "the valid display begin is accepted after the refusals");
    if (accepted.code != GpuOcioProgramDiagnosticCode::None) {
        return;
    }
    if (!pollOcio(expectations, *program, "reflection display")) {
        return;
    }
    expectations.expect(program->takeDisplayOutput().isValid(),
                        "the valid display output publishes after the refusals");
}

} // namespace bloom::color::ocio_resources_test
