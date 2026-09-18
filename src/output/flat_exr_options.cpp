#include "flat_exr_preset_contract.hpp"
#include <bloom/output/flat_exr_options.hpp>
#include <cmath>

namespace bloom::output {
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
    auto result = std::make_shared<PreparedFlatExrOutputV1>();
    result->options_ = std::move(options);
    result->source_ = source;
    if (result->options_.outputColorSpaceId != source.workingColorSpaceId) {
        const auto revision = source.ocioConfigRevision == core::Sha256Digest{}
                                  ? color::kBloomNeutralV1ConfigDigest
                                  : source.ocioConfigRevision;
        const auto uri = revision == color::kBloomNeutralV1ConfigDigest
                             ? color::kBloomNeutralV1ConfigUri
                             : color::kAcesCgV1ConfigUri;
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
    return source.workingColorSpaceId == source_.workingColorSpaceId &&
           source.ocioConfigRevision == source_.ocioConfigRevision;
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
