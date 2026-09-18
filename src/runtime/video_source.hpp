#pragma once
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <list>
namespace bloom::runtime::detail {
struct VideoSourceSelection {
    std::shared_ptr<const media::provider::FrameProduct> frame;
    std::string cacheKey, warning;
    std::uint32_t interpretation = 0;
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
[[nodiscard]] VideoSourceSelection
selectVideoSource(const CompiledVideoSource& source, core::RationalTime time,
                  document::FrameRate rate, const std::filesystem::path& base,
                  VideoSourceContext& context, bool useCache, const CancellationToken& cancel);
} // namespace bloom::runtime::detail
