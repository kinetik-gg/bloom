#include "video_source.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/media/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <limits>
#include <numeric>
namespace bloom::runtime::detail {
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::session(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find_if(sessions_, [&](const auto& s) { return s.first == path; });
    if (found != sessions_.end()) {
        sessions_.splice(sessions_.begin(), sessions_, found);
        return sessions_.front().second;
    }
    auto session = std::make_shared<media::video::VideoDecodeSession>(path);
    if (sessions_.size() == 4)
        sessions_.pop_back();
    sessions_.emplace_front(path, session);
    return session;
}
namespace {
std::optional<core::RationalTime> shifted(core::RationalTime time, std::int64_t start,
                                          document::FrameRate rate) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (start < -1000000000 || start > 1000000000)
        return {};
    const auto offset = core::RationalTime::create(start * rate.denominator(), rate.numerator());
    if (!offset)
        return {};
    const auto divisor = std::gcd(time.denominator(), offset->denominator());
    if (divisor <= 0)
        return {};
    const auto leftScale = offset->denominator() / divisor,
               rightScale = time.denominator() / divisor;
    if (leftScale <= 0 || rightScale <= 0)
        return {};
    if (time.numerator() == -limit - 1 || std::abs(time.numerator()) > limit / leftScale ||
        std::abs(offset->numerator()) > limit / rightScale ||
        time.denominator() > limit / leftScale)
        return {};
    const auto left = time.numerator() * leftScale, right = offset->numerator() * rightScale;
    if ((right > 0 && left < -limit + right) || (right < 0 && left > limit + right))
        return {};
    return core::RationalTime::create(left - right, time.denominator() * leftScale);
}
} // namespace
VideoSourceSelection selectVideoSource(const CompiledVideoSource& source, core::RationalTime time,
                                       document::FrameRate rate, const std::filesystem::path& base,
                                       VideoSourceContext& context, bool useCache,
                                       const CancellationToken& cancel) {
    VideoSourceSelection selection;
    if (!source.asset || source.asset->kind != document::AssetKind::Video) {
        selection.warning = "Video asset is missing; choose an asset or relink in Assets";
        selection.cacheKey = "missing-video";
        return selection;
    }
    const auto& asset = *source.asset;
    const auto stream =
        std::ranges::find(asset.videoStreams, 1U, &document::AssetVideoStream::kind);
    if (stream == asset.videoStreams.end() || stream->framePeriod.numerator() <= 0 ||
        stream->framePeriod.numerator() > std::numeric_limits<std::uint32_t>::max() ||
        stream->framePeriod.denominator() > std::numeric_limits<std::uint32_t>::max() ||
        asset.frames == 0) {
        selection.warning = "Video stream timing is unavailable";
        return selection;
    }
    const auto sourceRate =
        document::FrameRate::create(static_cast<std::uint32_t>(stream->framePeriod.denominator()),
                                    static_cast<std::uint32_t>(stream->framePeriod.numerator()));
    const auto local = shifted(time, source.startFrame, rate);
    const auto frame =
        local && sourceRate ? valueGraphFrameIndex(*local, *sourceRate) : std::nullopt;
    if (!frame) {
        selection.warning = "Video time exceeds the supported range";
        return selection;
    }
    auto index = static_cast<std::uint64_t>(std::max<std::int64_t>(0, *frame));
    if (source.loopMode == 1)
        index %= asset.frames;
    else if (source.loopMode == 2 && asset.frames > 1) {
        index %= 2 * (asset.frames - 1);
        if (index >= asset.frames)
            index = 2 * (asset.frames - 1) - index;
    } else
        index = std::min(index, asset.frames - 1);
    selection.interpretation = static_cast<std::uint32_t>(
        source.colorSpace == 0 ? static_cast<std::int64_t>(asset.interpretation.colorSpace)
                               : source.colorSpace);
    const auto path = media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base);
    const auto session = context.session(path);
    const auto product = session->frame(
        runtime::video::probeMetadata(asset), stream->id, index, selection.interpretation,
        useCache ? &context.cache : nullptr, [&] { return cancel.isCancellationRequested(); });
    if (const auto* error = std::get_if<media::provider::Unavailable>(&product)) {
        selection.warning = error->detail;
        selection.cancelled = error->reason == media::provider::Error::Cancelled;
    } else
        selection.frame = std::get<std::shared_ptr<const media::provider::FrameProduct>>(product);
    OperationKey key;
    const auto digest = asset.contentDigest.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(stream->id);
    key.add(index);
    key.add(selection.interpretation);
    key.add(selection.frame != nullptr);
    key.add(selection.warning);
    selection.cacheKey = key.bytes();
    return selection;
}
} // namespace bloom::runtime::detail

namespace bloom::runtime {
std::shared_ptr<detail::VideoSourceContext> CpuCompositionEvaluator::videoContext() const {
    std::lock_guard lock(assetContext_->mutex);
    if (!assetContext_->video)
        assetContext_->video = std::make_shared<detail::VideoSourceContext>();
    return assetContext_->video;
}
void CpuCompositionEvaluator::setVideoCacheByteBudget(std::size_t budget) const {
    videoContext()->cache.setByteBudget(budget);
}
} // namespace bloom::runtime
