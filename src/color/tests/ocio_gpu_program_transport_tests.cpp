// Focused unit tests for the portable OCIO GPU program transport codec and the shared per-sampler
// precise-sampling adapter. Kept separate from the extraction tests so each handwritten test file
// stays small.

#include "ocio_gpu_program_serialize.hpp"
#include "ocio_gpu_program_test_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace bloom::color::gpu_program_test {

void testSerializationRoundTrip(Expectations& expectations) {
    using bloom::render::OcioGpuProgramError;
    using bloom::render::OcioGpuProgramLimits;
    auto resolution = neutralConfig();
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return;
    }
    auto built =
        buildOcioGpuProgramForDisplay(*resolved, resolved->displayName(), resolved->viewName());
    if (built.program() == nullptr) {
        expectations.expect(false, "a display program exists for serialization coverage");
        return;
    }
    const auto* original = built.program();
    const auto encoded = detail::serializeOcioGpuProgram(*original, kMaximumLutBytes);
    expectations.expect(encoded.has_value(), "a validated program serializes within the bound");
    if (!encoded.has_value()) {
        return;
    }
    auto restored = detail::deserializeOcioGpuProgram(*encoded, {});
    expectations.expect(restored.succeeded(), "the transported payload deserializes");
    if (restored.reflected() != nullptr) {
        const auto& copy = *restored.reflected();
        expectations.expect(copy.shaderText == original->shaderText &&
                                copy.functionName == original->functionName &&
                                copy.semanticsId == original->semanticsId &&
                                copy.stage == original->stage &&
                                copy.textures.size() == original->textures.size() &&
                                copy.uniformBufferData == original->uniformBufferData,
                            "every transported field round-trips exactly");
        // The host must recompute the identity, never trust a transported one.
        expectations.expect(copy.contentIdentity == bloom::core::Sha256Digest{},
                            "the content identity is recomputed host-side, never transported");
        expectations.expect(bloom::render::validateOcioGpuProgram(copy, {}) ==
                                OcioGpuProgramError::None,
                            "the transported program passes host validation");
    }
    {
        auto truncated = *encoded;
        truncated.pop_back();
        expectations.expect(detail::deserializeOcioGpuProgram(truncated, {}).error ==
                                OcioGpuProgramError::UnsupportedResourceForm,
                            "a truncated payload is refused");
    }
    {
        auto trailing = *encoded;
        trailing.push_back(std::byte{0});
        expectations.expect(detail::deserializeOcioGpuProgram(trailing, {}).error ==
                                OcioGpuProgramError::UnsupportedResourceForm,
                            "a trailing-byte payload is refused");
    }
    {
        auto corrupt = *encoded;
        corrupt[0] ^= std::byte{0xff};
        expectations.expect(detail::deserializeOcioGpuProgram(corrupt, {}).error ==
                                OcioGpuProgramError::UnsupportedResourceForm,
                            "a corrupted magic is refused");
    }
    {
        OcioGpuProgramLimits tiny;
        tiny.maxShaderBytes = 4;
        expectations.expect(detail::deserializeOcioGpuProgram(*encoded, tiny).error ==
                                OcioGpuProgramError::ResourceLimitExceeded,
                            "a payload beyond the caller's limit is refused");
    }
}

void testPerSamplerSamplingAdapter(Expectations& expectations) {
    bloom::render::OcioGpuProgramDesc program;
    program.functionName = "bloom_ocio_transform";
    program.shaderText = "// generated body\n";
    program.semanticsId = "bloom.test";
    bloom::render::OcioGpuTextureDesc linear;
    linear.binding = 1;
    linear.name = "bloom_ocio_lut1d_0";
    linear.samplerName = "bloom_ocio_lut1d_0Sampler";
    linear.dimensions = bloom::render::OcioGpuTextureDimensions::OneD;
    linear.width = 4;
    linear.height = 1;
    linear.interpolation = bloom::render::OcioGpuInterpolation::Linear;
    bloom::render::OcioGpuTextureDesc nearest;
    nearest.binding = 2;
    nearest.name = "bloom_ocio_lut3d_0";
    nearest.samplerName = "bloom_ocio_lut3d_0Sampler";
    nearest.dimensions = bloom::render::OcioGpuTextureDimensions::ThreeD;
    nearest.edgeLength = 2;
    nearest.interpolation = bloom::render::OcioGpuInterpolation::Nearest;
    program.textures = {linear, nearest};

    // A mixed program must keep one precise function per sampler rather than disabling the adapter
    // for the whole descriptor.
    const auto sampling = ocioGpuSamplingGlslFor(program);
    expectations.expect(sampling.dispatches(), "a mixed program dispatches per-sampler sampling");
    expectations.expect(sampling.preamble.find("bloom_ocio_sample_bloom_ocio_lut1d_0Sampler") !=
                                std::string::npos &&
                            sampling.preamble.find("bloom_ocio_sample_bloom_ocio_lut3d_0Sampler") !=
                                std::string::npos &&
                            sampling.preamble.find("#define texture") != std::string::npos,
                        "the mixed preamble declares both samplers and the dispatch macro");
    expectations.expect(sampling.definitions.find("texelFetch(") != std::string::npos &&
                            sampling.definitions.find("mix(") != std::string::npos,
                        "the linear sampler keeps interpolation while the nearest sampler fetches");

    // A texture whose sampler cannot be named safely is left entirely on hardware sampling.
    auto unnameable = program;
    unnameable.textures[1].samplerName = "not a valid identifier";
    expectations.expect(
        !ocioGpuSamplingGlslFor(unnameable).dispatches(),
        "an unnameable sampler refuses the whole adapter rather than half-applying");
    auto duplicated = program;
    duplicated.textures[1].samplerName = duplicated.textures[0].samplerName;
    expectations.expect(!ocioGpuSamplingGlslFor(duplicated).dispatches(),
                        "a duplicated sampler name refuses the whole adapter");
}

} // namespace bloom::color::gpu_program_test
