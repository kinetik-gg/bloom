#include "image_effect.hpp"
#include "resize_fit_geometry.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

render::Rgba32fImage
source(const std::int64_t x = 0, const std::int64_t y = 0,
       const core::PixelAspectRatio aspect = core::PixelAspectRatio::square()) {
    const auto window = render::ImageWindow::create(x, y, 4, 2);
    const auto descriptor =
        render::Rgba32fImageDescriptor::create(*window.value(), *window.value(), aspect);
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    for (auto rowY = y; rowY < y + 2; ++rowY) {
        auto row = builder.value()->row(rowY);
        for (std::size_t column = 0; column < 4; ++column) {
            // Negative and HDR premultiplied RGB, with varying coverage and an exact zero pixel.
            const float a = column == 0 ? 0.0F : 0.5F;
            const auto pixel =
                render::Rgba32f::fromPremultiplied(-a, 4.0F * a, static_cast<float>(column) * a, a);
            (*row.value())[column] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

runtime::detail::ImageEffectResult run(const runtime::ResizeFitKernel& kernel,
                                       const render::Rgba32fImage& input,
                                       const std::size_t budget = 1U << 20U,
                                       const runtime::CancellationToken cancellation = {}) {
    runtime::detail::PreparedImageEffect effect;
    effect.resizeFit = kernel;
    return runtime::detail::applyImageEffect(effect, input, nullptr, budget, cancellation,
                                             runtime::OperationIndex::fromRaw(1), {});
}

float alpha(const render::Rgba32fImage& image, const std::int64_t x, const std::int64_t y) {
    const auto pixel = image.read(x, y);
    if (!pixel)
        throw std::logic_error("Resize/Fit test coordinate outside output");
    return pixel.value()->alpha();
}

void testKernel() {
    const auto input = source();
    const auto identity = run({4, 2}, input);
    expect(identity.image && std::ranges::equal(identity.image->pixels(), input.pixels()),
           "unit Resize is an exact premultiplied pixel copy");
    const auto fit = run({8, 8, 1, 1, 0, 1}, input);
    const auto fill = run({8, 8, 1, 1, 0, 2}, input);
    const auto resize = run({8, 8}, input);
    expect(fit.image && fill.image && resize.image, "Resize, Fit and Fill produce valid images");
    if (!fit.image || !fill.image || !resize.image)
        return;
    expect(alpha(*fit.image, 4, 0) == 0 && alpha(*fit.image, 4, 7) == 0 &&
               alpha(*fit.image, 4, 3) == 0.5F,
           "Fit centres a wide image with transparent letterbox rows");
    expect(alpha(*fill.image, 4, 0) > 0 && alpha(*resize.image, 4, 0) > 0,
           "Fill crops the wide image and Resize stretches it without letterbox rows");
    expect(alpha(*fill.image, 0, 3) > 0,
           "centred Fill removes the transparent leftmost source column");
    bool negative = false, hdr = false;
    for (const auto pixel : fit.image->pixels()) {
        negative = negative || pixel.red() < 0;
        hdr = hdr || pixel.green() > 1;
        expect(pixel.alpha() != 0 || (pixel.red() == 0 && pixel.green() == 0 && pixel.blue() == 0),
               "transparent border taps remain exact transparent black");
    }
    expect(negative && hdr, "bilinear filtering preserves signed and HDR RGB");
    const auto offset = source(-13, 7);
    const auto shifted = run({8, 8, 1, 1, 0, 1}, offset);
    expect(shifted.image && std::ranges::equal(fit.image->pixels(), shifted.image->pixels()),
           "negative/nonzero data and display origins do not shift the fitted picture");

    const auto wideAspect = *core::PixelAspectRatio::create(2, 1);
    const auto anamorphic = source(0, 0, wideAspect);
    const auto parFit = run({8, 8, 1, 1, 0, 1}, anamorphic);
    expect(parFit.image && alpha(*parFit.image, 4, 2) == 0 && alpha(*parFit.image, 4, 3) > 0,
           "Fit uses physical aspect including the source pixel aspect");
    const auto outputPar = run({8, 8, 2, 1, 0, 1}, input);
    expect(outputPar.image && outputPar.image->descriptor()->pixelAspect() == wideAspect,
           "the authored output PAR survives CPU publication");
    const auto proxy =
        runtime::detail::resolveResizeFitGeometry({7, 5, 2, 1}, *input.descriptor(), 0.5, 0.25);
    expect(proxy && proxy->descriptor.dataWindow().extent().width() == 4 &&
               proxy->descriptor.dataWindow().extent().height() == 2 &&
               proxy->descriptor.pixelAspect() == *core::PixelAspectRatio::create(7, 5),
           "asymmetric proxies round extents and retain the exact physical output aspect");

    const auto offsetData = render::ImageWindow::create(-1, 0, 6, 2);
    const auto sparseDescriptor = render::Rgba32fImageDescriptor::create(
        *offsetData.value(), input.descriptor()->displayWindow(), core::PixelAspectRatio::square());
    const auto sparseGeometry =
        runtime::detail::resolveResizeFitGeometry({8, 4}, *sparseDescriptor.value(), 1, 1);
    expect(sparseGeometry && sparseGeometry->matrix.a == 2 && sparseGeometry->matrix.x == 0,
           "data overscan does not change the displayed source canvas used for Resize");

    expect(!runtime::detail::validResizeFitKernel({0, 8}) &&
               !runtime::detail::validResizeFitKernel({8, -1}) &&
               !runtime::detail::validResizeFitKernel({8, 8, 0, 1}) &&
               !runtime::detail::validResizeFitKernel({8, 8, 1, 1, 1}),
           "zero/negative extents, degenerate PAR and unknown filters are refused");
    expect(run({0, 8}, input).diagnostic.has_value(),
           "invalid output geometry returns a diagnostic before allocation");
    expect(run({8, 8}, input, 1).diagnostic.has_value(),
           "output allocation obeys the remaining pixel budget");
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    const runtime::TaskOwner owner{runtime::TaskOwnerKind::Composition,
                                   runtime::TaskOwnerId::fromRaw(1)};
    auto group = scheduler.createGroup(owner, "Resize/Fit cancellation");
    runtime::TaskRequest request{"Resize/Fit row cancellation", owner};
    request.groupId = group.handle.id();
    std::atomic_bool observedCancellation = false;
    auto submission = scheduler.submit<void>(
        request, [&input, &group, &observedCancellation](runtime::TaskContext& context) {
            runtime::detail::PreparedImageEffect effect;
            effect.resizeFit = runtime::ResizeFitKernel{8, 8};
            const auto stopped =
                runtime::detail::applyImageEffect(effect, input, nullptr, 1U << 20U,
                                                  context.cancellation(), runtime::OperationIndex::fromRaw(1),
                                                  [&group](const auto&) { group.handle.cancel(); });
            observedCancellation.store(stopped.cancelled && !stopped.image);
            return stopped.cancelled && !stopped.image ? runtime::TaskResult<void>::cancelled()
                                                       : runtime::TaskResult<void>::succeeded();
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<runtime::TaskResult<void>> stopped;
    while (!stopped && std::chrono::steady_clock::now() < deadline) {
        stopped = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(stopped && stopped->state() == runtime::TaskState::Cancelled &&
               observedCancellation.load(),
           "row cancellation retires the task without publishing a partial image");
    scheduler.beginShutdown();
}
} // namespace

int main() {
    try {
        testKernel();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
