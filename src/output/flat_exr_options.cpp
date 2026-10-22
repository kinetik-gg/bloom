#include "flat_exr_preset_contract.hpp"
#include <OpenEXR/openexr_compression.h>
#include <bloom/output/flat_exr_options.hpp>

#include <cmath>
#include <limits>

namespace bloom::output {
std::optional<std::uint64_t>
flatExrCodecWorkspaceBytesV1(const std::uint64_t width, const std::uint64_t height,
                             const FlatExrCompressionV1 compression) noexcept {
    std::uint64_t rows = 0;
    switch (compression) {
    case FlatExrCompressionV1::Zip:
        rows = 16;
        break;
    case FlatExrCompressionV1::Piz:
        rows = 32;
        break;
    case FlatExrCompressionV1::Zips:
    case FlatExrCompressionV1::None:
        rows = 1;
        break;
    }
    if (rows == 0 || width == 0 || height == 0 ||
        width > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) / (16 * rows))
        return std::nullopt;
    const auto block = static_cast<std::size_t>(16 * width * rows);
    const auto bound = static_cast<std::uint64_t>(exr_compress_max_buffer_size(block));
    // OpenEXR 3.4.15: OutputFile owns two line buffers at numThreads=1. Each
    // owns input, compressed output and ZIP/PIZ scratch. The public bound includes
    // Huffman spare storage; eight bounds cover both codec pipelines and allocator
    // objects. PIZ also owns bitmap/LUT tables (8192 + 65536*2 bytes per pipeline).
    // The writer owns bytesPerLine/offset tables and chunk offsets (at most 24H).
    // InputFile(numThreads=1) reuses one decoder, so its workspace fits this envelope.
    // This is a geometry-derived admission estimate, not physical-allocation telemetry.
    constexpr std::uint64_t tableBytes = 2 * (8192 + 65536 * 2);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (bound < block || bound > (maximum - tableBytes) / 8)
        return std::nullopt;
    const auto buffers = 8 * bound + tableBytes;
    if (height > (maximum - buffers) / 24)
        return std::nullopt;
    return buffers + 24 * height;
}

std::string_view flatExrCompressionNameV1(FlatExrCompressionV1 compression) noexcept {
    switch (compression) {
    case FlatExrCompressionV1::Zip:
        return "zip";
    case FlatExrCompressionV1::Piz:
        return "piz";
    case FlatExrCompressionV1::Zips:
        return "zips";
    case FlatExrCompressionV1::None:
        return "none";
    }
    return {};
}
std::shared_ptr<const PreparedFlatExrOutputV1>
PreparedFlatExrOutputV1::prepare(const runtime::EvaluationColorIntent& source,
                                 FlatExrRgba32fOptionsV1 options) {
    if (options.outputColorSpaceId.empty())
        options.outputColorSpaceId = source.workingColorSpaceId;
    if (flatExrCompressionNameV1(options.compression).empty() ||
        !detail::flatExrChromaticityBitsForWorkingColorSpaceV1(options.outputColorSpaceId))
        return {};
    auto result = std::shared_ptr<PreparedFlatExrOutputV1>(new PreparedFlatExrOutputV1());
    result->options_ = std::move(options);
    result->sourceColorSpaceId_ = source.workingColorSpaceId;
    result->sourceRevision_ = source.ocioConfigRevision;
    if (result->options_.outputColorSpaceId != source.workingColorSpaceId) {
        const auto revision = source.ocioConfigRevision == core::Sha256Digest{}
                                  ? color::kBloomNeutralV1ConfigDigest
                                  : source.ocioConfigRevision;
        const auto uri = revision == color::kBloomNeutralV1ConfigDigest
                             ? color::kBloomNeutralV1ConfigUri
                         : source.ocioConfigUri == runtime::kBloomNeutralOcioConfigUri
                             ? color::kAcesCgV1ConfigUri
                             : source.ocioConfigUri;
        auto resolved = color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, uri,
                                                  revision, source.workingColorSpaceId);
        auto config = std::move(resolved).takeResolved();
        if (!config)
            return {};
        auto prepared = color::CpuColorSpaceProcessor::prepare(*config, source.workingColorSpaceId,
                                                               result->options_.outputColorSpaceId);
        if (!prepared)
            return {};
        result->processor_ = prepared.share();
    }
    return result;
}
bool PreparedFlatExrOutputV1::matches(const runtime::EvaluationColorIntent& source) const noexcept {
    return source.workingColorSpaceId == sourceColorSpaceId_ &&
           source.ocioConfigRevision == sourceRevision_;
}
bool PreparedFlatExrOutputV1::apply(std::span<const render::Rgba32f> source,
                                    std::span<std::array<float, 4>> target) const noexcept {
    if (source.size() != target.size())
        return false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto& p = source[i];
        target[i] = {p.red(), p.green(), p.blue(), p.alpha()};
        if (processor_) {
            for (std::size_t c = 0; c < 3; ++c)
                target[i][c] = p.alpha() == 0.0F ? 0.0F : target[i][c] / p.alpha();
        }
    }
    if (!processor_)
        return true;
    if (!processor_->apply(target))
        return false;
    for (auto& p : target) {
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] = p[3] == 0.0F ? 0.0F : p[c] * p[3];
            if (!std::isfinite(p[c]))
                return false;
        }
    }
    return true;
}
} // namespace bloom::output
