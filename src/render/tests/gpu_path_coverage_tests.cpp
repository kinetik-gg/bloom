// Native GpuPathCoverage test. The CPU oracle is the unchanged
// PathRaster::coverageRow, compared byte-for-byte with the resident mask the GPU
// producer reads back over shapes, transforms, fill rules, strokes, clipping,
// proxies and adversarial sample-on-edge cases. It also proves one resident
// coverage -> GpuSolid covered fill -> native output chain consumes the same
// device buffer (no download/re-upload), reports a positive native dispatch
// counter, and that empty/budget/cancel/refused inputs recover cleanly. Local
// mode pins the explicit loader and requires a device; a hardware-free CI image
// prints an explicit skip and --require-device fails closed.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_path_coverage.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include "gpu_path_coverage_fault.hpp"
#include "shaders/path_coverage_spirv.inc"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::EmbeddedFace;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuPathCoverage;
using bloom::render::GpuPathCoverageDiagnosticCode;
using bloom::render::GpuPathCoverageParameters;
using bloom::render::GpuPathCoveragePollResult;
using bloom::render::GpuPathCoverageReadbackCode;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Path;
using bloom::render::PathBounds;
using bloom::render::PathFillRule;
using bloom::render::PathMatrix;
using bloom::render::PathRaster;
using bloom::render::PathStroke;
using bloom::render::PathStrokeAlign;
using bloom::render::PathStrokeCap;
using bloom::render::PathStrokeJoin;
using bloom::render::Rgba32f;
using bloom::render::TextFont;
using bloom::render::TextLayoutOptions;
using bloom::render::TextRasterParameters;

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

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
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

[[nodiscard]] std::vector<std::uint8_t> cpuCoverage(const PathRaster& raster,
                                                    const ImageWindow& window,
                                                    const PathFillRule rule, const bool stroke) {
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

[[nodiscard]] PathRaster rasterOf(bloom::render::ImageResult<PathRaster> result) {
    return result ? *result.value() : PathRaster{};
}

[[nodiscard]] GpuPathCoveragePollResult pollToCompletion(GpuPathCoverage& coverage) {
    GpuPathCoveragePollResult poll = GpuPathCoveragePollResult::Pending;
    while (poll == GpuPathCoveragePollResult::Pending) {
        poll = coverage.poll();
    }
    return poll;
}

[[nodiscard]] GpuSolidPollResult pollSolidToCompletion(GpuSolid& solid) {
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    return poll;
}

[[nodiscard]] bool runCoverage(GpuPathCoverage& producer, const ImageWindow& window,
                               const PathFillRule rule, const bool stroke, const PathRaster& raster,
                               std::vector<std::uint8_t>& out) {
    const auto geometry = raster.coverageGeometry(window.originX(), window.originY(),
                                                  window.extent().width(),
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

void expectGeometryMatch(Expectations& expectations, GpuPathCoverage& producer,
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

// Pins the checked-in source -> SPIR-V -> embedded-array chain.
void testEmbeddedSpirvDigest(Expectations& expectations) {
    static_assert(bloom::render::vulkan_detail::kPathCoverageSpirvWordCount * 4U ==
                      bloom::render::vulkan_detail::kPathCoverageSpirvByteCount,
                  "SPIR-V word count must exactly cover the byte count");
    const auto* raw =
        reinterpret_cast<const std::byte*>(bloom::render::vulkan_detail::kPathCoverageSpirvCode);
    const std::span<const std::byte> bytes(raw,
                                           bloom::render::vulkan_detail::kPathCoverageSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the embedded GpuPathCoverage SPIR-V hashes");
    if (!digest.has_value()) {
        return;
    }
    const auto hex = digest->toLowercaseHex();
    expectations.expect(std::string_view(hex.data(), hex.size()) == BLOOM_PATH_COVERAGE_SPV_SHA256,
                        "the embedded GpuPathCoverage SPIR-V matches the pinned digest");
    expectations.expect(std::string_view(bloom::render::vulkan_detail::kPathCoverageSpirvDigest) ==
                            BLOOM_PATH_COVERAGE_SPV_SHA256,
                        "the .inc digest comment matches the pinned digest");
}

[[nodiscard]] Path doubleRectangle() {
    auto path = bloom::render::rectanglePath(2, 2);
    const auto anchors = path.anchors;
    path.anchors.insert(path.anchors.end(), anchors.begin(), anchors.end());
    return path;
}

void testShapeMatrix(Expectations& expectations, GpuPathCoverage& producer) {
    const auto window = ImageWindow::create(-2, -2, 24, 16);
    const auto small = ImageWindow::create(0, 0, 4, 4);
    const auto wide = ImageWindow::create(0, 0, 20, 20);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(small) &&
                            static_cast<bool>(wide),
                        "the coverage windows build");
    if (!window || !small || !wide) {
        return;
    }
    const auto& w = *window.value();
    const auto& s = *small.value();
    const auto& d = *wide.value();
    const double c = std::sqrt(0.5);

    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(20.0, 12.0)}, {},
                                 PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1)
             ),
        w, PathFillRule::NonZero, false, "rect+translate coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::create(bloom::render::rectanglePath(18.0, 12.0, 3.5), {}, 1, 1)), w,
        PathFillRule::NonZero, false, "rounded rect coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::ellipsePath(20.0, 12.0), {}, 1, 1)
                             ),
                        w, PathFillRule::NonZero, false, "ellipse coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::polygonPath(20.0, 12.0, 3), {}, 1, 1)
                             ),
                        w, PathFillRule::NonZero, false, "triangle coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::create(bloom::render::polygonPath(20.0, 12.0, 5, 2.0), {}, 1, 1)), w,
        PathFillRule::NonZero, false, "rounded polygon coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::starPath(20.0, 12.0, 5, 0.5), {}, 1, 1)
                             ),
                        w, PathFillRule::NonZero, false, "star coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::starPath(20.0, 12.0, 5, 0.5), {}, 1, 1)
                             ),
                        w, PathFillRule::EvenOdd, false, "star even-odd coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(doubleRectangle(), {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "nonzero double contour coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(doubleRectangle(), {}, 1, 1)), w,
                        PathFillRule::EvenOdd, false, "evenodd double contour coverage");
    const auto rotatedWindow = ImageWindow::create(0, 0, 16, 12);
    expectations.expect(static_cast<bool>(rotatedWindow), "the rotated window builds");
    if (rotatedWindow) {
        expectGeometryMatch(
            expectations, producer,
            rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(8.0, 8.0)}, {},
                                             PathMatrix{c, -c, c, c, 8.1, 0}, 1, 1)),
            *rotatedWindow.value(), PathFillRule::NonZero, false, "rotated coverage");
    }
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::rectanglePath(4.0, 4.0), {}, 0.5, 0.5)
                             ),
                        d, PathFillRule::NonZero, false, "proxy coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(8.0, 8.0)}, {},
                                 PathMatrix{1.7, 0.3, -0.2, 1.3, 0.25, 0.75}, 2.0, 1.5)
             ),
        d, PathFillRule::NonZero, false, "nonuniform scale coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(20.0, 12.0)}, {},
                                 PathMatrix{1, 0, 0, 1, 0.3, 0.3}, 1, 1, {},
                                 PathBounds{4.0, 3.0, 16.0, 10.0})
             ),
        w, PathFillRule::NonZero, false, "clipped coverage");
    expectGeometryMatch(expectations, producer,
                        rasterOf(PathRaster::create(bloom::render::rectanglePath(3.0, 3.0), {}, 1, 1)
                             ),
                        s, PathFillRule::NonZero, false, "edge-on-sample coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(2.0, 2.0)}, {},
                                 PathMatrix{1, 0, 0, 1, 0.125, 0.125}, 1, 1)
             ),
        s, PathFillRule::NonZero, false, "edge-shift-0.125 coverage");
    expectGeometryMatch(
        expectations, producer,
        rasterOf(PathRaster::transformed(std::array{bloom::render::rectanglePath(2.0, 2.0)}, {},
                                 PathMatrix{1, 0, 0, 1, 0.875, 0.875}, 1, 1)
             ),
        s, PathFillRule::NonZero, false, "edge-shift-0.875 coverage");

    for (auto align : {PathStrokeAlign::Center, PathStrokeAlign::Inside, PathStrokeAlign::Outside}) {
        for (auto join : {PathStrokeJoin::Miter, PathStrokeJoin::Round, PathStrokeJoin::Bevel}) {
            const PathStroke stroke{2.0, align, join, PathStrokeCap::Butt};
            expectGeometryMatch(expectations, producer,
                                rasterOf(PathRaster::create(bloom::render::rectanglePath(16.0, 10.0),
                                                    stroke, 1, 1)
                                     ),
                                w, PathFillRule::NonZero, true, "stroke rect coverage");
        }
    }
    const auto capWindow = ImageWindow::create(0, 0, 6, 6);
    expectations.expect(static_cast<bool>(capWindow), "the cap window builds");
    for (auto cap : {PathStrokeCap::Butt, PathStrokeCap::Round, PathStrokeCap::Square}) {
        if (!capWindow) {
            break;
        }
        expectGeometryMatch(expectations, producer,
                            rasterOf(PathRaster::create(bloom::render::linePath({1, 1}, {4, 1}),
                                                        {1.0, PathStrokeAlign::Outside,
                                                         PathStrokeJoin::Miter, cap},
                                                        1, 1)),
                            *capWindow.value(), PathFillRule::NonZero, true,
                            "stroke line cap coverage");
    }

    // Empty geometry: an anchorless path yields a zero mask.
    expectGeometryMatch(expectations, producer, rasterOf(PathRaster::create(Path{}, {}, 1, 1)), w,
                        PathFillRule::NonZero, false, "empty coverage");

    // Non-integer PAR metadata is preserved on the resident parameters.
    const auto par = PixelAspectRatio::create(4, 3);
    const auto parDisplay = ImageWindow::create(-5, 3, 12, 8);
    expectations.expect(par.has_value() && static_cast<bool>(parDisplay),
                        "the non-square pixel aspect and display window build");
    if (par.has_value() && parDisplay) {
        const auto parRaster =
            PathRaster::create(bloom::render::rectanglePath(6.0, 6.0), {}, 1, 1);
        expectations.expect(static_cast<bool>(parRaster), "the PAR coverage raster builds");
        if (!parRaster) {
            return;
        }
        const auto geometry = parRaster.value()->coverageGeometry(
            w.originX(), w.originY(), w.extent().width(), w.extent().height(),
            PathFillRule::NonZero, false);
        expectations.expect(static_cast<bool>(geometry), "the PAR coverage geometry builds");
        if (geometry) {
            const GpuPathCoverageParameters parameters{w, *parDisplay.value(), par.value()};
            const auto began = producer.begin(parameters, *geometry.value(), 1ULL << 34ULL);
            expectations.expect(began.code == GpuPathCoverageDiagnosticCode::None,
                                "the PAR coverage job is accepted");
            if (began.code == GpuPathCoverageDiagnosticCode::None) {
                expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Ready,
                                    "the PAR coverage job completes");
            }
        }
    }
}

// Glyph contours with counters (holes), rendered by the GPU producer.
void testGlyphs(Expectations& expectations, GpuPathCoverage& producer) {
    const auto parameters = TextRasterParameters::create(40.0, 40.0);
    expectations.expect(static_cast<bool>(parameters), "the glyph parameters build");
    if (!parameters) {
        return;
    }
    const auto outlines = textOutlines(TextFont{EmbeddedFace::DejaVuSans}, "Bo8ge",
                                       *parameters.value(), TextLayoutOptions{});
    expectations.expect(static_cast<bool>(outlines) && !outlines.value()->empty(),
                        "the glyph outlines build");
    if (!outlines || outlines.value()->empty()) {
        return;
    }
    const auto raster = PathRaster::transformed(std::span<const Path>(*outlines.value()), {},
                                                PathMatrix{1, 0, 0, 1, 0, 0}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "the glyph raster builds");
    if (!raster) {
        return;
    }
    const auto bounds = raster.value()->bounds(true, false);
    const auto x0 = static_cast<std::int64_t>(std::floor(bounds.left)) - 1;
    const auto y0 = static_cast<std::int64_t>(std::floor(bounds.top)) - 1;
    const auto width = static_cast<std::uint32_t>(
                           std::ceil(bounds.right - static_cast<double>(x0))) + 2U;
    const auto height = static_cast<std::uint32_t>(
                            std::ceil(bounds.bottom - static_cast<double>(y0))) + 2U;
    const auto window = ImageWindow::create(x0, y0, width, height);
    expectations.expect(static_cast<bool>(window), "the glyph window builds");
    if (!window) {
        return;
    }
    expectGeometryMatch(expectations, producer, *raster.value(), *window.value(),
                        PathFillRule::NonZero, false, "glyph contour coverage");
    expectGeometryMatch(expectations, producer, *raster.value(), *window.value(),
                        PathFillRule::EvenOdd, false, "glyph contour even-odd coverage");
}

void testResidentConsumption(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                             GpuDevice& device) {
    const auto window = ImageWindow::create(-2, -2, 20, 14);
    expectations.expect(static_cast<bool>(window), "the resident chain window builds");
    if (!window) {
        return;
    }
    const auto& w = *window.value();
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.2, 0.6, 0.9, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the resident chain pixel builds");
    if (!pixel) {
        return;
    }
    auto raster = PathRaster::transformed(
        std::array{bloom::render::rectanglePath(18.0, 12.0)}, {}, PathMatrix{1, 0, 0, 1, 0.3, 0.3},
        1, 1);
    expectations.expect(static_cast<bool>(raster), "the resident chain raster builds");
    if (!raster) {
        return;
    }
    const auto coverage = cpuCoverage(*raster.value(), w, PathFillRule::NonZero, false);
    const std::uint64_t before = GpuPathCoverage::nativeDispatchCount();
    std::vector<std::uint8_t> measured;
    expectations.expect(runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(),
                                    measured),
                        "the resident coverage producer completes");
    expectations.expect(GpuPathCoverage::nativeDispatchCount() > before,
                        "a native coverage dispatch was counted");
    expectations.expect(producer.isBoundTo(device), "the producer is bound to its device");

    const float opacity = 0.6F;
    const GpuSolidParameters base{*pixel.value(), w, w, PixelAspectRatio::square()};
    const auto began = solid.beginCoveredResident(base, producer, opacity, 1ULL << 34ULL);
    expectations.expect(began.code == GpuSolidDiagnosticCode::None,
                        "the resident covered fill is accepted");
    if (began.code != GpuSolidDiagnosticCode::None) {
        return;
    }
    expectations.expect(pollSolidToCompletion(solid) == GpuSolidPollResult::Ready,
                        "the resident covered fill completes");
    const auto readback = solid.readback();
    expectations.expect(readback.hasValue(), "the resident covered fill reads back");
    if (!readback.hasValue()) {
        return;
    }
    // CPU oracle: coverageRow then coverageSolidRow plus the separate Float32 opacity.
    std::vector<Rgba32f> reference(static_cast<std::size_t>(w.extent().width()) *
                                       w.extent().height(),
                                   Rgba32f::transparent());
    const std::uint32_t width = w.extent().width();
    const std::uint32_t height = w.extent().height();
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto offset = static_cast<std::size_t>(row) * width;
        const auto rowCoverage = std::span<const std::uint8_t>(coverage.data() + offset, width);
        const auto rowOutput = std::span<Rgba32f>(reference.data() + offset, width);
        expectations.expect(!bloom::render::coverageSolidRow(rowCoverage, *pixel.value(), rowOutput),
                            "the CPU covered oracle runs");
        for (auto& value : rowOutput) {
            const auto faded = Rgba32f::fromPremultiplied(
                value.red() * opacity, value.green() * opacity, value.blue() * opacity,
                value.alpha() * opacity);
            value = *faded.value();
        }
    }
    expectations.expect(readback.pixels == reference,
                        "the resident coverage -> GpuSolid chain matches the CPU oracle");
}

void testGuards(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid) {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    expectations.expect(static_cast<bool>(window), "the guard window builds");
    if (!window) {
        return;
    }
    const auto& w = *window.value();
    const auto raster =
        PathRaster::create(bloom::render::rectanglePath(6.0, 3.0), {}, 1, 1);
    if (!raster) {
        expectations.expect(false, "the guard raster builds");
        return;
    }
    const auto geometry = raster.value()->coverageGeometry(w.originX(), w.originY(),
                                                           w.extent().width(),
                                                           w.extent().height(),
                                                           PathFillRule::NonZero, false);
    expectations.expect(static_cast<bool>(geometry), "the guard geometry builds");
    if (!geometry) {
        return;
    }
    const GpuPathCoverageParameters parameters{w, w, PixelAspectRatio::square()};

    // Mismatched window geometry is refused.
    const auto mismatchWindow = ImageWindow::create(0, 0, 9, 4);
    expectations.expect(static_cast<bool>(mismatchWindow), "the mismatched window builds");
    if (mismatchWindow) {
        const auto mismatch =
            producer.begin(GpuPathCoverageParameters{*mismatchWindow.value(), w,
                                                     PixelAspectRatio::square()},
                           *geometry.value(), 1ULL << 34ULL);
        expectations.expect(mismatch.code == GpuPathCoverageDiagnosticCode::InvalidArgument,
                            "a mismatched data window is rejected");
    }

    // A budget below the retained mask is refused and leaves the producer reusable.
    const auto tight = producer.begin(parameters, *geometry.value(), 4);
    expectations.expect(tight.code == GpuPathCoverageDiagnosticCode::OverBudget,
                        "an under-budget coverage request is rejected");

    // Consuming a non-Ready producer is refused.
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.5, 0.5, 0.5, 1.0});
    const auto notReady =
        solid.beginCoveredResident(GpuSolidParameters{*pixel.value(), w, w,
                                                      PixelAspectRatio::square()},
                                   producer, 0.5F, 1ULL << 34ULL);
    expectations.expect(notReady.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a non-Ready resident coverage is rejected");

    // Cancel publishes nothing and the producer recovers for the next job.
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the cancellable coverage begin is accepted");
    producer.cancel();
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code == GpuPathCoverageDiagnosticCode::Cancelled,
                        "a cancelled coverage job reports Cancelled");
    expectations.expect(producer.coverageWidth() == 0, "no coverage is published after cancel");

    // Reuse after cancel produces the exact mask.
    std::vector<std::uint8_t> measured;
    expectations.expect(runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(),
                                    measured),
                        "the producer recovers after cancellation");
    expectations.expect(measured == cpuCoverage(*raster.value(), w, PathFillRule::NonZero, false),
                        "the recovered coverage is exact");

    // Wrong-thread poll fails closed.
    auto foreignPoll = GpuPathCoveragePollResult::Ready;
    std::thread worker([&producer, &foreignPoll]() { foreignPoll = producer.poll(); });
    worker.join();
    expectations.expect(foreignPoll == GpuPathCoveragePollResult::WrongThread,
                        "a poll from a joined non-owner thread is WrongThread");
}

// Device-identity and wrong-thread hardening for the resident consume path.
void testDeviceIdentity(Expectations& expectations, GpuPathCoverage& producer, GpuSolid& solid,
                        GpuDevice& device, const std::filesystem::path& loaderPath) {
    const auto window = ImageWindow::create(-2, -2, 8, 4);
    const auto raster = PathRaster::create(bloom::render::rectanglePath(6.0, 3.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the identity window and raster build");
    if (!window || !raster) {
        return;
    }
    std::vector<std::uint8_t> measured;
    expectations.expect(producer.isBoundTo(device), "the identity producer is bound to its device");
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the identity coverage producer runs");
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.4, 0.5, 0.6, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the identity pixel builds");
    if (!pixel) {
        return;
    }
    const GpuSolidParameters base{*pixel.value(), *window.value(), *window.value(),
                                  PixelAspectRatio::square()};

    // Wrong-thread consume fails closed before reading the producer's state.
    auto foreign = GpuSolidDiagnosticCode::None;
    std::thread worker([&solid, &base, &producer, &foreign]() {
        foreign = solid.beginCoveredResident(base, producer, 0.5F, 1ULL << 34ULL).code;
    });
    worker.join();
    expectations.expect(foreign == GpuSolidDiagnosticCode::WrongThread,
                        "a wrong-thread resident consume is WrongThread");

    // A foreign device's resident coverage must be rejected by identity, not dimensions.
    GpuDeviceCreationOptions secondOptions;
    secondOptions.loader_path = loaderPath;
    auto second = GpuDevice::create(secondOptions);
    if (!second) {
        return;
    }
    auto secondProducer = GpuPathCoverage::create(*second.device);
    expectations.expect(secondProducer.hasValue(), "the second-device producer is created");
    if (!secondProducer) {
        return;
    }
    std::vector<std::uint8_t> secondMask;
    expectations.expect(runCoverage(*secondProducer.coverage, *window.value(),
                                    PathFillRule::NonZero, false, *raster.value(), secondMask),
                        "the second-device coverage runs");
    const auto foreignBegin =
        solid.beginCoveredResident(base, *secondProducer.coverage, 0.5F, 1ULL << 34ULL);
    expectations.expect(foreignBegin.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a foreign-device resident coverage is rejected by identity");
}

// Bounded-reservation lifecycle: forced submit failure, forced fence timeout with quarantine and
// retirement, and forced device loss, each followed by reuse.
void testFaultLifecycle(Expectations& expectations, GpuDevice& device) {
    using namespace bloom::render::path_coverage_detail;
    auto created = GpuPathCoverage::create(device);
    expectations.expect(created.hasValue(), "the fault-lifecycle producer is created");
    if (!created) {
        return;
    }
    GpuPathCoverage& producer = *created.coverage;
    const auto window = ImageWindow::create(0, 0, 16, 8);
    const auto raster = PathRaster::create(bloom::render::rectanglePath(14.0, 6.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the fault window and raster build");
    if (!window || !raster) {
        return;
    }
    const auto geometry = raster.value()->coverageGeometry(
        window.value()->originX(), window.value()->originY(), window.value()->extent().width(),
        window.value()->extent().height(), PathFillRule::NonZero, false);
    expectations.expect(static_cast<bool>(geometry), "the fault geometry builds");
    if (!geometry) {
        return;
    }
    const GpuPathCoverageParameters parameters{*window.value(), *window.value(),
                                               PixelAspectRatio::square()};
    std::vector<std::uint8_t> measured;

    // Forced submit failure happens before any in-flight submission: the claim is released and the
    // producer is reusable.
    setPathCoverageFaultForTest(PathCoverageFault::FailSubmit);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "a forced submit failure is reported");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "a forced submit failure does not quarantine");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the producer is reusable after a forced submit failure");

    // Forced fence timeout: the exact submission is quarantined, admission is refused, then the
    // owner retires it and the producer is reusable.
    setPathCoverageFaultForTest(PathCoverageFault::ForceFenceTimeout);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the fence-timeout begin is accepted");
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code ==
                                GpuPathCoverageDiagnosticCode::NativeTimeout,
                        "a forced fence timeout reports NativeTimeout");
    expectations.expect(pathCoverageQuarantineOccupiedForTest(), "the quarantine is occupied");
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceUnavailable,
                        "admission is refused while the quarantine is occupied");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    bool retired = false;
    const auto retireDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retired && std::chrono::steady_clock::now() < retireDeadline) {
        retired = retirePathCoverageQuarantineForTest();
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(retired, "the owner retires the quarantine");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "the quarantine is free after retirement");
    expectations.expect(runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                    *raster.value(), measured),
                        "the producer is reusable after quarantine retirement");

    // Forced device loss: released without quarantine and latched on this instance.
    setPathCoverageFaultForTest(PathCoverageFault::ForceDeviceLost);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::None,
                        "the device-loss begin is accepted");
    expectations.expect(pollToCompletion(producer) == GpuPathCoveragePollResult::Failure &&
                            producer.diagnostic().code == GpuPathCoverageDiagnosticCode::DeviceLost,
                        "a forced device loss is reported");
    expectations.expect(!pathCoverageQuarantineOccupiedForTest(),
                        "a device loss does not quarantine");
    setPathCoverageFaultForTest(PathCoverageFault::None);
    expectations.expect(producer.begin(parameters, *geometry.value(), 1ULL << 34ULL).code ==
                            GpuPathCoverageDiagnosticCode::DeviceLost,
                        "a lost instance refuses reuse");
    auto fresh = GpuPathCoverage::create(device);
    expectations.expect(fresh.hasValue(), "a fresh producer is created after device loss");
    if (fresh) {
        expectations.expect(runCoverage(*fresh.coverage, *window.value(), PathFillRule::NonZero,
                                        false, *raster.value(), measured),
                            "a fresh producer recovers after device loss");
    }
}

// Injected small X workgroup limit: the flattened 2D grid must still plan and execute a
// capacity-valid geometry that a 1D grid would have refused.
void testTwoDimensionalPlan(Expectations& expectations, GpuPathCoverage& producer) {
    using namespace bloom::render::path_coverage_detail;
    const auto window = ImageWindow::create(0, 0, 80, 64);
    const auto raster = PathRaster::create(bloom::render::rectanglePath(70.0, 54.0), {}, 1, 1);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(raster),
                        "the 2D plan window and raster build");
    if (!window || !raster) {
        return;
    }
    const auto reference =
        cpuCoverage(*raster.value(), *window.value(), PathFillRule::NonZero, false);
    setPathCoverageMaxWorkGroupCountXForTest(3);
    setPathCoverageMaxWorkGroupCountYForTest(4);
    std::vector<std::uint8_t> measured;
    const bool ran = runCoverage(producer, *window.value(), PathFillRule::NonZero, false,
                                 *raster.value(), measured);
    setPathCoverageMaxWorkGroupCountXForTest(0);
    setPathCoverageMaxWorkGroupCountYForTest(0);
    expectations.expect(ran, "the 2D-flattened coverage plan runs under an injected small maxX");
    expectations.expect(measured == reference, "the 2D-flattened coverage is byte-exact");
}

void testPerformance(Expectations& expectations, GpuPathCoverage& producer) {
    struct Size final {
        std::uint32_t width;
        std::uint32_t height;
    };
    const std::array<Size, 3> sizes{{{1280, 720}, {1920, 1080}, {3840, 2160}}};
    auto raster = PathRaster::create(bloom::render::starPath(900.0, 500.0, 12, 0.45), {}, 1, 1);
    expectations.expect(static_cast<bool>(raster), "the performance raster builds");
    if (!raster) {
        return;
    }
    std::cout << "path coverage performance (ms; CPU coverageRow vs GPU geometry + dispatch + "
                 "readback)\n";
    for (const auto size : sizes) {
        const auto window = ImageWindow::create(0, 0, size.width, size.height);
        if (!window) {
            expectations.expect(false, "the performance window builds");
            continue;
        }
        const auto& w = *window.value();
        std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(size.width) * size.height, 0);
        const auto cpuRun = [&]() {
            for (std::uint32_t row = 0; row < size.height; ++row) {
                const auto offset = static_cast<std::size_t>(row) * size.width;
                (void)raster.value()->coverageRow(
                    w.originX(), w.originY() + row,
                    std::span<std::uint8_t>(cpuBytes.data() + offset, size.width),
                    PathFillRule::NonZero, false);
            }
        };
        const auto gpuRun = [&]() {
            std::vector<std::uint8_t> measured;
            if (!runCoverage(producer, w, PathFillRule::NonZero, false, *raster.value(), measured)) {
                expectations.expect(false, "the performance coverage job runs");
            }
        };
        for (int warmup = 0; warmup < 3; ++warmup) {
            cpuRun();
            gpuRun();
        }
        std::vector<double> cpuSamples;
        std::vector<double> gpuSamples;
        for (int sample = 0; sample < 7; ++sample) {
            const auto cpuStart = std::chrono::steady_clock::now();
            cpuRun();
            const auto cpuStop = std::chrono::steady_clock::now();
            const auto gpuStart = std::chrono::steady_clock::now();
            gpuRun();
            const auto gpuStop = std::chrono::steady_clock::now();
            cpuSamples.push_back(
                std::chrono::duration<double, std::milli>(cpuStop - cpuStart).count());
            gpuSamples.push_back(
                std::chrono::duration<double, std::milli>(gpuStop - gpuStart).count());
        }
        std::sort(cpuSamples.begin(), cpuSamples.end());
        std::sort(gpuSamples.begin(), gpuSamples.end());
        std::cout << "  " << size.width << 'x' << size.height << ": cpu " << cpuSamples[3]
                  << " ms, gpu " << gpuSamples[3] << " ms\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        testEmbeddedSpirvDigest(expectations);

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.ok() ? 0 : 1;
        }

        auto produced = GpuPathCoverage::create(*device.device);
        expectations.expect(produced.hasValue(), "the GpuPathCoverage pipeline is created");
        if (!produced) {
            std::cerr << "FAIL: GpuPathCoverage create failed: "
                      << produced.diagnostic.message << '\n';
            return 1;
        }
        auto created = GpuSolid::create(*device.device);
        expectations.expect(created.hasValue(), "the GpuSolid pipeline is created");
        if (!created) {
            std::cerr << "FAIL: GpuSolid create failed: " << created.diagnostic.message << '\n';
            return 1;
        }

        testShapeMatrix(expectations, *produced.coverage);
        testGlyphs(expectations, *produced.coverage);
        testResidentConsumption(expectations, *produced.coverage, *created.solid, *device.device);
        testGuards(expectations, *produced.coverage, *created.solid);
        testDeviceIdentity(expectations, *produced.coverage, *created.solid, *device.device,
                           options.loader_path);
        testFaultLifecycle(expectations, *device.device);
        testTwoDimensionalPlan(expectations, *produced.coverage);
        testPerformance(expectations, *produced.coverage);

        if (!expectations.ok()) {
            std::cerr << "FAIL: GpuPathCoverage native checks failed\n";
            return 1;
        }
        std::cout << "PASS: GpuPathCoverage native checks\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
