#pragma once

// Shared helpers and fixtures for the GpuComposite native proof. Split out so each translation
// unit stays under the source-size budget.

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::render::composite_proof {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuComposite;
using bloom::render::GpuCompositeDiagnosticCode;
using bloom::render::GpuCompositeJobState;
using bloom::render::GpuCompositePollResult;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;

inline constexpr double kTolerance = 2e-6;
inline constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
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

// The fixed fixture literals below are always valid; a rejected value means the fixture itself is
// broken. Fail the test with a diagnostic instead of returning an empty optional that a caller
// could dereference unchecked.
[[nodiscard]] inline ImageWindow window(const std::int64_t x, const std::int64_t y,
                                        const std::uint64_t w, const std::uint64_t h) {
    const auto result = ImageWindow::create(x, y, w, h);
    if (!result) {
        throw std::logic_error("invalid composite-proof fixture window");
    }
    return *result.value();
}

[[nodiscard]] inline Rgba32f pixel(const float r, const float g, const float b, const float a) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!result) {
        throw std::logic_error("invalid composite-proof fixture pixel");
    }
    return *result.value();
}

[[nodiscard]] inline std::optional<Rgba32fImage> makeImage(const ImageWindow dataWindow,
                                                           const ImageWindow displayWindow,
                                                           const PixelAspectRatio aspect,
                                                           const std::vector<Rgba32f>& pixels) {
    const auto descriptor = Rgba32fImageDescriptor::create(dataWindow, displayWindow, aspect);
    if (!descriptor || pixels.size() != descriptor.value()->layout().pixelCount) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return std::nullopt;
    }
    const auto width = dataWindow.extent().width();
    for (std::uint32_t y = 0; y < dataWindow.extent().height(); ++y) {
        const auto row = builder.value()->row(dataWindow.originY() + y);
        if (!row) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return frozen ? std::optional(std::move(*frozen.value())) : std::nullopt;
}

[[nodiscard]] inline std::optional<GpuImage> upload(GpuImageUpload& uploader,
                                                    std::shared_ptr<const Rgba32fImage> source) {
    if (uploader.begin({std::move(source)}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
        return std::nullopt;
    }
    GpuImageUploadPollResult poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    return poll == GpuImageUploadPollResult::Ready ? std::optional(uploader.takeImage())
                                                   : std::nullopt;
}

[[nodiscard]] inline std::optional<GpuImage> solid(GpuSolid& solidOp, const Color4d color,
                                                   const ImageWindow dataWindow,
                                                   const ImageWindow displayWindow,
                                                   const PixelAspectRatio aspect) {
    const auto value = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
    if (!value) {
        return std::nullopt;
    }
    if (solidOp.begin({*value.value(), dataWindow, displayWindow, aspect}, kBudget).code !=
        GpuSolidDiagnosticCode::None) {
        return std::nullopt;
    }
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solidOp.poll();
    }
    return poll == GpuSolidPollResult::Ready ? std::optional(solidOp.takeImage()) : std::nullopt;
}

[[nodiscard]] inline bool closeEnough(const float actual, const float expected) {
    if (actual == expected) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        return false;
    }
    const auto absolute = std::fabs(static_cast<double>(actual) - static_cast<double>(expected));
    const auto magnitude =
        std::max(std::fabs(static_cast<double>(actual)), std::fabs(static_cast<double>(expected)));
    return absolute <= kTolerance || absolute <= kTolerance * magnitude;
}

[[nodiscard]] inline bool pixelsMatch(const std::vector<Rgba32f>& gpu,
                                      const std::span<const Rgba32f> cpu,
                                      Expectations& expectations, const std::string& label) {
    if (gpu.size() != cpu.size()) {
        expectations.expect(false, label + ": pixel counts agree");
        return false;
    }
    for (std::size_t index = 0; index < gpu.size(); ++index) {
        const auto& a = gpu[index];
        const auto& b = cpu[index];
        if (closeEnough(a.red(), b.red()) && closeEnough(a.green(), b.green()) &&
            closeEnough(a.blue(), b.blue()) && closeEnough(a.alpha(), b.alpha())) {
            continue;
        }
        expectations.expect(false, label + ": pixel " + std::to_string(index) + " within 2e-6");
        return false;
    }
    expectations.expect(true, label + ": every component within 2e-6 abs-or-rel");
    return true;
}

[[nodiscard]] inline std::vector<Rgba32f> checker(const std::uint32_t width,
                                                  const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] =
                (x % 2 == 0) ? pixel(1.0F, 1.0F, 1.0F, 1.0F) : pixel(0.0F, 0.0F, 0.0F, 1.0F);
        }
    }
    return pixels;
}

[[nodiscard]] inline std::vector<Rgba32f> semanticPixels(const std::uint32_t width,
                                                         const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto fx = static_cast<float>(x) / static_cast<float>(width);
            const auto fy = static_cast<float>(y) / static_cast<float>(height);
            pixels[static_cast<std::size_t>(y) * width + x] =
                pixel(2.0F * fx, -0.5F + fy, 0.25F + 3.0F * fx, fx * fy);
        }
    }
    return pixels;
}

struct TranslationCase final {
    std::string name;
    std::uint32_t width;
    std::uint32_t height;
    std::int64_t outputOriginX;
    std::int64_t outputOriginY;
    std::int64_t sourceOriginX;
    std::int64_t sourceOriginY;
    double translationX;
    double translationY;
    float opacity;
};

struct SourceOverCase final {
    std::string name;
    std::uint32_t width;
    std::uint32_t height;
    std::int64_t sourceOriginX;
    std::int64_t sourceOriginY;
    std::int64_t destOriginX;
    std::int64_t destOriginY;
    bool semantic;
};

} // namespace bloom::render::composite_proof
