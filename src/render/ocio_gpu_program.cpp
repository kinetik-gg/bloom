#include <bloom/render/ocio_gpu_program.hpp>

#include <cmath>
#include <cstddef>
#include <unordered_set>

namespace bloom::render {
namespace {

void feedBytes(core::Sha256Hasher& hasher, const std::span<const std::byte> bytes) noexcept {
    static_cast<void>(hasher.update(bytes));
}

void feedU32(core::Sha256Hasher& hasher, const std::uint32_t value) noexcept {
    const std::array<std::byte, 4> bytes{static_cast<std::byte>((value >> 24U) & 0xffU),
                                         static_cast<std::byte>((value >> 16U) & 0xffU),
                                         static_cast<std::byte>((value >> 8U) & 0xffU),
                                         static_cast<std::byte>(value & 0xffU)};
    feedBytes(hasher, bytes);
}

void feedU64(core::Sha256Hasher& hasher, const std::uint64_t value) noexcept {
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((value >> (56U - (8U * i))) & 0xffU);
    }
    feedBytes(hasher, bytes);
}

void feedText(core::Sha256Hasher& hasher, const std::string_view text) noexcept {
    feedU32(hasher, static_cast<std::uint32_t>(text.size()));
    feedBytes(hasher, std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] core::Sha256Digest finalize(const core::Sha256Hasher& hasher) noexcept {
    return hasher.finalize();
}

[[nodiscard]] std::uint64_t channelCount(const OcioGpuTextureChannel channel) noexcept {
    return channel == OcioGpuTextureChannel::Red ? 1U : 3U;
}

[[nodiscard]] std::uint64_t declaredSampleCount(const OcioGpuTextureDesc& texture) noexcept {
    switch (texture.dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return static_cast<std::uint64_t>(texture.width) * channelCount(texture.channel);
    case OcioGpuTextureDimensions::TwoD:
        return static_cast<std::uint64_t>(texture.width) * texture.height *
               channelCount(texture.channel);
    case OcioGpuTextureDimensions::ThreeD:
        return static_cast<std::uint64_t>(texture.edgeLength) * texture.edgeLength *
               texture.edgeLength * 3U;
    }
    return 0;
}

} // namespace

std::string_view ocioGpuProgramErrorName(const OcioGpuProgramError error) noexcept {
    switch (error) {
    case OcioGpuProgramError::None:
        return "none";
    case OcioGpuProgramError::InvalidRequest:
        return "invalid-request";
    case OcioGpuProgramError::UnsupportedColorSpace:
        return "unsupported-color-space";
    case OcioGpuProgramError::IdentityTransform:
        return "identity-transform";
    case OcioGpuProgramError::TransformBuildFailed:
        return "transform-build-failed";
    case OcioGpuProgramError::GpuProcessorUnavailable:
        return "gpu-processor-unavailable";
    case OcioGpuProgramError::ShaderExtractionFailed:
        return "shader-extraction-failed";
    case OcioGpuProgramError::IdentityShaderText:
        return "identity-shader-text";
    case OcioGpuProgramError::ResourceLimitExceeded:
        return "resource-limit-exceeded";
    case OcioGpuProgramError::UnsupportedResourceForm:
        return "unsupported-resource-form";
    case OcioGpuProgramError::ExternalLutBoundaryRequired:
        return "external-lut-boundary-required";
    }
    return "unknown";
}

std::string_view ocioGpuInterpolationName(const OcioGpuInterpolation value) noexcept {
    switch (value) {
    case OcioGpuInterpolation::Unknown:
        return "unknown";
    case OcioGpuInterpolation::Nearest:
        return "nearest";
    case OcioGpuInterpolation::Linear:
        return "linear";
    case OcioGpuInterpolation::Tetrahedral:
        return "tetrahedral";
    case OcioGpuInterpolation::Cubic:
        return "cubic";
    }
    return "unknown";
}

OcioGpuProgramResult::OcioGpuProgramResult(std::optional<OcioGpuProgramDesc> program,
                                           const OcioGpuProgramError error) noexcept
    : program_(std::move(program)), error_(error) {}

OcioGpuProgramResult OcioGpuProgramResult::success(OcioGpuProgramDesc program) {
    return OcioGpuProgramResult(std::move(program), OcioGpuProgramError::None);
}

OcioGpuProgramResult OcioGpuProgramResult::failure(const OcioGpuProgramError error) noexcept {
    return OcioGpuProgramResult({}, error);
}

core::Sha256Digest computeOcioGpuShaderTextDigest(const std::string_view shaderText) noexcept {
    static constexpr char kDomain[] = "BloomOcioGpuShader";
    core::Sha256Hasher hasher;
    feedBytes(hasher, std::as_bytes(std::span(kDomain, sizeof(kDomain) - 1)));
    const std::array<std::byte, 2> version{std::byte{0}, std::byte{1}};
    feedBytes(hasher, version);
    feedBytes(hasher, std::as_bytes(std::span(shaderText.data(), shaderText.size())));
    return finalize(hasher);
}

core::Sha256Digest computeOcioGpuResourceDigest(const OcioGpuProgramDesc& program) noexcept {
    static constexpr char kDomain[] = "BloomOcioGpuResources";
    core::Sha256Hasher hasher;
    feedBytes(hasher, std::as_bytes(std::span(kDomain, sizeof(kDomain) - 1)));
    const std::array<std::byte, 2> version{std::byte{0}, std::byte{1}};
    feedBytes(hasher, version);
    feedU32(hasher, program.descriptorSetIndex);
    feedU32(hasher, program.textureBindingStart);
    feedU32(hasher, static_cast<std::uint32_t>(program.textures.size()));
    for (const auto& texture : program.textures) {
        feedU32(hasher, texture.binding);
        feedText(hasher, texture.name);
        feedText(hasher, texture.samplerName);
        feedU32(hasher, static_cast<std::uint32_t>(texture.dimensions));
        feedU32(hasher, static_cast<std::uint32_t>(texture.channel));
        feedU32(hasher, texture.width);
        feedU32(hasher, texture.height);
        feedU32(hasher, texture.edgeLength);
        feedU32(hasher, static_cast<std::uint32_t>(texture.interpolation));
        feedU32(hasher, static_cast<std::uint32_t>(texture.samples.size()));
        feedBytes(hasher, std::as_bytes(std::span(texture.samples.data(), texture.samples.size())));
    }
    feedU32(hasher, static_cast<std::uint32_t>(program.uniforms.size()));
    for (const auto& uniform : program.uniforms) {
        feedText(hasher, uniform.name);
        feedU32(hasher, static_cast<std::uint32_t>(uniform.type));
        feedU32(hasher, uniform.bufferOffset);
        feedU32(hasher, uniform.elementCount);
    }
    feedU32(hasher, program.dynamicPropertyCount);
    feedU64(hasher, program.uniformBufferSize);
    return finalize(hasher);
}

OcioGpuProgramError validateOcioGpuProgram(const OcioGpuProgramDesc& program,
                                           const OcioGpuProgramLimits& limits) noexcept {
    if (program.shaderText.empty() || program.functionName.empty() || program.semanticsId.empty()) {
        return OcioGpuProgramError::ShaderExtractionFailed;
    }
    if (program.shaderText.size() > limits.maxShaderBytes) {
        return OcioGpuProgramError::ResourceLimitExceeded;
    }
    if (program.textures.size() > limits.maxTextures ||
        program.uniforms.size() > limits.maxUniforms ||
        program.dynamicPropertyCount > limits.maxDynamicPropertyCount ||
        program.uniformBufferSize > limits.maxUniformBufferBytes) {
        return OcioGpuProgramError::ResourceLimitExceeded;
    }
    std::unordered_set<std::string> names;
    std::uint64_t aggregateBytes = 0;
    for (const auto& texture : program.textures) {
        if (texture.name.empty() || !names.insert(texture.name).second) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (texture.binding < program.textureBindingStart) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (texture.interpolation == OcioGpuInterpolation::Cubic) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (texture.dimensions == OcioGpuTextureDimensions::ThreeD &&
            (texture.edgeLength == 0 || texture.edgeLength > limits.max3dEdge)) {
            return OcioGpuProgramError::ResourceLimitExceeded;
        }
        if (texture.samples.size() != declaredSampleCount(texture)) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        aggregateBytes += texture.samples.size() * sizeof(float);
        for (const float sample : texture.samples) {
            if (!std::isfinite(sample)) {
                return OcioGpuProgramError::UnsupportedResourceForm;
            }
        }
    }
    if (aggregateBytes > limits.maxAggregateLutBytes) {
        return OcioGpuProgramError::ResourceLimitExceeded;
    }
    names.clear();
    for (const auto& uniform : program.uniforms) {
        if (uniform.name.empty() || !names.insert(uniform.name).second) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (uniform.elementCount == 0) {
            return OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (program.uniformBufferSize != 0) {
            const std::uint64_t entryBytes =
                (uniform.type == OcioGpuUniformType::VectorFloat ||
                 uniform.type == OcioGpuUniformType::VectorInt)
                    ? static_cast<std::uint64_t>(uniform.elementCount) * sizeof(float)
                    : (uniform.type == OcioGpuUniformType::Float3 ? 3U * sizeof(float)
                                                                  : sizeof(float));
            if (static_cast<std::uint64_t>(uniform.bufferOffset) + entryBytes >
                program.uniformBufferSize) {
                return OcioGpuProgramError::UnsupportedResourceForm;
            }
        }
    }
    return OcioGpuProgramError::None;
}

core::Sha256Digest
computeOcioGpuContentIdentity(const OcioGpuContentIdentityParts& parts) noexcept {
    static constexpr char kDomain[] = "BloomOcioGpuProgram";
    core::Sha256Hasher hasher;
    feedBytes(hasher, std::as_bytes(std::span(kDomain, sizeof(kDomain) - 1)));
    const std::array<std::byte, 2> version{std::byte{0}, std::byte{1}};
    feedBytes(hasher, version);
    feedText(hasher, parts.semanticsId);
    feedBytes(hasher, parts.semanticIdentityBytes);
    feedBytes(hasher, std::as_bytes(std::span(parts.ocioConfigRevision.bytes())));
    feedBytes(hasher, std::as_bytes(std::span(parts.shaderTextDigest.bytes())));
    feedBytes(hasher, std::as_bytes(std::span(parts.resourceDigest.bytes())));
    return finalize(hasher);
}

} // namespace bloom::render
