#include <bloom/ui/viewer_channel_display.hpp>

#include <limits>

namespace bloom::ui {
std::size_t viewerPackedSourceByteCost(
    const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) noexcept {
    if (!frame)
        return 0;
    if (frame->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked)
        return frame->displayOnlyFrame()->displayByteCost();
    const auto view = frame->displayBufferView();
    if (!view)
        return std::numeric_limits<std::size_t>::max();
    const auto geometry = frame->evaluatedBounds().size_bytes();
    if (geometry > std::numeric_limits<std::size_t>::max() - view->pixels.size_bytes())
        return std::numeric_limits<std::size_t>::max();
    const auto packedBytes = view->pixels.size_bytes() + geometry;
    const auto processBytes =
        frame->hasProcessFrame() ? frame->processImage().pixelStorageByteCost() : 0;
    return processBytes > std::numeric_limits<std::size_t>::max() - packedBytes
               ? std::numeric_limits<std::size_t>::max()
               : packedBytes + processBytes;
}
bool sameViewerPackedSource(
    const std::shared_ptr<const runtime::PreparedPreviewFrame>& a,
    const std::shared_ptr<const runtime::PreparedPreviewFrame>& b) noexcept {
    if (!a || !b)
        return false;
    const auto av = a->displayBufferView();
    const auto bv = b->displayBufferView();
    // Both handles pin their immutable storage, so an address cannot be recycled. A restamped
    // envelope may reuse these exact bytes/geometry without a new channel derivation.
    return av && bv && av->pixels.data() == bv->pixels.data() &&
           av->pixels.size() == bv->pixels.size() && av->layout == bv->layout &&
           av->dataWindow == bv->dataWindow && av->displayWindow == bv->displayWindow &&
           av->pixelAspect == bv->pixelAspect;
}

runtime::TaskResult<ViewerChannelDisplayHandle>
prepareViewerChannelDisplay(std::shared_ptr<const runtime::PreparedPreviewFrame> frame,
                            const ViewerChannel channel, const std::size_t byteLimit,
                            runtime::TaskContext& context) {
    using Result = runtime::TaskResult<ViewerChannelDisplayHandle>;
    if (context.isCancellationRequested())
        return Result::cancelled();
    const auto view = frame ? frame->displayBufferView() : std::nullopt;
    constexpr auto metadata =
        sizeof(ViewerChannelDisplay) + 2 * sizeof(std::shared_ptr<const void>);
    if (!view || channel == ViewerChannel::Rgba || channel > ViewerChannel::Alpha ||
        view->pixels.size() != view->layout.pixelCount || view->pixels.empty() ||
        metadata > byteLimit || view->pixels.size_bytes() > byteLimit - metadata) {
        return Result::failed({{.code = "bloom.ui.viewer-channel-admission",
                                .summary = "Viewer channel pixels exceed the preparation allowance",
                                .detail = {},
                                .suggestedAction = {}}});
    }
    auto output = std::make_shared<ViewerChannelDisplay>();
    output->frame = std::move(frame);
    output->channel = channel;
    output->pixels.resize(view->pixels.size());
    if (output->byteCost() > byteLimit)
        return Result::failed({{.code = "bloom.ui.viewer-channel-allocation",
                                .summary = "Actual Viewer channel capacity exceeds the allowance",
                                .detail = {},
                                .suggestedAction = {}}});
    const auto width = view->dataWindow.value_or(view->displayWindow).extent().width();
    for (std::size_t i = 0; i < view->pixels.size(); ++i) {
        if (i % width == 0 && context.isCancellationRequested())
            return Result::cancelled();
        const auto pixel = view->pixels[i];
        if (channel == ViewerChannel::Rgb) {
            output->pixels[i] = {pixel.red, pixel.green, pixel.blue, 255};
        } else {
            const auto grey = channel == ViewerChannel::Red     ? pixel.red
                              : channel == ViewerChannel::Green ? pixel.green
                              : channel == ViewerChannel::Blue  ? pixel.blue
                                                                : pixel.alpha;
            output->pixels[i] = {grey, grey, grey, 255};
        }
    }
    return Result::succeeded(std::move(output));
}
} // namespace bloom::ui
