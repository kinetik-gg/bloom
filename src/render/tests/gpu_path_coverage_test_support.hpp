#pragma once

// Private shared fixtures for the GpuPathCoverage native test. Split out of
// gpu_path_coverage_tests.cpp so the parity and lifecycle translation units stay
// under the file budget; every test and the single CTest name are unchanged.
// This header is never installed or included by production code.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_path_coverage.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::render::gpu_path_coverage_test {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] inline Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] inline std::vector<std::uint8_t> cpuCoverage(const PathRaster& raster,
                                                           const ImageWindow& window,
                                                           const PathFillRule rule,
                                                           const bool stroke) {
    const std::uint32_t width = window.extent().width();
    const std::uint32_t height = window.extent().height();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height, 0);
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto offset = static_cast<std::size_t>(row) * width;
        if (!raster.coverageRow(window.originX(), window.originY() + row,
                                std::span<std::uint8_t>(bytes.data() + offset, width), rule,
                                stroke)) {
            return {};
        }
    }
    return bytes;
}

[[nodiscard]] inline PathRaster rasterOf(ImageResult<PathRaster> result) {
    return result ? *result.value() : PathRaster{};
}

[[nodiscard]] inline GpuPathCoveragePollResult pollToCompletion(GpuPathCoverage& coverage) {
    GpuPathCoveragePollResult poll = GpuPathCoveragePollResult::Pending;
    while (poll == GpuPathCoveragePollResult::Pending) {
        poll = coverage.poll();
    }
    return poll;
}

[[nodiscard]] inline GpuSolidPollResult pollSolidToCompletion(GpuSolid& solid) {
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    return poll;
}

[[nodiscard]] inline bool runCoverage(GpuPathCoverage& producer, const ImageWindow& window,
                                      const PathFillRule rule, const bool stroke,
                                      const PathRaster& raster, std::vector<std::uint8_t>& out) {
    const auto geometry =
        raster.coverageGeometry(window.originX(), window.originY(), window.extent().width(),
                                window.extent().height(), rule, stroke);
    if (!geometry) {
        return false;
    }
    const GpuPathCoverageParameters parameters{window, window, PixelAspectRatio::square()};
    if (producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code !=
        GpuPathCoverageDiagnosticCode::None) {
        return false;
    }
    if (pollToCompletion(producer) != GpuPathCoveragePollResult::Ready) {
        return false;
    }
    const auto readback = producer.readback(1ULL << 34ULL);
    if (!readback.hasValue()) {
        return false;
    }
    out = readback.coverage;
    return true;
}

inline void expectGeometryMatch(Expectations& expectations, GpuPathCoverage& producer,
                                const PathRaster& raster, const ImageWindow& window,
                                const PathFillRule rule, const bool stroke, const char* label) {
    const auto reference = cpuCoverage(raster, window, rule, stroke);
    std::vector<std::uint8_t> measured;
    const bool ran = runCoverage(producer, window, rule, stroke, raster, measured);
    expectations.expect(ran, label);
    if (!ran) {
        return;
    }
    expectations.expect(measured == reference,
                        "the resident coverage is byte-identical to coverageRow");
    expectations.expect(producer.coverageWidth() == window.extent().width() &&
                            producer.coverageHeight() == window.extent().height(),
                        "the resident coverage geometry is published");
}

// Definitions live in the parity and lifecycle translation units.
void testShapeMatrix(Expectations& expectations, GpuPathCoverage& producer);
void testGlyphs(Expectations& expectations, GpuPathCoverage& producer);
void testResidentConsumption(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                             GpuDevice& device);
void testGuards(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid);
void testDeviceIdentity(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                        GpuDevice& device, const std::filesystem::path& loaderPath);
void testFaultLifecycle(Expectations& expectations, GpuDevice& device);
void testResidentPoolBound(Expectations& expectations, GpuDevice& device);
void testResidentPoolOwnerIsolation(Expectations& expectations,
                                    const std::filesystem::path& loaderPath);
void testTwoDimensionalPlan(Expectations& expectations, GpuPathCoverage& producer);
void testPerformance(Expectations& expectations, GpuPathCoverage& producer);

} // namespace bloom::render::gpu_path_coverage_test
