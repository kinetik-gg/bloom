#pragma once

#include <bloom/media/image.hpp>

namespace bloom::media::detail {

[[nodiscard]] ImageResult<ImageProbe> probeExr(const std::filesystem::path& path,
                                               core::Sha256Digest contentDigest,
                                               const CancelImageWork& cancel);
[[nodiscard]] ImageResult<ImageProbe> probeExr(std::span<const std::byte> bytes,
                                               core::Sha256Digest contentDigest,
                                               const CancelImageWork& cancel);

[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget);
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(std::span<const std::byte> bytes, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget);

[[nodiscard]] ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeExrChannels(std::span<const std::byte> bytes, const ImageChannelSelection& selection,
                  const CancelImageWork& cancel, const ImageProgress& progress,
                  std::size_t budget);

} // namespace bloom::media::detail
