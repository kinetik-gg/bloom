#pragma once
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <list>
namespace bloom::runtime::detail {
struct VideoSourceSelection {
    std::shared_ptr<const media::provider::FrameProduct> frame;
    std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
    std::string cacheKey, warning;
    // Decode-only identity: asset content digest + video stream id + selected frame index. It
    // excludes the input/working colour space, config revision, display and proxy, because the raw
    // codec-side preparation does not depend on them. The GPU media colour split keys its raw video
    // upload on this identity, so a changed working space or display reuses the decoded frame.
    std::string decodeKey;
    std::string inputColorSpaceId;
    std::string resolvedInputColorSpaceName, inputColorSpaceWarning, workingColorSpaceId;
    core::Sha256Digest configRevision;
    std::uint32_t interpretation = 0;
    bool inputColorSpaceAutomatic = false;
    bool cancelled = false;
    platform::FileSnapshotStatistics sourceWork;
    bool retainedRevision = false;
};
class VideoSourceContext final {
  public:
    media::video::DecodedVideoCache cache{0};
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    session(const std::string& key, const std::filesystem::path& path, bool allowInsertion);
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    retainedSession(const std::string& key);
    void discardFailedSession(const std::string& key,
                              const std::shared_ptr<media::video::VideoDecodeSession>& session);
    void bindRetirement(std::shared_ptr<render::detail::PackedHostRetirementState> retirement);
    // Removing the owning sessions also invalidates retained source authority. In-flight callers
    // may finish their immutable revision but cannot reinsert into this list after the boundary.
    void clear();
    void invalidateSources();
    // Signal-only for a rooted caller: closes source admission and queues session destruction.
    void beginShutdown();
    [[nodiscard]] std::uint64_t sourceSnapshotBytes() const { return quota_->residentBytes(); }
    [[nodiscard]] std::size_t sourceSnapshotCount() const { return quota_->residentFiles(); }

  private:
    std::mutex mutex_;
    struct SessionEntry {
        std::string key;
        std::filesystem::path path;
        std::shared_ptr<media::video::VideoDecodeSession> session;
    };
    std::list<SessionEntry> sessions_;
    std::shared_ptr<platform::FileSnapshotQuota> quota_ =
        std::make_shared<platform::FileSnapshotQuota>(32ULL * 1024 * 1024 * 1024, 8);
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
};
[[nodiscard]] VideoSourceSelection selectVideoSource(
    const CompiledVideoSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, VideoSourceContext& context, bool useCache,
    const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent = EvaluationColorIntent::LinearRec709Scene,
    MediaSourceRevisionPolicy revisionPolicy = MediaSourceRevisionPolicy::Revalidate,
    bool allowInsertion = true,
    std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max());
} // namespace bloom::runtime::detail
