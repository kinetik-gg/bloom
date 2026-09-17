#include "view_adjust_support.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <vector>

namespace bloom::runtime::detail {
render::ImageResult<render::PreparedReferenceDisplayBuffer>
adjustedQualifiedBuffer(const color::PreparedCpuDisplayProcessorHandle& handle,
                        const render::Rgba32fImageView source, const ViewAdjust adjust,
                        const std::size_t chunkPixels, const std::size_t byteLimit,
                        const CancellationToken& cancellation) {
    using Result = render::ImageResult<render::PreparedReferenceDisplayBuffer>;
    const auto fail = [](render::ImageErrorCode code) {
        return Result::failure(render::ImageError::codeOnly(code));
    };
    const auto descriptor = source.descriptor();
    if (!descriptor || !adjust.valid() || chunkPixels == 0)
        return fail(render::ImageErrorCode::InvalidParameter);
    const auto display = render::ReferenceDisplayBufferDescriptor::create(
        descriptor->displayWindow(), descriptor->pixelAspect());
    if (!display)
        return Result::failure(*display.error());
    const auto chunk =
        std::min<std::size_t>(chunkPixels, descriptor->dataWindow().extent().width());
    const auto displayBytes = display.value()->layout().pixelStorageBytes;
    if (displayBytes > byteLimit || chunk > (byteLimit - displayBytes) / (4 * sizeof(float)))
        return fail(render::ImageErrorCode::PixelStorageBudgetExceeded);
    try {
        auto builder =
            render::ReferenceDisplayBufferBuilder::create(*display.value(), displayBytes);
        if (!builder)
            return Result::failure(*builder.error());
        std::vector<float> scratch(chunk * 4);
        const auto data = descriptor->dataWindow();
        const auto window = descriptor->displayWindow();
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested())
                return fail(render::ImageErrorCode::InvalidState);
            if (y < data.originY() || y >= data.maxYExclusive())
                continue;
            const auto input = source.row(y);
            auto output = builder.value()->row(y);
            if (!input || !output)
                return fail(render::ImageErrorCode::InvalidState);
            const auto left = std::max(data.originX(), window.originX());
            const auto right = std::min(data.maxXExclusive(), window.maxXExclusive());
            for (auto x = left; x < right;) {
                if (cancellation.isCancellationRequested())
                    return fail(render::ImageErrorCode::InvalidState);
                const auto count = std::min(chunk, static_cast<std::size_t>(right - x));
                auto pixels =
                    output.value()->subspan(static_cast<std::size_t>(x - window.originX()), count);
                const auto values =
                    input.value()->subspan(static_cast<std::size_t>(x - data.originX()), count);
                if (const auto error = color::applyBloomNeutralDisplayChunk(
                        handle, values, std::span(scratch).first(count * 4), pixels))
                    return Result::failure(*error);
                for (std::size_t i = 0; i < count; ++i) {
                    pixels[i].red = ViewAdjust::quantize(
                        adjust.fromEncoded(static_cast<double>(scratch[i * 4])));
                    pixels[i].green = ViewAdjust::quantize(
                        adjust.fromEncoded(static_cast<double>(scratch[i * 4 + 1])));
                    pixels[i].blue = ViewAdjust::quantize(
                        adjust.fromEncoded(static_cast<double>(scratch[i * 4 + 2])));
                }
                x += static_cast<std::int64_t>(count);
            }
        }
        return std::move(*builder.value()).freeze();
    } catch (const std::bad_alloc&) {
        return fail(render::ImageErrorCode::AllocationFailure);
    } catch (const std::length_error&) {
        return fail(render::ImageErrorCode::AllocationFailure);
    }
}
} // namespace bloom::runtime::detail
