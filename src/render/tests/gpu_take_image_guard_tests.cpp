// Focused regression test for the F1 guard: a resident-producing operation may only move its
// image out once the job is Ready. Immediately after begin() the job is deterministically Pending
// (logical state, independent of how fast the hardware finished), so takeImage() must refuse and
// leave the submission and its buffers untouched; the same job must still reach Ready through
// poll(), after which takeImage() succeeds. Idle and a double take are safe no-ops.
//
// Scope: GpuImageUpload (staging buffer), GpuSolid::begin (resident image) and
// GpuSolid::beginCovered (mask/palette buffers), GpuResidentDisplay (input/output/status buffers).
// The CPU-only stub build cannot reach Ready; --require-device fails closed instead of skipping.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadJobState;
using bloom::render::GpuImageUploadParameters;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayJobState;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidJobState;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;

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
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<Rgba32f> makePixel(const std::uint32_t x, const std::uint32_t y) {
    const float r = static_cast<float>(x) * 0.25F - 0.25F;
    const float g = static_cast<float>(y) * 0.5F;
    const float b = static_cast<float>(x + y) * 0.125F;
    const float a = (x % 2U == 0U) ? 1.0F : 0.5F;
    const auto pixel = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!pixel) {
        return std::nullopt;
    }
    return *pixel.value();
}

[[nodiscard]] std::optional<Rgba32fImage> buildFixture(const ImageWindow dataWindow) {
    const auto descriptor =
        Rgba32fImageDescriptor::create(dataWindow, dataWindow, PixelAspectRatio::square());
    if (!descriptor) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builder) {
        return std::nullopt;
    }
    const auto width = dataWindow.extent().width();
    for (std::int64_t y = dataWindow.originY(); y < dataWindow.maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        if (!row || row.value()->size() != width) {
            return std::nullopt;
        }
        auto pixels = *row.value();
        for (std::uint32_t i = 0; i < width; ++i) {
            auto pixel = makePixel(i, static_cast<std::uint32_t>(y - dataWindow.originY()));
            if (!pixel) {
                return std::nullopt;
            }
            pixels[i] = *pixel;
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

template <typename Op, typename Poll> [[nodiscard]] Poll pollUntilTerminal(Op& op) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
        const Poll result = op.poll();
        if (result != Poll::Pending) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return Poll::Pending;
}

// Proves the guard on one already-created pipeline: a Ready take succeeds, a second (Idle) take is
// refused, and a take immediately after begin is refused while leaving the job pollable.
template <typename Op, typename Poll, typename TakeResult>
void checkTakeGuard(Expectations& expectations, Op& op, const std::string_view label,
                    const bool beganPending) {
    if (!beganPending) {
        return;
    }
    expectations.expect(op.state() == decltype(op.state())::Pending,
                        std::string(label) + ": begin leaves the job Pending before any poll");
    const TakeResult refused = op.takeImage();
    expectations.expect(!refused.isValid(),
                        std::string(label) + ": takeImage before Ready is refused");
    expectations.expect(op.hasUnretiredSubmission(),
                        std::string(label) + ": refusal preserves the unretired submission");
    const Poll ready = pollUntilTerminal<Op, Poll>(op);
    expectations.expect(ready == Poll::Ready,
                        std::string(label) + ": the refused job still reaches Ready");
    if (ready != Poll::Ready) {
        return;
    }
    TakeResult taken = op.takeImage();
    expectations.expect(taken.isValid(), std::string(label) + ": takeImage at Ready succeeds");
    const TakeResult again = op.takeImage();
    expectations.expect(!again.isValid(),
                        std::string(label) + ": a second take from Idle is refused");
}

void testSolidGuard(Expectations& expectations, GpuDevice& device) {
    auto created = GpuSolid::create(device);
    expectations.expect(created.hasValue(), "solid: pipeline created");
    if (!created) {
        return;
    }
    GpuSolid& solid = *created.solid;
    expectations.expect(!solid.takeImage().isValid(), "solid: Idle takeImage is refused");

    const auto window = ImageWindow::create(-1, 2, 7, 3);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.25, 0.5, 0.75, 1.0});
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(pixel),
                        "solid: fixture builds");
    if (!window || !pixel) {
        return;
    }
    const GpuSolidParameters parameters{*pixel.value(), *window.value(), *window.value(),
                                        PixelAspectRatio::square()};
    const auto began = solid.begin(parameters, 1ULL << 32ULL);
    expectations.expect(began.code == GpuSolidDiagnosticCode::None, "solid: begin accepted");
    checkTakeGuard<GpuSolid, GpuSolidPollResult, GpuImage>(
        expectations, solid, "solid", began.code == GpuSolidDiagnosticCode::None);
}

void testCoveredSolidGuard(Expectations& expectations, GpuDevice& device) {
    auto created = GpuSolid::create(device);
    expectations.expect(created.hasValue(), "covered: pipeline created");
    if (!created) {
        return;
    }
    GpuSolid& solid = *created.solid;
    const auto window = ImageWindow::create(0, 0, 7, 3);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.1, 0.2, 0.3, 1.0});
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(pixel),
                        "covered: fixture builds");
    if (!window || !pixel) {
        return;
    }
    std::vector<std::uint8_t> coverage(7U * 3U);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        coverage[i] = static_cast<std::uint8_t>(i * 255U / (coverage.size() - 1U));
    }
    const GpuSolidParameters base{*pixel.value(), *window.value(), *window.value(),
                                  PixelAspectRatio::square()};
    const auto began = solid.beginCovered(base, coverage, 0.75F, 1ULL << 32ULL);
    if (began.code == GpuSolidDiagnosticCode::Unsupported) {
        std::cout << "SKIP: covered pipeline unsupported on this device: " << began.message << '\n';
        return;
    }
    expectations.expect(began.code == GpuSolidDiagnosticCode::None, "covered: begin accepted");
    checkTakeGuard<GpuSolid, GpuSolidPollResult, GpuImage>(
        expectations, solid, "covered", began.code == GpuSolidDiagnosticCode::None);
}

void testUploadGuard(Expectations& expectations, GpuDevice& device) {
    auto created = GpuImageUpload::create(device);
    expectations.expect(created.hasValue(), "upload: pipeline created");
    if (!created) {
        return;
    }
    GpuImageUpload& upload = *created.upload;
    expectations.expect(!upload.takeImage().isValid(), "upload: Idle takeImage is refused");

    const auto window = ImageWindow::create(0, 0, 5, 3);
    auto fixture = window ? buildFixture(*window.value()) : std::nullopt;
    expectations.expect(fixture.has_value(), "upload: fixture builds");
    if (!fixture) {
        return;
    }
    GpuImageUploadParameters parameters;
    parameters.source = std::make_shared<const Rgba32fImage>(std::move(*fixture));
    const auto began = upload.begin(parameters, 1ULL << 32ULL);
    expectations.expect(began.code == GpuImageUploadDiagnosticCode::None, "upload: begin accepted");
    checkTakeGuard<GpuImageUpload, GpuImageUploadPollResult, GpuImage>(
        expectations, upload, "upload", began.code == GpuImageUploadDiagnosticCode::None);
}

void testResidentDisplayGuard(Expectations& expectations, GpuDevice& device) {
    auto solidCreated = GpuSolid::create(device);
    auto displayCreated = GpuResidentDisplay::create(device);
    expectations.expect(solidCreated.hasValue() && displayCreated.hasValue(),
                        "display: pipelines created");
    if (!solidCreated || !displayCreated) {
        return;
    }
    GpuResidentDisplay& display = *displayCreated.display;
    expectations.expect(!display.takeImage().isValid(), "display: Idle takeImage is refused");

    const auto window = ImageWindow::create(0, 0, 8, 4);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.4, 0.6, 0.2, 1.0});
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(pixel),
                        "display: fixture builds");
    if (!window || !pixel) {
        return;
    }
    const GpuSolidParameters parameters{*pixel.value(), *window.value(), *window.value(),
                                        PixelAspectRatio::square()};
    GpuSolid& solid = *solidCreated.solid;
    const auto solidBegan = solid.begin(parameters, 1ULL << 32ULL);
    if (solidBegan.code != GpuSolidDiagnosticCode::None ||
        pollUntilTerminal<GpuSolid, GpuSolidPollResult>(solid) != GpuSolidPollResult::Ready) {
        std::cerr << "FAIL: display input solid did not reach Ready\n";
        expectations.expect(false, "display: input solid reaches Ready");
        return;
    }
    auto input = std::make_shared<const GpuImage>(solid.takeImage());
    expectations.expect(input->isValid(), "display: resident input is valid");

    const auto began = display.begin(input, 1ULL << 32ULL);
    expectations.expect(began.code == GpuResidentDisplayDiagnosticCode::None,
                        "display: begin accepted");
    checkTakeGuard<GpuResidentDisplay, GpuResidentDisplayPollResult, GpuDisplayImage>(
        expectations, display, "display", began.code == GpuResidentDisplayDiagnosticCode::None);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;

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
            return 0;
        }

        testSolidGuard(expectations, *device.device);
        testCoveredSolidGuard(expectations, *device.device);
        testUploadGuard(expectations, *device.device);
        testResidentDisplayGuard(expectations, *device.device);

        if (!expectations.ok()) {
            std::cerr << "FAIL: takeImage guard regressions detected\n";
            return 1;
        }
        std::cout << "PASS: takeImage Ready guard on upload/solid/covered/resident display\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
