#pragma once

#include <bloom/media/image.hpp>

namespace bloom::media::detail {

[[nodiscard]] ImageResult<ImageProbe> probeExr(const std::filesystem::path& path,
                                               core::Sha256Digest contentDigest,
                                               const CancelImageWork& cancel);

[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget);

} // namespace bloom::media::detail
