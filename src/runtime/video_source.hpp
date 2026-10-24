#pragma once
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/video_source_report.hpp>
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
    // "software", "vaapi" or "software (fallback: <reason>)" for the decoded frame.
    std::string decodeProvenance;
};
class VideoSourceContext final {
  public:
    media::video::DecodedVideoCache cache{0};
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    session(const std::string& key, const std::filesystem::path& path, bool allowInsertion,
            std::size_t workerBytes = 0);
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
    // Retained sessions are admitted by memory, not a fixed count: their estimated worker bytes
    // must fit this budget (default a quarter of physical memory). At least two are kept so two
    // sources can alternate, and never more than the source-snapshot file quota can retain.
    void setSessionByteBudget(std::size_t bytes);
    [[nodiscard]] std::size_t retainedSessionCount();
    // Decode route and colour note of the most recently selected sources (most recent first, at
    // most kReportedSources). Status surfaces read this; it is not evaluation output.
    void report(VideoSourceReport value);
    [[nodiscard]] std::vector<VideoSourceReport> reports() const;
    static constexpr std::size_t kReportedSources = 32;
    static constexpr std::size_t kSnapshotFiles = 8;
    static constexpr std::size_t kMinimumRetainedSessions = 2;

  private:
    std::mutex mutex_;
    mutable std::mutex reportMutex_;
    std::vector<VideoSourceReport> reports_;
    std::uint64_t reportSequence_ = 0;
    struct SessionEntry {
        std::string key;
        std::filesystem::path path;
        std::shared_ptr<media::video::VideoDecodeSession> session;
        std::size_t workerBytes = 0;
    };
    // Called with mutex_ held; moves evicted entries into `detached` for destruction unlocked.
    void admitLocked(std::list<SessionEntry>& detached);
    std::size_t sessionByteBudget_ = 0;
    std::list<SessionEntry> sessions_;
    std::shared_ptr<platform::FileSnapshotQuota> quota_ =
        std::make_shared<platform::FileSnapshotQuota>(
            media::video::defaultSourceSnapshotQuotaBytes(), kSnapshotFiles);
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
