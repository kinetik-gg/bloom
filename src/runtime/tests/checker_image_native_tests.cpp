#include "checker_image.hpp"

#include <bloom/core/color.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_solid.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using bloom::runtime::detail::CheckerGeometry;

[[nodiscard]] bool checkCase(bloom::render::GpuSolid& gpu, const CheckerGeometry& geometry,
                             const bloom::core::Color4d authoredA,
                             const bloom::core::Color4d authoredB) {
    const auto a = bloom::render::solidPixelFromStraightLinearRec709Scene(authoredA);
    const auto b = bloom::render::solidPixelFromStraightLinearRec709Scene(authoredB);
    const auto width = (std::uint64_t{geometry.authorWidth} * geometry.proxyWidth +
                        geometry.compositionWidth - 1) / geometry.compositionWidth;
    const auto height = (std::uint64_t{geometry.authorHeight} * geometry.proxyHeight +
                         geometry.compositionHeight - 1) / geometry.compositionHeight;
    const auto window = bloom::render::ImageWindow::create(0, 0, width, height);
    if (!a || !b || !window)
        return false;
    const bloom::render::GpuSolidParameters parameters{
        *a.value(), *window.value(), *window.value(),
        bloom::core::PixelAspectRatio::square(),
        bloom::render::GpuSolidParameters::Checker{
            *b.value(), geometry.authorWidth, geometry.authorHeight, geometry.cellWidth,
            geometry.cellHeight, geometry.compositionWidth, geometry.compositionHeight,
            geometry.proxyWidth, geometry.proxyHeight}};
    const auto started = gpu.begin(parameters, 64ULL * 1024ULL * 1024ULL);
    if (started.code != bloom::render::GpuSolidDiagnosticCode::None) {
        std::cerr << started.message << '\n';
        return false;
    }
    auto state = bloom::render::GpuSolidPollResult::Pending;
    while (state == bloom::render::GpuSolidPollResult::Pending)
        state = gpu.poll();
    if (state != bloom::render::GpuSolidPollResult::Ready || gpu.image() == nullptr)
        return false;
    const auto readback = gpu.readback();
    if (!readback.hasValue() || readback.pixels.size() != width * height)
        return false;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto expected = bloom::runtime::detail::checkerPixel(
                *a.value(), *b.value(),
                bloom::runtime::detail::checkerSampleCounts(geometry, x, y));
            const auto actual = readback.pixels[static_cast<std::size_t>(y) * width + x];
            if (!expected)
                return false;
            const auto close = [](const float lhs, const float rhs) {
                return std::abs(lhs - rhs) <= 2e-6F * std::max(1.0F, std::abs(rhs));
            };
            if (!close(actual.red(), expected.value()->red()) ||
                !close(actual.green(), expected.value()->green()) ||
                !close(actual.blue(), expected.value()->blue()) ||
                !close(actual.alpha(), expected.value()->alpha()))
                return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bloom::render::GpuDeviceCreationOptions options;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--loader" && index + 1 < argc)
            options.loader_path = std::filesystem::path(argv[++index]);
        else if (arg == "--require-device")
            requireDevice = true;
        else
            return 2;
    }
    auto device = bloom::render::GpuDevice::create(options);
    if (!device) {
        std::cout << (requireDevice ? "FAIL: " : "SKIP: ") << device.diagnostic.message << '\n';
        return requireDevice ? 1 : 77;
    }
    auto created = bloom::render::GpuSolid::create(*device.device);
    if (!created)
        return 1;
    const bool full = checkCase(*created.solid, {13, 9, 3, 2, 13, 9, 13, 9},
                                {-2.0, 0.5, 4.0, 0.5}, {3.0, -1.0, 0.25, 0.75});
    const bool proxy = checkCase(*created.solid, {13, 9, 3, 2, 13, 9, 7, 5},
                                 {-2.0, 0.5, 4.0, 0.5}, {3.0, -1.0, 0.25, 0.75});
    if (!full || !proxy)
        std::cerr << "Checker native pixels differ from the CPU reference\n";
    return full && proxy ? 0 : 1;
}
