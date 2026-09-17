#pragma once

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/view_adjust.hpp>

namespace bloom::runtime::detail {
[[nodiscard]] render::ImageResult<render::PreparedReferenceDisplayBuffer>
adjustedQualifiedBuffer(const color::PreparedCpuDisplayProcessorHandle& handle,
                        render::Rgba32fImageView source, ViewAdjust adjust, std::size_t chunkPixels,
                        std::size_t byteLimit, const CancellationToken& cancellation);
}
