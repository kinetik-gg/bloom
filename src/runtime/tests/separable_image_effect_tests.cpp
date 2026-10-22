#include "image_effect.hpp"
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
runtime::SeparableBlurKernel kernel(const runtime::SeparableBlurKind kind,
                                    const runtime::BlurEdgeMode edge) {
    return {kind, {document::ParameterId::fromRaw(1), 1.0}, edge};
}
render::Rgba32fImage source() {
    const auto data = render::ImageWindow::create(-3, 5, 3, 2);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    const auto p = render::Rgba32f::fromPremultiplied(-0.25F, 2.0F, 0.5F, 0.5F);
    require(!builder.value()->write(-2, 5, *p.value()), "impulse writes");
    auto image = std::move(*builder.value()).freeze();
    return std::move(*image.value());
}
void referenceTests() {
    const auto input = source();
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        for (const auto edge : {runtime::BlurEdgeMode::Transparent, runtime::BlurEdgeMode::Clamp}) {
            const auto blur =
                runtime::detail::prepareSeparableBlur(kernel(kind, edge), 1.0, 1.0, 1.0);
            require(blur.has_value(), "blur prepares");
            const auto result = runtime::detail::applySeparableBlur(
                *blur, input, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
            require(result.image && !result.diagnostic, "filter publishes finite RGBA");
            const auto output = result.image->descriptor()->dataWindow();
            require(result.image->descriptor()->displayWindow() ==
                        input.descriptor()->displayWindow(),
                    "filter preserves display window independently of data window");
            require(output.originX() == (edge == runtime::BlurEdgeMode::Transparent ? -4 : -3) &&
                        output.originY() == (edge == runtime::BlurEdgeMode::Transparent ? 4 : 5),
                    "transparent halo expands both axes; clamp retains extent");
            const auto original = input.descriptor()->dataWindow();
            // Independent direct 2-D convolution; the implementation is two one-dimensional passes.
            for (auto y = output.originY(); y < output.maxYExclusive(); ++y) {
                for (auto x = output.originX(); x < output.maxXExclusive(); ++x) {
                    std::array<double, 4> expected{};
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            auto sx = x + dx, sy = y + dy;
                            if (edge == runtime::BlurEdgeMode::Clamp) {
                                sx = std::clamp(sx, original.originX(),
                                                original.maxXExclusive() - 1);
                                sy = std::clamp(sy, original.originY(),
                                                original.maxYExclusive() - 1);
                            }
                            const auto sample = input.read(sx, sy);
                            if (!sample)
                                continue;
                            const double weight =
                                static_cast<double>(
                                    blur->horizontal[static_cast<std::size_t>(dx + 1)]) *
                                static_cast<double>(
                                    blur->vertical[static_cast<std::size_t>(dy + 1)]);
                            const auto components = sample.value()->components();
                            for (std::size_t c = 0; c < 4; ++c)
                                expected[c] += static_cast<double>(components[c]) * weight;
                        }
                    const auto actual = result.image->read(x, y);
                    require(actual.hasValue(), "expanded-window sample exists");
                    for (std::size_t c = 0; c < 4; ++c)
                        require(std::abs(static_cast<double>(actual.value()->components()[c]) -
                                         expected[c]) < 2e-6,
                                "separable output matches independent 2-D convolution on signed "
                                "HDR and coverage");
                }
            }
            require(!runtime::detail::applySeparableBlur(*blur, input, 1, {},
                                                         runtime::OperationIndex::fromRaw(0), {})
                         .image,
                    "intermediate budget refuses before publication");
            for (const bool vertical : {false, true}) {
                auto program = runtime::detail::buildSeparableBlurProgram(
                    vertical ? blur->vertical : blur->horizontal, vertical, edge);
                require(program.succeeded(),
                        "normalized host coefficients prepare a device program");
                require(render::validateOcioGpuProgram(*program.program(), {}) ==
                            render::OcioGpuProgramError::None,
                        "program descriptor validates");
                const auto wrapper = runtime::buildGpuOcioWrapperGlsl(*program.program());
                require(
                    wrapper.succeeded() &&
                        wrapper.source.find("imageLoad(bloom_ocio_input, p)") !=
                            std::string::npos &&
                        wrapper.source.find("vec4 p = bloom_separable_blur(c)") !=
                            std::string::npos,
                    "GPU wrapper performs resident neighborhood reads and associated-RGBA writes");
            }
        }
    }
    const auto k = kernel(runtime::SeparableBlurKind::Gaussian, runtime::BlurEdgeMode::Transparent);
    require(runtime::detail::prepareSeparableBlur(k, 0.0, 1.0, 1.0)->identity(),
            "radius zero is identity");
    require(!runtime::detail::prepareSeparableBlur(k, -1.0, 1.0, 1.0) &&
                !runtime::detail::prepareSeparableBlur(k, 257.0, 1.0, 1.0) &&
                !runtime::detail::prepareSeparableBlur(k, std::numeric_limits<double>::quiet_NaN(),
                                                       1.0, 1.0),
            "nonfinite and out-of-domain radius refuses");
    const auto proxy = runtime::detail::prepareSeparableBlur(k, 3.0, 0.5, 2.0);
    require(proxy->horizontal.size() == 5 && proxy->vertical.size() == 13,
            "proxy radii and halos scale independently per axis");
}
void cancellationTest() {
    const auto input = source();
    const auto blur = runtime::detail::prepareSeparableBlur(
        kernel(runtime::SeparableBlurKind::Box, runtime::BlurEdgeMode::Transparent), 2.0, 1.0, 1.0);
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(83)};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Blur cancellation", owner), [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::applySeparableBlur(
                *blur, input, 65536, context.cancellation(), runtime::OperationIndex::fromRaw(0),
                [&](const runtime::EvaluationProgress& progress) {
                    if (progress.total && progress.completed == *progress.total / 2)
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            require(result.cancelled && !result.image,
                    "cancellation between passes publishes no image");
            return runtime::TaskResult<void>::cancelled();
        });
    require(submission.accepted(), "cancellation task admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    require(result && result->state() == runtime::TaskState::Cancelled,
            "pass-boundary cancellation observed");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(scheduler.isQuiescent(), "blur cancellation shuts down safely");
}
} // namespace
int main() {
    try {
        referenceTests();
        cancellationTest();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Gaussian/Box reference, HDR/alpha/windows, proxy halo, budget, resident "
                 "shader and cancellation\n";
}
