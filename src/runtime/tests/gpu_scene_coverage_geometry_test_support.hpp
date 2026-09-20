#pragma once

// Test-only CPU oracle for the immutable vector coverage geometry emitted by scene preparation. It
// reconstructs the exact 8-bit coverage mask the native GpuPathCoverage kernel produces from the
// same integer spans -- the count of covered quarter-samples in the pixel, quantized with the exact
// `(count * 255 + 8) / 16` the shader uses -- so the preparation replay can composite the CPU
// reference without a stored per-pixel host mask. This mirrors src/render/tests
// gpu_path_coverage_geometry_tests.cpp; it is a test helper, never production code.

#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bloom::gpu_scene_coverage_test {

[[nodiscard]] inline std::vector<std::uint8_t>
reconstructCoverageMask(const bloom::render::PathRasterCoverageGeometry& geometry) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(geometry.width) * geometry.height, 0);
    for (std::uint32_t row = 0; row < geometry.height; ++row) {
        for (std::uint32_t px = 0; px < geometry.width; ++px) {
            std::uint32_t count = 0;
            const std::uint32_t lo = px * 4U;
            const std::uint32_t hi = lo + 3U;
            for (std::uint32_t sy = 0; sy < 4; ++sy) {
                const auto range = geometry.rows[static_cast<std::size_t>(row) * 4U + sy];
                for (std::uint32_t i = 0; i < range.count; ++i) {
                    const auto span = geometry.spans[range.offset + i];
                    const std::uint32_t a = std::max(span.first, lo);
                    const std::uint32_t b = std::min(span.last, hi);
                    if (a <= b) {
                        count += b - a + 1U;
                    }
                }
            }
            out[static_cast<std::size_t>(row) * geometry.width + px] =
                static_cast<std::uint8_t>((count * 255U + 8U) / 16U);
        }
    }
    return out;
}

// The reconstructible 8-bit mask for a prepared covered command, from either the immutable geometry
// or the integer-grid text leaf's host bitmap. Empty only when neither representation is present.
[[nodiscard]] inline std::vector<std::uint8_t>
coverageMaskBytes(const bloom::runtime::GpuSceneCoverageSolidCommand& command) {
    if (command.geometry != nullptr) {
        return reconstructCoverageMask(*command.geometry);
    }
    if (command.coverage != nullptr) {
        return {command.coverage->begin(), command.coverage->end()};
    }
    return {};
}

} // namespace bloom::gpu_scene_coverage_test
