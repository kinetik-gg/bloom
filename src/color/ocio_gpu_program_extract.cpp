#include "ocio_gpu_program_extract.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace bloom::color::detail {
namespace {

[[nodiscard]] render::OcioGpuInterpolation
mapInterpolation(const OCIO::Interpolation value) noexcept {
    switch (value) {
    case OCIO::INTERP_NEAREST:
        return render::OcioGpuInterpolation::Nearest;
    case OCIO::INTERP_LINEAR:
        return render::OcioGpuInterpolation::Linear;
    case OCIO::INTERP_TETRAHEDRAL:
        return render::OcioGpuInterpolation::Tetrahedral;
    case OCIO::INTERP_CUBIC:
        return render::OcioGpuInterpolation::Cubic;
    default:
        return render::OcioGpuInterpolation::Unknown;
    }
}

[[nodiscard]] render::OcioGpuUniformType
mapUniformType(const OCIO::UniformDataType value) noexcept {
    switch (value) {
    case OCIO::UNIFORM_DOUBLE:
        return render::OcioGpuUniformType::Double;
    case OCIO::UNIFORM_BOOL:
        return render::OcioGpuUniformType::Bool;
    case OCIO::UNIFORM_FLOAT3:
        return render::OcioGpuUniformType::Float3;
    case OCIO::UNIFORM_VECTOR_FLOAT:
        return render::OcioGpuUniformType::VectorFloat;
    case OCIO::UNIFORM_VECTOR_INT:
        return render::OcioGpuUniformType::VectorInt;
    default:
        return render::OcioGpuUniformType::Unknown;
    }
}

[[nodiscard]] render::OcioGpuProgramError
fillResources(const OCIO::GpuShaderDescRcPtr& description,
              const render::OcioGpuProgramLimits& limits,
              render::OcioGpuProgramDesc& program) noexcept {
    program.descriptorSetIndex = description->getDescriptorSetIndex();
    program.textureBindingStart = description->getTextureBindingStart();

    const unsigned textureCount = description->getNumTextures();
    const unsigned texture3dCount = description->getNum3DTextures();
    if (textureCount + texture3dCount > limits.maxTextures) {
        return render::OcioGpuProgramError::ResourceLimitExceeded;
    }
    std::uint64_t aggregateBytes = 0;
    for (unsigned index = 0; index < textureCount; ++index) {
        const char* textureName = nullptr;
        const char* samplerName = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        OCIO::GpuShaderCreator::TextureType channel = OCIO::GpuShaderCreator::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderCreator::TextureDimensions dimensions = OCIO::GpuShaderCreator::TEXTURE_1D;
        OCIO::Interpolation interpolation = OCIO::INTERP_UNKNOWN;
        description->getTexture(index, textureName, samplerName, width, height, channel, dimensions,
                                interpolation);
        const float* values = nullptr;
        description->getTextureValues(index, values);
        if (textureName == nullptr || values == nullptr || width == 0 || height == 0) {
            return render::OcioGpuProgramError::UnsupportedResourceForm;
        }
        render::OcioGpuTextureDesc texture;
        texture.binding = description->getTextureShaderBindingIndex(index);
        texture.name = textureName;
        texture.samplerName = samplerName != nullptr ? samplerName : "";
        texture.dimensions = dimensions == OCIO::GpuShaderCreator::TEXTURE_1D
                                 ? render::OcioGpuTextureDimensions::OneD
                                 : render::OcioGpuTextureDimensions::TwoD;
        texture.channel = channel == OCIO::GpuShaderCreator::TEXTURE_RED_CHANNEL
                              ? render::OcioGpuTextureChannel::Red
                              : render::OcioGpuTextureChannel::Rgb;
        texture.width = width;
        texture.height = height;
        texture.interpolation = mapInterpolation(interpolation);
        const std::uint64_t channels =
            texture.channel == render::OcioGpuTextureChannel::Red ? 1U : 3U;
        const std::uint64_t sampleCount = static_cast<std::uint64_t>(width) * height * channels;
        aggregateBytes += sampleCount * sizeof(float);
        if (aggregateBytes > limits.maxAggregateLutBytes) {
            return render::OcioGpuProgramError::ResourceLimitExceeded;
        }
        texture.samples.assign(values, values + sampleCount);
        program.textures.push_back(std::move(texture));
    }
    for (unsigned index = 0; index < texture3dCount; ++index) {
        const char* textureName = nullptr;
        const char* samplerName = nullptr;
        unsigned edgeLength = 0;
        OCIO::Interpolation interpolation = OCIO::INTERP_UNKNOWN;
        description->get3DTexture(index, textureName, samplerName, edgeLength, interpolation);
        const float* values = nullptr;
        description->get3DTextureValues(index, values);
        if (textureName == nullptr || values == nullptr || edgeLength == 0) {
            return render::OcioGpuProgramError::UnsupportedResourceForm;
        }
        if (edgeLength > limits.max3dEdge) {
            return render::OcioGpuProgramError::ResourceLimitExceeded;
        }
        const std::uint64_t sampleCount =
            static_cast<std::uint64_t>(edgeLength) * edgeLength * edgeLength * 3U;
        aggregateBytes += sampleCount * sizeof(float);
        if (aggregateBytes > limits.maxAggregateLutBytes) {
            return render::OcioGpuProgramError::ResourceLimitExceeded;
        }
        render::OcioGpuTextureDesc texture;
        texture.binding = description->get3DTextureShaderBindingIndex(index);
        texture.name = textureName;
        texture.samplerName = samplerName != nullptr ? samplerName : "";
        texture.dimensions = render::OcioGpuTextureDimensions::ThreeD;
        texture.channel = render::OcioGpuTextureChannel::Rgb;
        texture.edgeLength = edgeLength;
        texture.interpolation = mapInterpolation(interpolation);
        texture.samples.assign(values, values + sampleCount);
        program.textures.push_back(std::move(texture));
    }

    const unsigned uniformCount = description->getNumUniforms();
    if (uniformCount > limits.maxUniforms) {
        return render::OcioGpuProgramError::ResourceLimitExceeded;
    }
    program.uniformBufferSize = description->getUniformBufferSize();
    if (program.uniformBufferSize > limits.maxUniformBufferBytes) {
        return render::OcioGpuProgramError::ResourceLimitExceeded;
    }
    program.uniformBufferData.assign(static_cast<std::size_t>(program.uniformBufferSize),
                                     std::byte{0});
    const auto storeFloat = [&program](const std::size_t offset, const float value) {
        if (offset + sizeof(float) > program.uniformBufferData.size()) {
            return false;
        }
        std::memcpy(program.uniformBufferData.data() + offset, &value, sizeof(float));
        return true;
    };
    for (unsigned index = 0; index < uniformCount; ++index) {
        OCIO::GpuShaderDesc::UniformData data;
        const char* name = description->getUniform(index, data);
        if (name == nullptr) {
            return render::OcioGpuProgramError::UnsupportedResourceForm;
        }
        render::OcioGpuUniformDesc uniform;
        uniform.name = name;
        uniform.type = mapUniformType(data.m_type);
        uniform.bufferOffset = static_cast<std::uint32_t>(data.m_bufferOffset);
        const std::size_t offset = data.m_bufferOffset;
        switch (uniform.type) {
        case render::OcioGpuUniformType::Double:
            if (!storeFloat(offset, static_cast<float>(data.m_getDouble()))) {
                return render::OcioGpuProgramError::UnsupportedResourceForm;
            }
            break;
        case render::OcioGpuUniformType::Bool:
            if (!storeFloat(offset, data.m_getBool() ? 1.0F : 0.0F)) {
                return render::OcioGpuProgramError::UnsupportedResourceForm;
            }
            break;
        case render::OcioGpuUniformType::Float3: {
            const OCIO::Float3& value = data.m_getFloat3();
            if (!storeFloat(offset, value[0]) || !storeFloat(offset + 4, value[1]) ||
                !storeFloat(offset + 8, value[2])) {
                return render::OcioGpuProgramError::UnsupportedResourceForm;
            }
            uniform.elementCount = 3;
            break;
        }
        case render::OcioGpuUniformType::VectorFloat: {
            uniform.elementCount = static_cast<std::uint32_t>(data.m_vectorFloat.m_getSize());
            const float* values = data.m_vectorFloat.m_getVector();
            if (values == nullptr) {
                return render::OcioGpuProgramError::UnsupportedResourceForm;
            }
            for (std::uint32_t element = 0; element < uniform.elementCount; ++element) {
                if (!storeFloat(offset + element * sizeof(float), values[element])) {
                    return render::OcioGpuProgramError::UnsupportedResourceForm;
                }
            }
            break;
        }
        case render::OcioGpuUniformType::VectorInt: {
            uniform.elementCount = static_cast<std::uint32_t>(data.m_vectorInt.m_getSize());
            const int* values = data.m_vectorInt.m_getVector();
            if (values == nullptr) {
                return render::OcioGpuProgramError::UnsupportedResourceForm;
            }
            for (std::uint32_t element = 0; element < uniform.elementCount; ++element) {
                if (!storeFloat(offset + element * sizeof(float),
                                static_cast<float>(values[element]))) {
                    return render::OcioGpuProgramError::UnsupportedResourceForm;
                }
            }
            break;
        }
        default:
            return render::OcioGpuProgramError::UnsupportedResourceForm;
        }
        program.uniforms.push_back(std::move(uniform));
    }
    program.dynamicPropertyCount = description->getNumDynamicProperties();
    return render::OcioGpuProgramError::None;
}

} // namespace

OcioGpuProgramTransport extractOcioGpuProgram(const OCIO::ConstProcessorRcPtr& processor,
                                              const render::OcioGpuProgramStage stage,
                                              const std::string_view semanticsId,
                                              const render::OcioGpuProgramLimits& limits) noexcept {
    OcioGpuProgramTransport result;
    try {
        const auto gpuProcessor = processor->getDefaultGPUProcessor();
        if (!gpuProcessor) {
            result.error = render::OcioGpuProgramError::GpuProcessorUnavailable;
            return result;
        }
        auto description = OCIO::GpuShaderDesc::CreateShaderDesc();
        description->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
        description->setFunctionName("bloom_ocio_transform");
        description->setResourcePrefix("bloom_ocio_");
        gpuProcessor->extractGpuShaderInfo(description);

        render::OcioGpuProgramDesc program;
        program.functionName =
            description->getFunctionName() != nullptr ? description->getFunctionName() : "";
        program.resourcePrefix =
            description->getResourcePrefix() != nullptr ? description->getResourcePrefix() : "";
        program.stage = stage;
        program.semanticsId = std::string(semanticsId);
        const char* const shaderText = description->getShaderText();
        if (shaderText == nullptr || *shaderText == '\0') {
            result.error = render::OcioGpuProgramError::IdentityShaderText;
            return result;
        }
        program.shaderText = shaderText;
        if (program.shaderText.size() > limits.maxShaderBytes) {
            result.error = render::OcioGpuProgramError::ResourceLimitExceeded;
            return result;
        }
        if (const auto error = fillResources(description, limits, program);
            error != render::OcioGpuProgramError::None) {
            result.error = error;
            return result;
        }
        program.processorCacheId = processor->getCacheID();
        program.gpuProcessorCacheId = gpuProcessor->getCacheID();
        program.ocioVersion = OCIO::GetVersion();
        result.program = std::move(program);
        return result;
    } catch (const OCIO::Exception&) {
        result.error = render::OcioGpuProgramError::ShaderExtractionFailed;
        return result;
    } catch (const std::exception&) {
        result.error = render::OcioGpuProgramError::ShaderExtractionFailed;
        return result;
    }
}

} // namespace bloom::color::detail
