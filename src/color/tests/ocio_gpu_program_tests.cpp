#include <bloom/color/ocio_gpu_program.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::color::OcioBuiltInRegistryOutcome;
using bloom::color::OcioConfigLocatorKind;
using bloom::render::OcioGpuProgramDesc;
using bloom::render::OcioGpuProgramError;
using bloom::render::OcioGpuProgramLimits;

[[nodiscard]] bloom::color::OcioBuiltInResolutionResult neutralConfig() {
    return bloom::color::resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                                      bloom::color::kBloomNeutralV1ConfigUri,
                                                      bloom::color::kBloomNeutralV1ConfigDigest);
}

[[nodiscard]] bloom::color::OcioBuiltInResolutionResult acesConfig() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return bloom::color::resolveOcioBuiltIn(
            OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
            bloom::color::kBloomNeutralV1ConfigDigest, "ACEScg");
    }
    return bloom::color::resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                            bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
}

[[nodiscard]] bloom::render::OcioGpuProgramResult
firstDisplayProgramWithTexture(const bloom::color::ResolvedBloomNeutralConfig& resolved) {
    for (const auto& candidate : resolved.displays()) {
        auto result = bloom::color::buildOcioGpuProgramForDisplay(resolved, candidate.display,
                                                                  candidate.view);
        if (result.succeeded() && result.program() != nullptr &&
            !result.program()->textures.empty()) {
            return result;
        }
    }
    return bloom::render::OcioGpuProgramResult::failure(
        bloom::render::OcioGpuProgramError::UnsupportedResourceForm);
}

void testNeutralDisplayExtraction(Expectations& expectations) {
    auto resolution = neutralConfig();
    expectations.expect(resolution.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "the Bloom Neutral built-in resolves for GPU extraction");
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return;
    }
    const auto first = bloom::color::buildOcioGpuProgramForDisplay(
        *resolved, resolved->displayName(), resolved->viewName());
    expectations.expect(first.succeeded(), "the Bloom Neutral display GPU program extracts");
    const OcioGpuProgramDesc* program = first.program();
    if (program == nullptr) {
        return;
    }
    expectations.expect(program->stage == bloom::render::OcioGpuProgramStage::DisplayPacking &&
                            program->semanticsId == bloom::color::kOcioGpuDisplaySemanticsId,
                        "the display program carries the display stage and semantics id");
    expectations.expect(program->functionName == "bloom_ocio_transform" &&
                            program->resourcePrefix == "bloom_ocio_" &&
                            !program->shaderText.empty(),
                        "the extracted program carries the stable OCIO function identity");
    expectations.expect(program->contentIdentity != bloom::core::Sha256Digest{} &&
                            program->shaderTextDigest ==
                                bloom::render::computeOcioGpuShaderTextDigest(program->shaderText),
                        "the content identity and shader digest are genuine and consistent");
    expectations.expect(bloom::render::validateOcioGpuProgram(*program, {}) ==
                            OcioGpuProgramError::None,
                        "the extracted program satisfies the default resource limits");

    const auto second = bloom::color::buildOcioGpuProgramForDisplay(
        *resolved, resolved->displayName(), resolved->viewName());
    expectations.expect(second.succeeded() && second.program() != nullptr &&
                            second.program()->contentIdentity == program->contentIdentity,
                        "extraction is deterministic for the same config and transform");

    // CPU oracle metadata: the display program and the CPU display processor must report the same
    // OCIO version and display/view for one request.
    const auto cpu = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (const auto* handle = cpu.handle(); handle != nullptr) {
        expectations.expect(handle->provenance().ocioVersion == program->ocioVersion &&
                                handle->provenance().displayName == resolved->displayName() &&
                                handle->provenance().viewName == resolved->viewName(),
                            "the GPU program and CPU oracle retain the same OCIO provenance");
    }
}

void testAcesCstExtraction(Expectations& expectations) {
    auto resolution = acesConfig();
    expectations.expect(resolution.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "the ACES CG built-in resolves for GPU extraction");
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return;
    }
    const auto cst = bloom::color::buildOcioGpuProgramForCst(*resolved, "ACES2065-1", "ACEScg");
    expectations.expect(cst.succeeded(), "ACES2065-1 to ACEScg extracts a GPU program");
    const OcioGpuProgramDesc* program = cst.program();
    if (program == nullptr) {
        return;
    }
    expectations.expect(program->stage == bloom::render::OcioGpuProgramStage::ProcessEffect &&
                            program->semanticsId == bloom::color::kOcioGpuCstSemanticsId,
                        "the CST program carries the process-effect stage and semantics id");
    expectations.expect(bloom::render::validateOcioGpuProgram(*program, {}) ==
                            OcioGpuProgramError::None,
                        "the ACES CST program satisfies the default resource limits");
    expectations.expect(program->contentIdentity != bloom::core::Sha256Digest{} &&
                            program->shaderTextDigest ==
                                bloom::render::computeOcioGpuShaderTextDigest(program->shaderText),
                        "the CST content identity and shader digest are genuine");

    const auto second = bloom::color::buildOcioGpuProgramForCst(*resolved, "ACES2065-1", "ACEScg");
    expectations.expect(second.succeeded() && second.program() != nullptr &&
                            second.program()->contentIdentity == program->contentIdentity,
                        "CST extraction is deterministic for the same transform");
    const auto other = bloom::color::buildOcioGpuProgramForCst(*resolved, "ACES2065-1", "ACEScct");
    expectations.expect(other.succeeded() && other.program() != nullptr &&
                            other.program()->contentIdentity != program->contentIdentity,
                        "a different destination colour space mutates the content identity");

    const auto identity = bloom::color::buildOcioGpuProgramForCst(*resolved, "ACEScg", "ACEScg");
    expectations.expect(identity.error() == OcioGpuProgramError::IdentityTransform,
                        "an equal CST is refused as an identity, never fabricated");

    const auto missing =
        bloom::color::buildOcioGpuProgramForCst(*resolved, "NotAColorSpace", "ACEScg");
    expectations.expect(missing.error() == OcioGpuProgramError::UnsupportedColorSpace,
                        "an unknown colour space is a typed refusal");
    const auto invalid = bloom::color::buildOcioGpuProgramForCst(*resolved, "", "ACEScg");
    expectations.expect(invalid.error() == OcioGpuProgramError::InvalidRequest,
                        "an empty colour-space id is an invalid request");
}

void testAcesDisplayExtraction(Expectations& expectations) {
    auto resolution = acesConfig();
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        expectations.expect(false, "the ACES config is available for display extraction");
        return;
    }
    const auto entry = std::find_if(resolved->displays().begin(), resolved->displays().end(),
                                    [](const auto& candidate) { return candidate.isDefault; });
    if (entry == resolved->displays().end()) {
        expectations.expect(false, "the ACES config declares a default display/view");
        return;
    }
    const auto program =
        bloom::color::buildOcioGpuProgramForDisplay(*resolved, entry->display, entry->view);
    expectations.expect(program.succeeded(),
                        "a non-Bloom-Neutral display/view extracts on the GPU path");
    if (program.program() != nullptr) {
        const auto& desc = *program.program();
        expectations.expect(desc.semanticsId == bloom::color::kOcioGpuDisplaySemanticsId,
                            "the ACES display program carries the display semantics id");
        expectations.expect(bloom::render::validateOcioGpuProgram(desc, {}) ==
                                OcioGpuProgramError::None,
                            "the ACES display program satisfies the default resource limits");
        const auto again =
            bloom::color::buildOcioGpuProgramForDisplay(*resolved, entry->display, entry->view);
        expectations.expect(again.program() != nullptr &&
                                again.program()->contentIdentity == desc.contentIdentity,
                            "display extraction is deterministic for one display/view");
    }

    const auto withTexture = firstDisplayProgramWithTexture(*resolved);
    expectations.expect(withTexture.succeeded(),
                        "at least one ACES display/view preserves OCIO LUT textures");
    if (withTexture.program() != nullptr) {
        bool declared = !withTexture.program()->textures.empty();
        for (const auto& texture : withTexture.program()->textures) {
            declared = declared &&
                       texture.interpolation != bloom::render::OcioGpuInterpolation::Unknown &&
                       !texture.samples.empty();
        }
        expectations.expect(declared,
                            "every extracted LUT texture preserves its declared interpolation");
    }

    const auto unknown =
        bloom::color::buildOcioGpuProgramForDisplay(*resolved, "No Such Display", entry->view);
    expectations.expect(unknown.error() == OcioGpuProgramError::InvalidRequest,
                        "an unenumerated display/view is a typed refusal");
}

void testResourceAndIdentityMutations(Expectations& expectations) {
    auto resolution = acesConfig();
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return;
    }
    auto built = firstDisplayProgramWithTexture(*resolved);
    if (built.program() == nullptr || built.program()->textures.empty()) {
        expectations.expect(false,
                            "an ACES display program carries LUT textures for mutation coverage");
        return;
    }
    OcioGpuProgramDesc program = *built.program();
    const auto originalResources = program.resourceDigest;

    program.textures.front().samples.front() += 0.25F;
    expectations.expect(bloom::render::computeOcioGpuResourceDigest(program) != originalResources,
                        "a LUT sample change mutates the resource digest");
    program = *built.program();
    program.uniformBufferSize += 16;
    expectations.expect(bloom::render::computeOcioGpuResourceDigest(program) != originalResources,
                        "a uniform-buffer size change mutates the resource digest");
    expectations.expect(bloom::render::computeOcioGpuShaderTextDigest(program.shaderText + "\n") !=
                            program.shaderTextDigest,
                        "a shader-text change mutates the shader digest");

    const std::array<std::byte, 2> semanticsA{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 2> semanticsB{std::byte{1}, std::byte{3}};
    const auto partsA = bloom::render::OcioGpuContentIdentityParts{
        .semanticsId = bloom::color::kOcioGpuCstSemanticsId,
        .semanticIdentityBytes = semanticsA,
        .ocioConfigRevision = resolved->expectedRevision(),
        .shaderTextDigest = program.shaderTextDigest,
        .resourceDigest = originalResources,
    };
    auto partsB = partsA;
    partsB.semanticIdentityBytes = semanticsB;
    const auto identityA = bloom::render::computeOcioGpuContentIdentity(partsA);
    expectations.expect(identityA == bloom::render::computeOcioGpuContentIdentity(partsA) &&
                            identityA != originalResources,
                        "the content identity is reproducible and distinct from a resource digest");
    expectations.expect(identityA != bloom::render::computeOcioGpuContentIdentity(partsB),
                        "a transform-semantics change mutates the content identity");

    // Bounds: every mutation returns a typed refusal, never a truncated program. The 3D-LUT edge
    // ceiling is exercised on a synthetic descriptor because the built-in ACES GPU processor emits
    // analytic/1D resources only; a real 3D LUT requires the isolated file-transform boundary.
    OcioGpuProgramDesc bounds;
    bounds.shaderText = "void main() {}";
    bounds.functionName = "bloom_ocio_transform";
    bounds.semanticsId = "bloom.test";
    bloom::render::OcioGpuTextureDesc overLimit3d;
    overLimit3d.binding = 1;
    overLimit3d.name = "bloom_ocio_lut3d";
    overLimit3d.dimensions = bloom::render::OcioGpuTextureDimensions::ThreeD;
    overLimit3d.edgeLength = 4096;
    bounds.textures.push_back(std::move(overLimit3d));
    expectations.expect(bloom::render::validateOcioGpuProgram(bounds, {}) ==
                            OcioGpuProgramError::ResourceLimitExceeded,
                        "an over-limit 3D LUT edge is refused");

    OcioGpuProgramDesc mismatched = *built.program();
    mismatched.textures.front().samples.push_back(0.0F);
    expectations.expect(bloom::render::validateOcioGpuProgram(mismatched, {}) ==
                            OcioGpuProgramError::UnsupportedResourceForm,
                        "a LUT sample/dimension mismatch is refused");
    {
        OcioGpuProgramLimits tiny;
        tiny.maxShaderBytes = 8;
        expectations.expect(bloom::render::validateOcioGpuProgram(*built.program(), tiny) ==
                                OcioGpuProgramError::ResourceLimitExceeded,
                            "an over-limit shader byte count is refused");
    }
}

void testFileTransformBoundary(Expectations& expectations) {
    auto resolution = neutralConfig();
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return;
    }
    bloom::core::Sha256Digest::Bytes digestBytes{};
    digestBytes[0] = 1U;
    const auto digest = bloom::core::Sha256Digest::fromBytes(digestBytes);
    const auto boundary = bloom::color::buildOcioGpuProgramForFileTransform(
        *resolved, "lin_rec709_scene", digest, 3, bloom::color::LutInterpolation::Tetrahedral,
        bloom::color::LutDirection::Forward, "lin_rec709_scene");
    expectations.expect(boundary.error() == OcioGpuProgramError::ExternalLutBoundaryRequired,
                        "a file transform is refused in-process at the explicit LUT boundary");
    const auto invalid = bloom::color::buildOcioGpuProgramForFileTransform(
        *resolved, "lin_rec709_scene", digest, 9, bloom::color::LutInterpolation::Linear,
        bloom::color::LutDirection::Forward, "lin_rec709_scene");
    expectations.expect(invalid.error() == OcioGpuProgramError::InvalidRequest,
                        "an unsupported LUT format is an invalid request");
}

void testWrapperFixtureCompiles(Expectations& expectations) {
#if defined(BLOOM_OCIO_GPU_WRAPPER_FIXTURE) && defined(BLOOM_GPUSHADER_TOOLS_DIR)
    const std::filesystem::path tools{BLOOM_GPUSHADER_TOOLS_DIR};
    const std::filesystem::path glslang = tools / "glslangValidator";
    const std::filesystem::path spirvVal = tools / "spirv-val";
    const std::filesystem::path fixture{BLOOM_OCIO_GPU_WRAPPER_FIXTURE};
    if (!std::filesystem::exists(glslang) || !std::filesystem::exists(spirvVal) ||
        !std::filesystem::exists(fixture)) {
        std::cout << "SKIP: pinned glslangValidator/spirv-val or wrapper fixture unavailable\n";
        return;
    }
    const auto spv = std::filesystem::temp_directory_path() / "bloom_ocio_wrapper_fixture.spv";
    const std::string compile = "\"" + glslang.string() + "\" --target-env vulkan1.2 -V \"" +
                                fixture.string() + "\" -o \"" + spv.string() + "\"";
    if (std::system(compile.c_str()) != 0) {
        expectations.expect(false, "the pinned wrapper fixture compiles with glslangValidator");
        return;
    }
    const std::string validate =
        "\"" + spirvVal.string() + "\" --target-env vulkan1.2 \"" + spv.string() + "\"";
    expectations.expect(std::system(validate.c_str()) == 0,
                        "the compiled wrapper fixture passes spirv-val");
    std::error_code ignored;
    std::filesystem::remove(spv, ignored);
#else
    std::cout << "SKIP: pinned glslang/spirv-val fixture paths not configured\n";
#endif
}

} // namespace

int main() {
    Expectations expectations;
    testNeutralDisplayExtraction(expectations);
    testAcesCstExtraction(expectations);
    testAcesDisplayExtraction(expectations);
    testResourceAndIdentityMutations(expectations);
    testFileTransformBoundary(expectations);
    testWrapperFixtureCompiles(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO GPU program expectation(s) failed\n";
        return 1;
    }
    std::cout << "OCIO GPU program extraction expectations passed\n";
    return 0;
}
