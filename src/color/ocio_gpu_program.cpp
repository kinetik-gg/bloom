#include <bloom/color/ocio_gpu_program.hpp>

#include "ocio_gpu_program_extract.hpp"
#include "ocio_gpu_program_serialize.hpp"
#include "ocio_gpu_program_worker.hpp"
#include "ocio_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::color {
namespace {

[[nodiscard]] bool validId(const std::string_view id) noexcept {
    return !id.empty() && id.size() <= 4096U && id.find('\0') == std::string_view::npos;
}

void appendU32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void appendText(std::vector<std::byte>& bytes, const std::string_view text) {
    appendU32(bytes, static_cast<std::uint32_t>(text.size()));
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]] render::OcioGpuProgramError mapLutError(const LutError error) noexcept {
    switch (error) {
    case LutError::None:
        return render::OcioGpuProgramError::None;
    case LutError::MissingFile:
    case LutError::ChangedFile:
        return render::OcioGpuProgramError::InvalidRequest;
    case LutError::UnsupportedFormat:
    case LutError::MalformedFile:
    case LutError::InvalidPixel:
        return render::OcioGpuProgramError::UnsupportedResourceForm;
    case LutError::FileTooLarge:
    case LutError::EdgeTooLarge:
    case LutError::HelperMemoryLimit:
        return render::OcioGpuProgramError::ResourceLimitExceeded;
    case LutError::HelperUnavailable:
        return render::OcioGpuProgramError::GpuProcessorUnavailable;
    case LutError::HelperCancelled:
        return render::OcioGpuProgramError::Cancelled;
    case LutError::TransformBuildFailed:
        return render::OcioGpuProgramError::TransformBuildFailed;
    case LutError::HelperProtocolViolation:
    case LutError::HelperDeadline:
    case LutError::HelperTerminated:
        return render::OcioGpuProgramError::ShaderExtractionFailed;
    }
    return render::OcioGpuProgramError::ShaderExtractionFailed;
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
    auto reflected = detail::extractOcioGpuProgram(processor, stage, semanticsId, limits);
    if (!reflected.succeeded() || !reflected.program.has_value()) {
        return render::OcioGpuProgramResult::failure(reflected.error);
    }
    return finalize(std::move(*reflected.program), std::move(semanticBytes), configRevision,
                    limits);
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
    const ResolvedBloomNeutralConfig& resolved, const LutFile& lutFile,
    const LutInterpolation interpolation, const LutDirection direction,
    const std::string_view processSpaceId, const std::string_view workingSpaceId,
    const render::OcioGpuProgramLimits& limits,
    const std::function<bool()>& cancellation) noexcept {
    try {
        if (!validId(processSpaceId) || !validId(workingSpaceId) ||
            static_cast<std::uint32_t>(interpolation) >
                static_cast<std::uint32_t>(LutInterpolation::Best) ||
            static_cast<std::uint32_t>(direction) >
                static_cast<std::uint32_t>(LutDirection::Inverse)) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
        }
        if (lutFile.error != LutError::None) {
            return render::OcioGpuProgramResult::failure(mapLutError(lutFile.error));
        }
        if (lutFile.bytes.empty() || lutFile.bytes.size() > kMaximumLutBytes) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::ResourceLimitExceeded);
        }
        if (lutFile.format < 1 || lutFile.format > 4 || lutFile.digest == core::Sha256Digest{}) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
        }
        {
            const auto& config = resolved.impl().config();
            const auto process = config->getColorSpace(std::string(processSpaceId).c_str());
            const auto working = config->getColorSpace(std::string(workingSpaceId).c_str());
            if (!process || !working || process->isData() || working->isData()) {
                return render::OcioGpuProgramResult::failure(
                    render::OcioGpuProgramError::UnsupportedColorSpace);
            }
        }
#ifndef __linux__
        // No confinement/process-supervision primitive: never parse LUT bytes in-process.
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::ExternalLutBoundaryRequired);
#else
        if (cancellation && cancellation()) {
            return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::Cancelled);
        }
        auto extracted = detail::extractFileTransformGpuProgram(
            lutFile.bytes, lutFile.digest, lutFile.format, interpolation, direction, cancellation);
        if (extracted.error != LutError::None) {
            return render::OcioGpuProgramResult::failure(mapLutError(extracted.error));
        }
        auto deserialized = detail::deserializeOcioGpuProgram(extracted.bytes, limits);
        if (!deserialized.succeeded() || !deserialized.program.has_value()) {
            return render::OcioGpuProgramResult::failure(deserialized.error);
        }
        auto program = std::move(*deserialized.program);
        if (program.semanticsId != std::string(kOcioGpuFileTransformSemanticsId)) {
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::ShaderExtractionFailed);
        }
        std::vector<std::byte> semanticBytes;
        const auto digestBytes = std::as_bytes(std::span(lutFile.digest.bytes()));
        semanticBytes.assign(digestBytes.begin(), digestBytes.end());
        appendU32(semanticBytes, lutFile.format);
        appendU32(semanticBytes, static_cast<std::uint32_t>(interpolation));
        appendU32(semanticBytes, static_cast<std::uint32_t>(direction));
        appendText(semanticBytes, workingSpaceId);
        appendText(semanticBytes, processSpaceId);
        return finalize(std::move(program), std::move(semanticBytes), resolved.expectedRevision(),
                        limits);
#endif
    } catch (const std::bad_alloc&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::ResourceLimitExceeded);
    } catch (const std::exception&) {
        return render::OcioGpuProgramResult::failure(
            render::OcioGpuProgramError::ShaderExtractionFailed);
    }
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
