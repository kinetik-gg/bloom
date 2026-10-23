#include "video_source.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/media/image.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <limits>
#include <numeric>
namespace bloom::runtime::detail {
namespace {
struct SessionRetirement final : render::detail::PackedHostRetirementNode {
    std::unique_ptr<media::video::VideoDecodeSession> session;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    bool registered = false;
    ~SessionRetirement() {
        session.reset();
        if (registered)
            render::detail::releasePackedHostRegistration(retirement);
    }
};
std::shared_ptr<media::video::VideoDecodeSession>
makeSession(const std::filesystem::path& path,
            const std::shared_ptr<platform::FileSnapshotQuota>& quota,
            const std::shared_ptr<render::detail::PackedHostRetirementState>& retirement) {
    auto owner = std::make_unique<SessionRetirement>();
    owner->retirement = retirement;
    if (retirement) {
        if (!render::detail::preparePackedHostRetirement(retirement) ||
            !render::detail::registerPackedHostAllocation(retirement))
            return {};
        owner->registered = true;
    }
    owner->session = std::make_unique<media::video::VideoDecodeSession>(
        path, media::video::VideoDecodeSession::defaultWorker(), quota, std::filesystem::path{},
        retirement);
    owner->destroy = [](render::detail::PackedHostRetirementNode* node) noexcept {
        auto* entry = static_cast<SessionRetirement*>(node);
        entry->registered = false; // The queue releases its own registration after destroy.
        delete entry;
    };
    auto* entry = owner.release();
    return std::shared_ptr<media::video::VideoDecodeSession>(
        entry->session.get(), [entry](media::video::VideoDecodeSession*) noexcept {
            if (entry->registered)
                render::detail::enqueuePackedHostRetirement(entry->retirement, entry);
            else
                delete entry;
        });
}
} // namespace
void VideoSourceContext::setSessionByteBudget(const std::size_t bytes) {
    std::list<SessionEntry> detached;
    const std::lock_guard lock(mutex_);
    sessionByteBudget_ = bytes;
    admitLocked(detached);
}
std::size_t VideoSourceContext::retainedSessionCount() {
    const std::lock_guard lock(mutex_);
    return sessions_.size();
}
void VideoSourceContext::admitLocked(std::list<SessionEntry>& detached) {
    if (sessionByteBudget_ == 0)
        sessionByteBudget_ =
            std::max<std::size_t>(physicalMemoryBytes() / 4U, std::size_t{2} << 30U);
    std::size_t total = 0;
    std::size_t kept = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        total += it->workerBytes;
        const bool admitted = kept < kSnapshotFiles &&
                              (kept < kMinimumRetainedSessions || total <= sessionByteBudget_);
        if (admitted) {
            ++kept;
            ++it;
        } else {
            total -= it->workerBytes;
            detached.splice(detached.end(), sessions_, it++);
        }
    }
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::session(const std::string& key, const std::filesystem::path& path,
                            const bool allowInsertion, const std::size_t workerBytes) {
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return {};
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end() && found->path == path) {
            sessions_.splice(sessions_.begin(), sessions_, found);
            return sessions_.front().session;
        }
        retirement = retirement_;
        generation = generation_;
    }
    // Worker bootstrap, library verification and scheduler creation hold no mutex needed by UI
    // source close. A refresh racing construction prevents insertion into the new generation.
    auto session = makeSession(path, quota_, retirement);
    std::list<SessionEntry> detached;
    if (session && allowInsertion) {
        std::lock_guard lock(mutex_);
        if (closed_ || generation != generation_ ||
            generation == std::numeric_limits<std::uint64_t>::max())
            return session;
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end())
            detached.splice(detached.end(), sessions_, found);
        sessions_.push_front({key, path, session, workerBytes});
        admitLocked(detached);
    }
    return session;
}
std::shared_ptr<media::video::VideoDecodeSession>
VideoSourceContext::retainedSession(const std::string& key) {
    std::lock_guard lock(mutex_);
    if (closed_)
        return {};
    const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
    if (found == sessions_.end())
        return {};
    sessions_.splice(sessions_.begin(), sessions_, found);
    return sessions_.front().session;
}
void VideoSourceContext::discardFailedSession(
    const std::string& key, const std::shared_ptr<media::video::VideoDecodeSession>& session) {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find(sessions_, key, &SessionEntry::key);
        if (found != sessions_.end() && found->session == session)
            detached.splice(detached.end(), sessions_, found);
    }
}
void VideoSourceContext::bindRetirement(
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement) {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        if (retirement_ != retirement) {
            detached.splice(detached.end(), sessions_);
            retirement_ = std::move(retirement);
            if (generation_ != std::numeric_limits<std::uint64_t>::max())
                ++generation_;
        }
    }
}
void VideoSourceContext::clear() {
    invalidateSources();
    cache.clear(); // Explicit refresh is off-UI; signal-only shutdown does not free decoded planes.
}
void VideoSourceContext::invalidateSources() {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        detached.splice(detached.end(), sessions_);
        if (generation_ != std::numeric_limits<std::uint64_t>::max())
            ++generation_;
    }
}
void VideoSourceContext::beginShutdown() {
    std::list<SessionEntry> detached;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        detached.splice(detached.end(), sessions_);
    }
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
                                       const CancellationToken& cancel,
                                       const EvaluationColorIntent& colorIntent,
                                       const MediaSourceRevisionPolicy revisionPolicy,
                                       const bool allowInsertion,
                                       const std::size_t hostByteBudget) {
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
    const auto input = document::effectiveAssetInputColor(asset.interpretation, source.colorSpace,
                                                          source.inputColorSpaceId);
    selection.interpretation = static_cast<std::uint32_t>(input.colorSpace);
    selection.inputColorSpaceId = input.inputColorSpaceId;
    auto config = resolveInputColorConfig(colorIntent);
    if (!config) {
        selection.warning = "The selected OCIO input configuration is unavailable";
        selection.cacheKey = "video-colour-config-unavailable";
        return selection;
    }
    const media::provider::ColourTags tags{
        static_cast<std::int32_t>(stream->primaries), static_cast<std::int32_t>(stream->transfer),
        static_cast<std::int32_t>(stream->matrix), static_cast<std::int32_t>(stream->range)};
    const auto videoResolution = media::video::resolveVideoInputColorSpace(
        *config, tags, selection.interpretation, selection.inputColorSpaceId);
    const InputColorSpaceResolution resolution{
        videoResolution.id, videoResolution.name, videoResolution.warning,
        videoResolution.noConversion, videoResolution.automatic};
    selection.inputColorSpaceAutomatic = resolution.automatic;
    selection.resolvedInputColorSpaceName = resolution.name;
    selection.inputColorSpaceWarning = resolution.warning;
    selection.workingColorSpaceId = std::string(config->processColorSpaceId());
    selection.configRevision = config->expectedRevision();
    if (!resolution.warning.empty() && resolution.id.empty()) {
        selection.warning = resolution.warning;
        selection.cacheKey = "video-colour-input-unresolved";
        return selection;
    }
    selection.inputColorSpaceId = resolution.id;
    std::string diagnostic;
    selection.inputProcessor = prepareInputColorProcessor(*config, resolution, diagnostic);
    if (!resolution.noConversion && !selection.inputProcessor) {
        selection.warning = diagnostic;
        selection.cacheKey = "video-colour-input-unavailable";
        return selection;
    }
    OperationKey sourceKey;
    const auto sourceDigest = asset.contentDigest.toLowercaseHex();
    sourceKey.add(std::string(sourceDigest.begin(), sourceDigest.end()));
    sourceKey.add(asset.locator.path);
    sourceKey.add(asset.locator.relinkHint);
    const auto baseUtf8 = base.generic_u8string();
    sourceKey.add(std::string(baseUtf8.begin(), baseUtf8.end()));
    const auto revisionKey = sourceKey.digest();
    auto session = revisionPolicy == MediaSourceRevisionPolicy::Retained
                       ? context.retainedSession(revisionKey)
                       : nullptr;
    if (!session) {
        const auto path =
            media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base);
        session =
            context.session(revisionKey, path, allowInsertion,
                            media::video::estimatedDecodeWorkerBytes(asset.width, asset.height));
    }
    if (!session) {
        selection.warning = "Video source owner is closing";
        selection.cancelled = true;
        return selection;
    }
    // Retained revisions serve interactive preview, which may use the discovered VA-API decoder;
    // revalidated (final/export) requests always decode through the software reference.
    media::video::FrameDecodeProvenance decodeProvenance;
    const auto product =
        session->frame(runtime::video::probeMetadata(asset), stream->id, index,
                       selection.interpretation, useCache ? &context.cache : nullptr,
                       selection.inputColorSpaceId, selection.workingColorSpaceId,
                       selection.configRevision, [&] { return cancel.isCancellationRequested(); },
                       {revisionPolicy == MediaSourceRevisionPolicy::Retained
                            ? media::video::SourceRevisionPolicy::Retained
                            : media::video::SourceRevisionPolicy::Revalidate,
                        allowInsertion,
                        &selection.sourceWork,
                        &selection.retainedRevision,
                        {},
                        hostByteBudget},
                       {revisionPolicy == MediaSourceRevisionPolicy::Retained, &decodeProvenance});
    if (const auto* error = std::get_if<media::provider::Unavailable>(&product)) {
        selection.warning = error->detail;
        selection.cancelled = error->reason == media::provider::Error::Cancelled;
        // A failed cold capture must not pin an unresolved/offline locator. On reconnect the
        // next request resolves the locator again. Pointer identity protects a newer generation.
        if (allowInsertion)
            context.discardFailedSession(revisionKey, session);
    } else
        selection.frame = std::get<std::shared_ptr<const media::provider::FrameProduct>>(product);
    OperationKey key;
    const auto digest = asset.contentDigest.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(stream->id);
    key.add(index);
    key.add(selection.interpretation);
    key.add(selection.inputColorSpaceId);
    key.add(selection.workingColorSpaceId);
    // A Bloom-synthesized input space (Rec.2100 HLG) is defined by OCIO built-ins, so its
    // definition and the linked OCIO version are part of the transform identity.
    key.add(color::synthesizedColorSpaceIdentity(selection.inputColorSpaceId));
    const auto revision = selection.configRevision.toLowercaseHex();
    key.add(std::string(revision.begin(), revision.end()));
    key.add(selection.frame != nullptr);
    key.add(selection.warning);
    const bool hardwareDecoded =
        decodeProvenance.implementation == media::provider::Implementation::Hardware;
    key.add(hardwareDecoded);
    selection.decodeProvenance = decodeProvenance.description;
    selection.cacheKey = key.bytes();
    // Decode-only identity for the raw codec-side preparation. It deliberately omits the input and
    // working colour spaces, config revision, display and proxy so a changed working space reuses
    // the decoded frame.
    OperationKey decodeKey;
    decodeKey.add(std::string{"gpu-raw-video-prep-v1"});
    decodeKey.add(std::string(digest.begin(), digest.end()));
    decodeKey.add(stream->id);
    decodeKey.add(index);
    // A hardware-decoded preview frame is a distinct preparation from the software reference.
    if (hardwareDecoded)
        decodeKey.add(std::string{"vaapi"});
    selection.decodeKey = decodeKey.digest();
    return selection;
}
} // namespace bloom::runtime::detail

namespace bloom::runtime {
std::shared_ptr<detail::VideoSourceContext> CpuCompositionEvaluator::videoContext() const {
    std::lock_guard lock(assetContext_->mutex);
    if (!assetContext_->video)
        assetContext_->video = std::make_shared<detail::VideoSourceContext>();
    assetContext_->video->bindRetirement(cache_->sourceRetirementState());
    cache_->bindVideoSources(assetContext_->video);
    return assetContext_->video;
}
void CpuCompositionEvaluator::setVideoCacheByteBudget(std::size_t budget) const {
    videoContext()->cache.setByteBudget(budget);
}
void CpuCompositionEvaluator::clearDecodedVideoCache() const { videoContext()->clear(); }
} // namespace bloom::runtime
