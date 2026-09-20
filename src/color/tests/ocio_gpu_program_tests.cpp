#include "ocio_gpu_program_test_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::color::OcioBuiltInRegistryOutcome;
using bloom::color::OcioConfigLocatorKind;
using bloom::color::gpu_program_test::acesConfig;
using bloom::color::gpu_program_test::Expectations;
using bloom::color::gpu_program_test::neutralConfig;
using bloom::render::OcioGpuProgramDesc;
using bloom::render::OcioGpuProgramError;
using bloom::render::OcioGpuProgramLimits;

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

#ifdef __linux__
// --- File-transform LUT fixtures, written to a scratch directory at runtime. -------------------
// Linux-only: these fixtures feed the isolated-worker parsing cases below, which a non-Linux build
// must not run in-process.

void writeCube1d(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "LUT_1D_SIZE 8\n";
    for (int index = 0; index < 8; ++index) {
        const float value = static_cast<float>(index) / 7.0F;
        file << value << ' ' << value * value << ' ' << 0.5F * value << '\n';
    }
}

void writeCube3d(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "LUT_3D_SIZE 3\n";
    for (int b = 0; b < 3; ++b) {
        for (int g = 0; g < 3; ++g) {
            for (int r = 0; r < 3; ++r) {
                const float red = static_cast<float>(r) / 2.0F;
                const float green = static_cast<float>(g) / 2.0F;
                const float blue = static_cast<float>(b) / 2.0F;
                file << red << ' ' << green * green << ' ' << blue << '\n';
            }
        }
    }
}

void writeClf(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ProcessList id=\"identity\" compCLFversion=\"3\">\n"
            "<Matrix inBitDepth=\"32f\" outBitDepth=\"32f\"><Array dim=\"3 3\">1 0 0 0 1 0 0 0 "
            "1</Array></Matrix>\n"
            "</ProcessList>\n";
}

void writeSpi1d(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "Version 1\nFrom 0 1\nLength 4\nComponents 3\n{\n0 0 0\n0.3333 0.1111 0.1667\n0.6667 "
            "0.4444 0.3333\n1 1 0.5\n}\n";
}

void writeSpi3d(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "SPILUT 1.0\n3 3\n2 2 2\n";
    for (int r = 0; r < 2; ++r) {
        for (int g = 0; g < 2; ++g) {
            for (int b = 0; b < 2; ++b) {
                const std::string triple =
                    std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b);
                file << r << ' ' << g << ' ' << b << ' ' << triple << '\n';
            }
        }
    }
}

// On a qualified Linux host each supported format must reach the isolated helper and return a real,
// accepted program. A non-Linux build must not parse in-process and returns the typed boundary.
void testFileTransformExtraction(Expectations& expectations,
                                 const bloom::color::ResolvedBloomNeutralConfig& resolved,
                                 const std::filesystem::path& root) {
    using bloom::color::LutDirection;
    using bloom::color::LutInterpolation;
    const auto process = std::string(resolved.processColorSpaceId());
    const auto& working = process;

    struct FormatCase final {
        const char* name;
        void (*write)(const std::filesystem::path&);
        std::uint32_t format;
        bool threeD;
    };
    const std::array cases{
        FormatCase{"curve1d.cube", &writeCube1d, 1, false},
        FormatCase{"volume3d.cube", &writeCube3d, 1, true},
        FormatCase{"identity.clf", &writeClf, 2, false},
        FormatCase{"curve1d.spi1d", &writeSpi1d, 3, false},
        FormatCase{"volume3d.spi3d", &writeSpi3d, 4, true},
    };
    for (const auto& testCase : cases) {
        const auto path = root / testCase.name;
        testCase.write(path);
        const auto file = bloom::color::readLutFile(path);
        expectations.expect(file.error == bloom::color::LutError::None,
                            std::string("LUT fixture passes preflight: ") + testCase.name);
        if (file.error != bloom::color::LutError::None) {
            continue;
        }
        expectations.expect(file.format == testCase.format,
                            std::string("LUT fixture format is derived: ") + testCase.name);
        const auto program = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, file, LutInterpolation::Best, LutDirection::Forward, process, working);
        if (!program.succeeded()) {
            std::cerr << "  file-transform " << testCase.name
                      << " error=" << bloom::render::ocioGpuProgramErrorName(program.error())
                      << '\n';
        }
        expectations.expect(program.succeeded(),
                            std::string("FileTransform GPU program extracts: ") + testCase.name);
        if (program.program() == nullptr) {
            continue;
        }
        const auto& desc = *program.program();
        expectations.expect(desc.stage == bloom::render::OcioGpuProgramStage::ProcessEffect &&
                                desc.semanticsId == bloom::color::kOcioGpuFileTransformSemanticsId,
                            "the FileTransform program carries the process-effect semantics id");
        expectations.expect(bloom::render::validateOcioGpuProgram(desc, {}) ==
                                OcioGpuProgramError::None,
                            "the FileTransform program satisfies the default resource limits");
        expectations.expect(desc.contentIdentity != bloom::core::Sha256Digest{} &&
                                desc.shaderTextDigest ==
                                    bloom::render::computeOcioGpuShaderTextDigest(desc.shaderText),
                            "the FileTransform content identity and shader digest are genuine");
        if (testCase.threeD) {
            const bool has3d =
                std::any_of(desc.textures.begin(), desc.textures.end(), [](const auto& texture) {
                    return texture.dimensions == bloom::render::OcioGpuTextureDimensions::ThreeD &&
                           texture.edgeLength >= 2 && !texture.samples.empty();
                });
            expectations.expect(has3d,
                                std::string("3D LUT resource is preserved: ") + testCase.name);
        }
        const auto again = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, file, LutInterpolation::Best, LutDirection::Forward, process, working);
        expectations.expect(again.succeeded() && again.program() != nullptr &&
                                again.program()->contentIdentity == desc.contentIdentity,
                            std::string("FileTransform extraction is deterministic: ") +
                                testCase.name);
    }

    const auto cubePath = root / "volume3d.cube";
    const auto cube = bloom::color::readLutFile(cubePath);
    if (cube.error != bloom::color::LutError::None) {
        expectations.expect(false, "the 3D cube fixture is available for mutation coverage");
        return;
    }
    // Inverse direction is a distinct, real OCIO GPU program.
    {
        const auto forward = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Forward, process, working);
        const auto inverse = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Inverse, process, working);
        if (!inverse.succeeded() || inverse.program() == nullptr) {
            std::cerr << "  inverse file-transform error="
                      << bloom::render::ocioGpuProgramErrorName(inverse.error()) << '\n';
        }
        expectations.expect(forward.succeeded() && inverse.succeeded(),
                            "forward and inverse FileTransforms both extract");
        if (forward.program() != nullptr && inverse.program() != nullptr) {
            expectations.expect(forward.program()->contentIdentity !=
                                    inverse.program()->contentIdentity,
                                "the inverse direction mutates the content identity");
        }
    }
    // Interpolation participates in the content identity.
    {
        const auto linear = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, cube, LutInterpolation::Linear, LutDirection::Forward, process, working);
        const auto tetrahedral = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Forward, process, working);
        expectations.expect(linear.succeeded() && tetrahedral.succeeded(),
                            "linear and tetrahedral FileTransforms both extract");
        if (linear.program() != nullptr && tetrahedral.program() != nullptr) {
            expectations.expect(linear.program()->contentIdentity !=
                                    tetrahedral.program()->contentIdentity,
                                "the interpolation choice mutates the content identity");
        }
    }
    // Working-color semantics: a different valid non-data space changes the identity; an unknown id
    // is refused against the exact config.
    {
        const auto other =
            std::find_if(resolved.colorSpaces().begin(), resolved.colorSpaces().end(),
                         [&](const auto& space) { return space.id != process; });
        if (other != resolved.colorSpaces().end()) {
            const auto changed = bloom::color::buildOcioGpuProgramForFileTransform(
                resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Forward, other->id,
                process);
            const auto baseline = bloom::color::buildOcioGpuProgramForFileTransform(
                resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Forward, process,
                process);
            expectations.expect(changed.succeeded() && baseline.succeeded() &&
                                    changed.program() != nullptr && baseline.program() != nullptr &&
                                    changed.program()->contentIdentity !=
                                        baseline.program()->contentIdentity,
                                "the process/working-space semantics mutate the content identity");
        }
        const auto unknown = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, cube, LutInterpolation::Tetrahedral, LutDirection::Forward, "No Such Space",
            process);
        expectations.expect(unknown.error() == OcioGpuProgramError::UnsupportedColorSpace,
                            "an unknown process space is a typed refusal");
    }
    // Changed digest, malformed LUT, oversize LUT, cancellation, and a genuine helper failure.
    {
        auto changed = cube;
        bloom::core::Sha256Digest::Bytes changedBytes{};
        std::copy(changed.digest.bytes().begin(), changed.digest.bytes().end(),
                  changedBytes.begin());
        changedBytes[0] = static_cast<std::uint8_t>(changedBytes[0] ^ 0xffU);
        changed.digest = bloom::core::Sha256Digest::fromBytes(changedBytes);
        expectations.expect(
            bloom::color::buildOcioGpuProgramForFileTransform(
                resolved, changed, LutInterpolation::Best, LutDirection::Forward, process, working)
                    .error() == OcioGpuProgramError::InvalidRequest,
            "a changed LUT digest is refused before IPC");

        auto malformed = cube;
        const std::string junk = "LUT_3D_SIZE 2\n0 0 0\n";
        const auto junkBytes = std::as_bytes(std::span(junk.data(), junk.size()));
        malformed.bytes.assign(junkBytes.begin(), junkBytes.end());
        const auto malformedDigest = bloom::core::Sha256Hasher::hash(malformed.bytes);
        expectations.expect(malformedDigest.has_value(), "the malformed LUT digest is computable");
        if (malformedDigest)
            malformed.digest = *malformedDigest;
        expectations.expect(bloom::color::buildOcioGpuProgramForFileTransform(
                                resolved, malformed, LutInterpolation::Best, LutDirection::Forward,
                                process, working)
                                    .error() == OcioGpuProgramError::UnsupportedResourceForm,
                            "a malformed LUT is a typed refusal");

        auto oversized = cube;
        oversized.bytes.resize(bloom::color::kMaximumLutBytes + 1);
        expectations.expect(bloom::color::buildOcioGpuProgramForFileTransform(
                                resolved, oversized, LutInterpolation::Best, LutDirection::Forward,
                                process, working)
                                    .error() == OcioGpuProgramError::ResourceLimitExceeded,
                            "an oversized LUT is refused before allocation");

        expectations.expect(bloom::color::buildOcioGpuProgramForFileTransform(
                                resolved, cube, LutInterpolation::Best, LutDirection::Forward,
                                process, working, {},
                                [] {
                                    return true;
                                }).error() == OcioGpuProgramError::Cancelled,
                            "cancellation before IPC is a typed Cancelled failure");

        // A CLF whose ProcessList opens but is not a valid OCIO document passes the host's cheap
        // structural preflight and fails inside the isolated OCIO parser: a real helper failure.
        const auto brokenPath = root / "broken.clf";
        {
            std::ofstream file(brokenPath);
            file << "<ProcessList>";
        }
        const auto broken = bloom::color::readLutFile(brokenPath);
        expectations.expect(broken.error == bloom::color::LutError::None,
                            "the broken CLF reaches the isolated parser");
        const auto refusal = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, broken, LutInterpolation::Best, LutDirection::Forward, process, working);
        expectations.expect(refusal.error() == OcioGpuProgramError::UnsupportedResourceForm,
                            "a helper parse failure stays a typed non-opted-out refusal");
    }
    // A truly unsupported extension never becomes a LUT.
    {
        const auto notLut = root / "not-a-lut.txt";
        {
            std::ofstream file(notLut);
            file << "LUT_3D_SIZE 2\n0 0 0\n";
        }
        const auto unknown = bloom::color::readLutFile(notLut);
        expectations.expect(unknown.error == bloom::color::LutError::UnsupportedFormat,
                            "an unsupported extension is not admitted as a LUT");
        expectations.expect(
            bloom::color::buildOcioGpuProgramForFileTransform(
                resolved, unknown, LutInterpolation::Best, LutDirection::Forward, process, working)
                    .error() == OcioGpuProgramError::UnsupportedResourceForm,
            "the unsupported LUT stays a typed failure, not a blanket opt-out");
    }
}
#endif

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
    // NOLINTNEXTLINE(bugprone-command-processor) -- bounded test fixture: pinned build tool path.
    if (std::system(compile.c_str()) != 0) {
        expectations.expect(false, "the pinned wrapper fixture compiles with glslangValidator");
        return;
    }
    const std::string validate =
        "\"" + spirvVal.string() + "\" --target-env vulkan1.2 \"" + spv.string() + "\"";
    // NOLINTNEXTLINE(bugprone-command-processor) -- bounded test fixture: pinned build tool path.
    expectations.expect(std::system(validate.c_str()) == 0,
                        "the compiled wrapper fixture passes spirv-val");
    std::error_code ignored;
    std::filesystem::remove(spv, ignored);
#else
    static_cast<void>(expectations);
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
    bloom::color::gpu_program_test::testSerializationRoundTrip(expectations);
    bloom::color::gpu_program_test::testPerSamplerSamplingAdapter(expectations);
#ifdef __linux__
    auto resolution = neutralConfig();
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral.has_value()) {
        expectations.expect(false, "the Bloom Neutral built-in resolves for file transforms");
    } else {
        const auto root = std::filesystem::current_path() / "color3-ocio-gpu-lut-fixtures";
        std::filesystem::create_directories(root);
        testFileTransformExtraction(expectations, *neutral, root);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
#endif
    testWrapperFixtureCompiles(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO GPU program expectation(s) failed\n";
        return 1;
    }
    std::cout << "OCIO GPU program extraction expectations passed\n";
    return 0;
}
