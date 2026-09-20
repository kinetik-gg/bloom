#include "ocio_gpu_program_serialize.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace bloom::color::detail {
namespace {

constexpr std::uint64_t kMaximumTextBytes = std::uint64_t{4} * 1024U * 1024U;

using Bytes = std::vector<std::byte>;

void appendU32(Bytes& out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void appendU64(Bytes& out, const std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void appendFloat(Bytes& out, const float value) {
    appendU32(out, std::bit_cast<std::uint32_t>(value));
}

void appendText(Bytes& out, const std::string_view text) {
    appendU32(out, static_cast<std::uint32_t>(text.size()));
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

class Reader final {
  public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u32(std::uint32_t& value) {
        if (remaining() < 4) {
            return false;
        }
        value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        }
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) {
        if (remaining() < 8) {
            return false;
        }
        value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        }
        return true;
    }
    [[nodiscard]] bool f32(float& value) {
        std::uint32_t bits = 0;
        if (!u32(bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }
    [[nodiscard]] bool text(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size) || size > kMaximumTextBytes || remaining() < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool floats(std::vector<float>& values, const std::uint32_t count) {
        if (static_cast<std::uint64_t>(count) * sizeof(float) > remaining()) {
            return false;
        }
        values.resize(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!f32(values[index])) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool bytes(std::vector<std::byte>& out, const std::uint32_t count) {
        if (remaining() < count) {
            return false;
        }
        out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] std::size_t remainingBytes() const noexcept { return remaining(); }
    [[nodiscard]] bool rawText(std::string& value, const std::uint32_t count) {
        if (remaining() < count) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), count);
        offset_ += count;
        return true;
    }

  private:
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

[[nodiscard]] bool fitsU32(const std::size_t value) noexcept {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

std::optional<std::vector<std::byte>>
serializeOcioGpuProgram(const render::OcioGpuProgramDesc& program, const std::uint64_t maxBytes) {
    if (!fitsU32(program.shaderText.size()) || !fitsU32(program.functionName.size()) ||
        !fitsU32(program.resourcePrefix.size()) || !fitsU32(program.semanticsId.size()) ||
        !fitsU32(program.processorCacheId.size()) || !fitsU32(program.gpuProcessorCacheId.size()) ||
        !fitsU32(program.ocioVersion.size()) || !fitsU32(program.uniformBufferData.size()) ||
        program.textures.size() > std::numeric_limits<std::uint32_t>::max() ||
        program.uniforms.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    Bytes out;
    appendU32(out, kOcioGpuProgramSerializationMagic);
    appendU32(out, kOcioGpuProgramSerializationVersion);
    appendU32(out, static_cast<std::uint32_t>(program.stage));
    appendU32(out, program.descriptorSetIndex);
    appendU32(out, program.textureBindingStart);
    appendU32(out, program.dynamicPropertyCount);
    appendU64(out, program.uniformBufferSize);
    appendText(out, program.functionName);
    appendText(out, program.resourcePrefix);
    appendText(out, program.semanticsId);
    appendText(out, program.processorCacheId);
    appendText(out, program.gpuProcessorCacheId);
    appendText(out, program.ocioVersion);
    appendU32(out, static_cast<std::uint32_t>(program.textures.size()));
    for (const auto& texture : program.textures) {
        if (!fitsU32(texture.name.size()) || !fitsU32(texture.samplerName.size()) ||
            !fitsU32(texture.samples.size())) {
            return std::nullopt;
        }
        appendU32(out, texture.binding);
        appendU32(out, static_cast<std::uint32_t>(texture.dimensions));
        appendU32(out, static_cast<std::uint32_t>(texture.channel));
        appendU32(out, texture.width);
        appendU32(out, texture.height);
        appendU32(out, texture.edgeLength);
        appendU32(out, static_cast<std::uint32_t>(texture.interpolation));
        appendText(out, texture.name);
        appendText(out, texture.samplerName);
        appendU32(out, static_cast<std::uint32_t>(texture.samples.size()));
        for (const float sample : texture.samples) {
            appendFloat(out, sample);
        }
    }
    appendU32(out, static_cast<std::uint32_t>(program.uniforms.size()));
    for (const auto& uniform : program.uniforms) {
        if (!fitsU32(uniform.name.size())) {
            return std::nullopt;
        }
        appendU32(out, static_cast<std::uint32_t>(uniform.type));
        appendU32(out, uniform.bufferOffset);
        appendU32(out, uniform.elementCount);
        appendText(out, uniform.name);
    }
    appendU32(out, static_cast<std::uint32_t>(program.uniformBufferData.size()));
    out.insert(out.end(), program.uniformBufferData.begin(), program.uniformBufferData.end());
    // Shader text last so its (potentially large) body is length-prefixed like every other field.
    appendU32(out, static_cast<std::uint32_t>(program.shaderText.size()));
    {
        const auto shader =
            std::as_bytes(std::span(program.shaderText.data(), program.shaderText.size()));
        out.insert(out.end(), shader.begin(), shader.end());
    }
    if (out.size() > maxBytes) {
        return std::nullopt;
    }
    return out;
}

OcioGpuProgramTransport
deserializeOcioGpuProgram(const std::span<const std::byte> bytes,
                          const render::OcioGpuProgramLimits& limits) noexcept {
    using render::OcioGpuProgramError;
    const auto fail = [](const OcioGpuProgramError error) {
        OcioGpuProgramTransport result;
        result.error = error;
        return result;
    };
    try {
        if (bytes.size() < 8) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        Reader reader(bytes);
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t stage = 0;
        std::uint32_t descriptorSetIndex = 0;
        std::uint32_t textureBindingStart = 0;
        std::uint32_t dynamicPropertyCount = 0;
        std::uint64_t uniformBufferSize = 0;
        if (!reader.u32(magic) || magic != kOcioGpuProgramSerializationMagic ||
            !reader.u32(version) || version != kOcioGpuProgramSerializationVersion ||
            !reader.u32(stage) ||
            stage > static_cast<std::uint32_t>(render::OcioGpuProgramStage::DisplayPacking) ||
            !reader.u32(descriptorSetIndex) || !reader.u32(textureBindingStart) ||
            !reader.u32(dynamicPropertyCount) || !reader.u64(uniformBufferSize)) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        if (dynamicPropertyCount > limits.maxDynamicPropertyCount ||
            uniformBufferSize > limits.maxUniformBufferBytes) {
            return fail(OcioGpuProgramError::ResourceLimitExceeded);
        }
        render::OcioGpuProgramDesc program;
        program.stage = static_cast<render::OcioGpuProgramStage>(stage);
        program.descriptorSetIndex = descriptorSetIndex;
        program.textureBindingStart = textureBindingStart;
        program.dynamicPropertyCount = dynamicPropertyCount;
        program.uniformBufferSize = uniformBufferSize;
        if (!reader.text(program.functionName) || !reader.text(program.resourcePrefix) ||
            !reader.text(program.semanticsId) || !reader.text(program.processorCacheId) ||
            !reader.text(program.gpuProcessorCacheId) || !reader.text(program.ocioVersion)) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        std::uint32_t textureCount = 0;
        if (!reader.u32(textureCount) || textureCount > limits.maxTextures) {
            return fail(textureCount > limits.maxTextures
                            ? OcioGpuProgramError::ResourceLimitExceeded
                            : OcioGpuProgramError::UnsupportedResourceForm);
        }
        program.textures.resize(textureCount);
        for (auto& texture : program.textures) {
            std::uint32_t dimensions = 0;
            std::uint32_t channel = 0;
            std::uint32_t interpolation = 0;
            std::uint32_t sampleCount = 0;
            if (!reader.u32(texture.binding) || !reader.u32(dimensions) || dimensions < 1 ||
                dimensions > 3 || !reader.u32(channel) || channel > 1 ||
                !reader.u32(texture.width) || !reader.u32(texture.height) ||
                !reader.u32(texture.edgeLength) || !reader.u32(interpolation) ||
                interpolation > static_cast<std::uint32_t>(render::OcioGpuInterpolation::Cubic) ||
                !reader.text(texture.name) || !reader.text(texture.samplerName) ||
                !reader.u32(sampleCount) ||
                static_cast<std::uint64_t>(sampleCount) * sizeof(float) >
                    limits.maxAggregateLutBytes) {
                return fail(OcioGpuProgramError::UnsupportedResourceForm);
            }
            texture.dimensions = static_cast<render::OcioGpuTextureDimensions>(dimensions);
            texture.channel = static_cast<render::OcioGpuTextureChannel>(channel);
            texture.interpolation = static_cast<render::OcioGpuInterpolation>(interpolation);
            if (!reader.floats(texture.samples, sampleCount)) {
                return fail(OcioGpuProgramError::UnsupportedResourceForm);
            }
        }
        std::uint32_t uniformCount = 0;
        if (!reader.u32(uniformCount) || uniformCount > limits.maxUniforms) {
            return fail(uniformCount > limits.maxUniforms
                            ? OcioGpuProgramError::ResourceLimitExceeded
                            : OcioGpuProgramError::UnsupportedResourceForm);
        }
        program.uniforms.resize(uniformCount);
        for (auto& uniform : program.uniforms) {
            std::uint32_t type = 0;
            if (!reader.u32(type) ||
                type > static_cast<std::uint32_t>(render::OcioGpuUniformType::Unknown) ||
                !reader.u32(uniform.bufferOffset) || !reader.u32(uniform.elementCount) ||
                !reader.text(uniform.name)) {
                return fail(OcioGpuProgramError::UnsupportedResourceForm);
            }
            uniform.type = static_cast<render::OcioGpuUniformType>(type);
        }
        std::uint32_t uniformBytes = 0;
        if (!reader.u32(uniformBytes) || uniformBytes != uniformBufferSize ||
            !reader.bytes(program.uniformBufferData, uniformBytes)) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        std::uint32_t shaderBytes = 0;
        if (!reader.u32(shaderBytes)) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        if (shaderBytes > limits.maxShaderBytes) {
            return fail(OcioGpuProgramError::ResourceLimitExceeded);
        }
        if (!reader.rawText(program.shaderText, shaderBytes) || !reader.done()) {
            return fail(OcioGpuProgramError::UnsupportedResourceForm);
        }
        OcioGpuProgramTransport result;
        result.program = std::move(program);
        return result;
    } catch (const std::bad_alloc&) {
        return fail(OcioGpuProgramError::ResourceLimitExceeded);
    } catch (const std::exception&) {
        return fail(OcioGpuProgramError::UnsupportedResourceForm);
    }
}

} // namespace bloom::color::detail
