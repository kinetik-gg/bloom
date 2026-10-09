#pragma once

#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/task_scheduler.hpp>

namespace bloom::ui {
enum class ViewerChannel : std::uint8_t { Rgba, Rgb, Red, Green, Blue, Alpha };

// Immutable viewer-only derivation. The source remains the genuine prepared product; channel
// presentation does not replace its evaluation/display provenance or enter the authoring cache.
struct ViewerChannelDisplay final {
    std::shared_ptr<const runtime::PreparedPreviewFrame> frame;
    ViewerChannel channel = ViewerChannel::Rgba;
    std::vector<render::Rgba8> pixels;
    [[nodiscard]] std::size_t byteCost() const noexcept {
        return sizeof(ViewerChannelDisplay) + 2 * sizeof(std::shared_ptr<const void>) +
               pixels.capacity() * sizeof(render::Rgba8);
    }
};
using ViewerChannelDisplayHandle = std::shared_ptr<const ViewerChannelDisplay>;

// Worker-only materialization; no widget, session or mutable UI object is captured.
[[nodiscard]] runtime::TaskResult<ViewerChannelDisplayHandle>
prepareViewerChannelDisplay(std::shared_ptr<const runtime::PreparedPreviewFrame> frame,
                            ViewerChannel channel, std::size_t byteLimit,
                            runtime::TaskContext& context);
[[nodiscard]] bool
sameViewerPackedSource(const std::shared_ptr<const runtime::PreparedPreviewFrame>& a,
                       const std::shared_ptr<const runtime::PreparedPreviewFrame>& b) noexcept;
[[nodiscard]] std::size_t viewerPackedSourceByteCost(
    const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) noexcept;
} // namespace bloom::ui
