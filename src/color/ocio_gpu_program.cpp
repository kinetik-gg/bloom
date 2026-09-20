#include <bloom/color/ocio_gpu_program.hpp>

#include "ocio_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::color {
namespace {

[[nodiscard]] bool validId(const std::string_view id) noexcept {
    return !id.empty() && id.size() <= 4096U && id.find('\0') == std::string_view::npos;
}

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

void appendText(std::vector<std::byte>& bytes, const std::string_view text) {
    const auto size = static_cast<std::uint32_t>(text.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((size >> shift) & 0xffU));
    }
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
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

[[nodiscard]] render::OcioGpuProgramResult finalize(render::OcioGpuProgramDesc program,
                                                    std::vector<std::byte> semanticBytes,
                                                    const core::Sha256Digest& configRevision,
                                                    const render::OcioGpuProgramLimits& limits) {
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    const render::OcioGpuContentIdentityParts parts{
        .semanticsId = program.semanticsId,
        .semanticIdentityBytes = semanticBytes,
        .ocioConfigRevision = configRevision,
        .shaderTextDigest = program.shaderTextDigest,
        .resourceDigest = program.resourceDigest,
    };
    program.contentIdentity = render::computeOcioGpuContentIdentity(parts);
    if (const auto error = render::validateOcioGpuProgram(program, limits);
        error != render::OcioGpuProgramError::None) {
        return render::OcioGpuProgramResult::failure(error);
    }
    return render::OcioGpuProgramResult::success(std::move(program));
}

[[nodiscard]] render::OcioGpuProgramResult
extract(const OCIO::ConstProcessorRcPtr& processor, const render::OcioGpuProgramStage stage,
        const std::string_view semanticsId, std::vector<std::byte> semanticBytes,
        const core::Sha256Digest& configRevision, const render::OcioGpuProgramLimits& limits) {
    try {
        const auto gpuProcessor = processor->getDefaultGPUProcessor();
        if (!gpuProcessor) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::GpuProcessorUnavailable);
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
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::IdentityShaderText);
        }
        program.shaderText = shaderText;
        if (program.shaderText.size() > limits.maxShaderBytes) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::ResourceLimitExceeded);
        }
        if (const auto error = fillResources(description, limits, program);
            error != render::OcioGpuProgramError::None) {
            return render::OcioGpuProgramResult::failure(error);
        }
        program.processorCacheId = processor->getCacheID();
        program.gpuProcessorCacheId = gpuProcessor->getCacheID();
        program.ocioVersion = OCIO::GetVersion();
        return finalize(std::move(program), std::move(semanticBytes), configRevision, limits);
    } catch (const OCIO::Exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::ShaderExtractionFailed);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::ShaderExtractionFailed);
    }
}

} // namespace

render::OcioGpuProgramResult
buildOcioGpuProgramForDisplay(const ResolvedBloomNeutralConfig& resolved,
                              const std::string_view display, const std::string_view view,
                              const render::OcioGpuProgramLimits& limits) noexcept {
    if (!validId(display) || !validId(view)) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    const auto entry = std::find_if(
        resolved.displays().begin(), resolved.displays().end(), [&](const auto& candidate) {
            return candidate.display == display && candidate.view == view;
        });
    if (entry == resolved.displays().end()) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    const auto& config = resolved.impl().config();
    try {
        const OCIO::ConstContextRcPtr emptyContext = OCIO::Context::Create();
        auto transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(std::string(resolved.processColorSpaceId()).c_str());
        transform->setDisplay(std::string(display).c_str());
        transform->setView(std::string(view).c_str());
        const auto processor =
            config->getProcessor(emptyContext, transform, OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::TransformBuildFailed);
        }
        std::vector<std::byte> semanticBytes;
        appendText(semanticBytes, resolved.processColorSpaceId());
        appendText(semanticBytes, display);
        appendText(semanticBytes, view);
        appendText(semanticBytes, entry->colourSpaceId);
        return extract(processor, render::OcioGpuProgramStage::DisplayPacking,
                       kOcioGpuDisplaySemanticsId, std::move(semanticBytes),
                       resolved.expectedRevision(), limits);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::TransformBuildFailed);
    }
}

render::OcioGpuProgramResult
buildOcioGpuProgramForCst(const ResolvedBloomNeutralConfig& resolved, const std::string_view fromId,
                          const std::string_view toId,
                          const render::OcioGpuProgramLimits& limits) noexcept {
    if (!validId(fromId) || !validId(toId)) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    const auto& config = resolved.impl().config();
    try {
        const auto source = config->getColorSpace(std::string(fromId).c_str());
        const auto destination = config->getColorSpace(std::string(toId).c_str());
        if (!source || !destination || source->isData() || destination->isData()) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::UnsupportedColorSpace);
        }
        if (fromId == toId) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::IdentityTransform);
        }
        const OCIO::ConstContextRcPtr emptyContext = OCIO::Context::Create();
        const auto processor = config->getProcessor(emptyContext, std::string(fromId).c_str(),
                                                    std::string(toId).c_str());
        if (!processor) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::TransformBuildFailed);
        }
        std::vector<std::byte> semanticBytes;
        appendText(semanticBytes, fromId);
        appendText(semanticBytes, toId);
        return extract(processor, render::OcioGpuProgramStage::ProcessEffect,
                       kOcioGpuCstSemanticsId, std::move(semanticBytes),
                       resolved.expectedRevision(), limits);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::TransformBuildFailed);
    }
}

render::OcioGpuProgramResult buildOcioGpuProgramForFileTransform(
    const ResolvedBloomNeutralConfig& resolved, const std::string_view processSpaceId,
    const core::Sha256Digest& sourceLutDigest, const std::uint32_t lutFormat,
    const LutInterpolation interpolation, const LutDirection direction,
    const std::string_view workingSpaceId, const render::OcioGpuProgramLimits& limits) noexcept {
    static_cast<void>(resolved);
    static_cast<void>(limits);
    // The boundary is explicit: validate the request, then refuse in-process extraction. Arbitrary
    // LUT bytes are parsed only by the isolated helper; no OCIO FileTransform is built here.
    const bool validRequest =
        validId(processSpaceId) && validId(workingSpaceId) && lutFormat >= 1 && lutFormat <= 4 &&
        interpolation <= LutInterpolation::Best && direction <= LutDirection::Inverse &&
        sourceLutDigest != core::Sha256Digest{};
    if (!validRequest) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    (void)kOcioGpuFileTransformSemanticsId;
    return render::OcioGpuProgramResult::failure(
        render::OcioGpuProgramError::ExternalLutBoundaryRequired);
}

render::OcioGpuProgramResult
buildOcioGpuProgramForExposureContrast(const ResolvedBloomNeutralConfig& resolved,
                                       const std::string_view sourceId, const double exposure,
                                       const double contrast,
                                       const render::OcioGpuProgramLimits& limits) noexcept {
    if (!validId(sourceId) || !std::isfinite(exposure) || !std::isfinite(contrast)) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    const auto& config = resolved.impl().config();
    try {
        if (!config->getColorSpace(std::string(sourceId).c_str())) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::UnsupportedColorSpace);
        }
        auto transform = OCIO::ExposureContrastTransform::Create();
        transform->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
        transform->setExposure(exposure);
        transform->setContrast(contrast);
        transform->makeExposureDynamic();
        transform->makeContrastDynamic();
        const OCIO::ConstContextRcPtr context = OCIO::Context::Create();
        const auto processor =
            config->getProcessor(context, transform, OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::TransformBuildFailed);
        }
        std::vector<std::byte> semanticBytes;
        appendText(semanticBytes, sourceId);
        appendText(semanticBytes, std::to_string(exposure));
        appendText(semanticBytes, std::to_string(contrast));
        return extract(processor, render::OcioGpuProgramStage::ProcessEffect,
                       kOcioGpuExposureContrastSemanticsId, std::move(semanticBytes),
                       resolved.expectedRevision(), limits);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::TransformBuildFailed);
    }
}

render::OcioGpuProgramResult buildOcioGpuProgramForLut3d(
    const ResolvedBloomNeutralConfig& resolved, const std::string_view sourceId,
    const std::uint32_t edge, const render::OcioGpuInterpolation interpolation,
    const std::span<const float> samples, const render::OcioGpuProgramLimits& limits) noexcept {
    const std::uint64_t edge64 = edge;
    const std::uint64_t expected = edge64 * edge64 * edge64 * 3U;
    if (!validId(sourceId) || edge == 0 || expected != samples.size()) {
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }
    for (const float sample : samples) {
        if (!std::isfinite(sample)) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::UnsupportedResourceForm);
        }
    }
    const auto& config = resolved.impl().config();
    try {
        if (!config->getColorSpace(std::string(sourceId).c_str())) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::UnsupportedColorSpace);
        }
        auto transform = OCIO::Lut3DTransform::Create();
        transform->setGridSize(edge);
        switch (interpolation) {
        case render::OcioGpuInterpolation::Nearest:
            transform->setInterpolation(OCIO::INTERP_NEAREST);
            break;
        case render::OcioGpuInterpolation::Tetrahedral:
            transform->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
            break;
        default:
            transform->setInterpolation(OCIO::INTERP_LINEAR);
            break;
        }
        for (std::uint32_t r = 0; r < edge; ++r) {
            for (std::uint32_t g = 0; g < edge; ++g) {
                for (std::uint32_t b = 0; b < edge; ++b) {
                    const std::size_t index =
                        ((static_cast<std::size_t>(r) * edge + g) * edge + b) * 3U;
                    transform->setValue(r, g, b, samples[index], samples[index + 1],
                                        samples[index + 2]);
                }
            }
        }
        const OCIO::ConstContextRcPtr context = OCIO::Context::Create();
        const auto processor =
            config->getProcessor(context, transform, OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::TransformBuildFailed);
        }
        std::vector<std::byte> semanticBytes;
        appendText(semanticBytes, sourceId);
        appendText(semanticBytes, std::to_string(edge));
        appendText(semanticBytes, std::to_string(static_cast<unsigned>(interpolation)));
        return extract(processor, render::OcioGpuProgramStage::ProcessEffect,
                       kOcioGpuLut3dSemanticsId, std::move(semanticBytes),
                       resolved.expectedRevision(), limits);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::TransformBuildFailed);
    }
}

} // namespace bloom::color
