#pragma once
#include <bloom/media/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <filesystem>

namespace bloom::runtime::detail {
struct ImageSourceSelection {
    std::filesystem::path path;
    core::Sha256Digest digest;
    media::ImageInterpretation interpretation;
    std::string cacheKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
};
[[nodiscard]] ImageSourceSelection selectImageSource(const CompiledImageSource& source,
                                                     core::RationalTime time,
                                                     document::FrameRate rate,
                                                     const std::filesystem::path& base,
                                                     const CancellationToken& cancel);
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
evaluateImageSource(const ImageSourceSelection& selection,
                    render::Rgba32fImageDescriptor composition, double horizontalScale,
                    double verticalScale, std::size_t budget, OperationCache& cache,
                    const CancellationToken& cancel);
} // namespace bloom::runtime::detail
