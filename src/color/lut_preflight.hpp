#pragma once
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
namespace bloom::color::detail {
[[nodiscard]] LutError preflightLut(std::string_view text, std::uint32_t format,
                                    const std::function<bool()>& cancellation = {},
                                    bool* identity = nullptr);
} // namespace bloom::color::detail
