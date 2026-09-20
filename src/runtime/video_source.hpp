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
};
class VideoSourceContext final {
  public:
    media::video::DecodedVideoCache cache{0};
    [[nodiscard]] std::shared_ptr<media::video::VideoDecodeSession>
    session(const std::filesystem::path& path);

  private:
    std::mutex mutex_;
    std::list<std::pair<std::filesystem::path, std::shared_ptr<media::video::VideoDecodeSession>>>
        sessions_;
};
[[nodiscard]] VideoSourceSelection selectVideoSource(
    const CompiledVideoSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, VideoSourceContext& context, bool useCache,
    const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent = EvaluationColorIntent::LinearRec709Scene);
} // namespace bloom::runtime::detail
