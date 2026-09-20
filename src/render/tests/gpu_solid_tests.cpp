// Tests for the SolidV1 GPU-resident image operation. Local mode pins the
// explicit loader and requires a device; a hardware-free CI image prints an
// explicit skip and the CPU-unavailable stub still exercises the
// typed-unavailable path. No fake success: GPU results are compared to the CPU
// solid primitive, and readback is used only as the parity oracle.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <bloom/core/sha256.hpp>

#include "shaders/solid_spirv.inc"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
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
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
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

// The CPU oracle: the same solid primitive and row fill the evaluator uses,
// into a real image so the data-window origin and pixel aspect participate
// exactly as the GPU parameters do.
[[nodiscard]] std::optional<Rgba32fImage> buildOracle(const Color4d color,
                                                      const ImageWindow dataWindow) {
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
    if (!pixel) {
        return std::nullopt;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(dataWindow, dataWindow,
                                                           bloom::core::PixelAspectRatio::square());
    if (!descriptor) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builder) {
        return std::nullopt;
    }
    std::vector<Rgba32f> row;
    row.assign(static_cast<std::size_t>(dataWindow.extent().width()), Rgba32f::transparent());
    bloom::render::fillSolidRow(row, *pixel.value());
    for (std::int64_t y = dataWindow.originY(); y < dataWindow.maxYExclusive(); ++y) {
        auto rowResult = builder.value()->row(y);
        if (!rowResult || rowResult.value()->size() != row.size()) {
            return std::nullopt;
        }
        std::copy(row.begin(), row.end(), rowResult.value()->begin());
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

[[nodiscard]] bool pixelsEqual(const std::span<const Rgba32f> lhs,
                               const std::span<const Rgba32f> rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index] != rhs[index]) {
            return false;
        }
    }
    return true;
}

void runSolidCase(Expectations& expectations, GpuSolid& solid, const Color4d color,
                  const ImageWindow dataWindow) {
    const auto oracle = buildOracle(color, dataWindow);
    expectations.expect(oracle.has_value(), "the CPU oracle builds");
    if (!oracle.has_value()) {
        return;
    }
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
    if (!pixel) {
        expectations.expect(false, "the solid primitive builds");
        return;
    }
    const GpuSolidParameters parameters{*pixel.value(), dataWindow, dataWindow,
                                        bloom::core::PixelAspectRatio::square()};
    const auto begin = solid.begin(parameters, 1ULL << 32ULL);
    expectations.expect(begin.code == GpuSolidDiagnosticCode::None, "the solid begin is accepted");
    if (begin.code != GpuSolidDiagnosticCode::None) {
        return;
    }
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    expectations.expect(poll == GpuSolidPollResult::Ready, "the solid job completes");
    expectations.expect(solid.image() != nullptr && solid.image()->isValid(),
                        "a resident image is published");
    const auto readback = solid.readback();
    expectations.expect(readback.hasValue(), "the resident image reads back for parity");
    if (readback.hasValue()) {
        expectations.expect(pixelsEqual(readback.pixels, oracle.value().pixels()),
                            "the GPU solid matches the CPU oracle exactly");
    }
}

// The embedded SPIR-V array is the bytes uploaded at runtime; hashing the serialized
// little-endian words proves the checked-in source -> SPIR-V -> array relationship. A changed
// array word fails even if every comment in the .inc is intact.
void testEmbeddedSpirvDigest(Expectations& expectations) {
    static_assert(bloom::render::vulkan_detail::kSolidSpirvWordCount * 4U ==
                      bloom::render::vulkan_detail::kSolidSpirvByteCount,
                  "SPIR-V word count must exactly cover the byte count");
    const auto* raw =
        reinterpret_cast<const std::byte*>(bloom::render::vulkan_detail::kSolidSpirvCode);
    const std::span<const std::byte> bytes(raw, bloom::render::vulkan_detail::kSolidSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the embedded SolidV1 SPIR-V hashes");
    if (!digest.has_value()) {
        return;
    }
    const auto hex = digest->toLowercaseHex();
    const std::string_view measured(hex.data(), hex.size());
    expectations.expect(measured == BLOOM_SOLID_SPV_SHA256,
                        "the embedded SolidV1 SPIR-V array hashes to the pinned SPIR-V digest");
    expectations.expect(std::string_view(bloom::render::vulkan_detail::kSolidSpirvDigest) ==
                            BLOOM_SOLID_SPV_SHA256,
                        "the .inc digest comment matches the pinned SPIR-V digest");
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

        // Missing loader: typed unavailable, no crash.
        GpuDeviceCreationOptions missing;
        missing.loader_path = "/nonexistent/bloom-missing-vulkan-loader";
        const auto missingDevice = GpuDevice::create(missing);
        expectations.expect(!missingDevice, "a forced missing loader yields no device");

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

        auto created = GpuSolid::create(*device.device);
        expectations.expect(created.hasValue(), "the SolidV1 pipeline is created");
        if (!created) {
            std::cerr << "FAIL: SolidV1 create failed: " << created.diagnostic.message << '\n';
            return 1;
        }
        GpuSolid& solid = *created.solid;
        expectations.expect(solid.isBoundTo(*device.device), "the pipeline is bound to its device");

        const auto odd = ImageWindow::create(-3, 5, 7, 3);
        expectations.expect(static_cast<bool>(odd), "the odd/nonzero-origin window builds");
        if (!odd) {
            return 1;
        }
        runSolidCase(expectations, solid, Color4d{0.25, 0.5, 0.75, 1.0}, *odd.value());
        runSolidCase(expectations, solid, Color4d{1.0, 0.0, 0.5, 0.0}, *odd.value());
        runSolidCase(expectations, solid, Color4d{0.1, 0.2, 0.3, 1.0}, *odd.value());
        runSolidCase(expectations, solid, Color4d{-0.25, 4.0, 0.125, 0.5}, *odd.value());
        runSolidCase(
            expectations, solid,
            Color4d{static_cast<double>(std::numeric_limits<float>::denorm_min()), 0.0, 0.0, 1.0},
            *odd.value());

        // Budget rejection.
        const auto tinyBudget =
            solid.begin(GpuSolidParameters{Rgba32f::transparent(), *odd.value(), *odd.value(),
                                           bloom::core::PixelAspectRatio::square()},
                        4);
        expectations.expect(tinyBudget.code == GpuSolidDiagnosticCode::OverBudget,
                            "an under-budget resident image is rejected");

        // Cancellation publishes nothing and leaves the pipeline reusable.
        const auto cancelBegin =
            solid.begin(GpuSolidParameters{Rgba32f::transparent(), *odd.value(), *odd.value(),
                                           bloom::core::PixelAspectRatio::square()},
                        1ULL << 32ULL);
        expectations.expect(cancelBegin.code == GpuSolidDiagnosticCode::None,
                            "the cancellable begin is accepted");
        solid.cancel();
        GpuSolidPollResult cancelled = GpuSolidPollResult::Pending;
        while (cancelled == GpuSolidPollResult::Pending) {
            cancelled = solid.poll();
        }
        expectations.expect(cancelled == GpuSolidPollResult::Failure &&
                                solid.diagnostic().code == GpuSolidDiagnosticCode::Cancelled,
                            "a cancelled job reports Cancelled and publishes nothing");
        expectations.expect(solid.image() == nullptr, "no image is published after cancellation");

        // Wrong-thread poll fails closed.
        auto foreignPoll = GpuSolidPollResult::Ready;
        std::thread worker([&solid, &foreignPoll]() { foreignPoll = solid.poll(); });
        worker.join();
        expectations.expect(foreignPoll == GpuSolidPollResult::WrongThread,
                            "poll from a joined non-owner thread is WrongThread");

        // Resource lifetime: take the resident image, destroy the pipeline, then
        // read back on the owner thread; the image co-owns the allocator
        // generation.
        runSolidCase(expectations, solid, Color4d{0.5, 0.5, 0.5, 1.0}, *odd.value());
        GpuImage taken = solid.takeImage();
        expectations.expect(taken.isValid() && solid.image() == nullptr,
                            "takeImage moves ownership out and leaves no resident image");
        created.solid.reset();
        const auto afterDestroy = bloom::render::readbackResidentImage(taken, 1ULL << 32ULL);
        expectations.expect(afterDestroy.hasValue(),
                            "the taken image stays readable after its pipeline is destroyed");
        return expectations.ok() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
