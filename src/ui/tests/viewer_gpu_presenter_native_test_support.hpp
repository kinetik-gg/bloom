#pragma once

// Shared support for the ViewerGpuPresenter fixtures. `describeViewerGpuPresenter` turns a failed
// bounded wait into an honest, concrete state report (adapter state, attach state, target id, the
// adapter diagnostic, and the container/window exposure the attach path depends on) instead of a
// bare timeout. The resident-display producer is the real fixture setup both the native acceptance
// fixture uses; it is template-parameterized on the caller's Expectations type so each fixture
// keeps its own failure reporting.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/ui/viewer_gpu_presenter.hpp>

#include <QWidget>
#include <QWindow>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace bloom::ui::test {

[[nodiscard]] inline std::string describeViewerGpuPresenter(const ViewerGpuPresenter& presenter) {
    std::ostringstream out;
    out << "state=" << static_cast<int>(presenter.state()) << " attached=" << presenter.attached()
        << " target=" << presenter.targetId()
        << " safeToDestroy=" << presenter.surfaceSafeToDestroy() << " diagnostic=\""
        << presenter.diagnostic() << '"';
    const QWidget* container = presenter.container();
    if (container != nullptr) {
        out << " container{visible=" << container->isVisible()
            << " hidden=" << container->isHidden() << " size=" << container->width() << 'x'
            << container->height() << '}';
    } else {
        out << " container=null";
    }
    const QWindow* window = presenter.window();
    if (window != nullptr) {
        out << " window{exposed=" << window->isExposed() << " visible=" << window->isVisible()
            << " size=" << window->width() << 'x' << window->height() << '}';
    } else {
        out << " window=null";
    }
    return out.str();
}

[[nodiscard]] inline std::optional<bloom::render::ImageWindow>
makeFixtureWindow(const std::int64_t x, const std::int64_t y, const std::uint64_t width,
                  const std::uint64_t height) {
    const auto created = bloom::render::ImageWindow::create(x, y, width, height);
    return created ? std::optional(*created.value()) : std::nullopt;
}

template <typename Expectations>
[[nodiscard]] std::shared_ptr<const bloom::render::GpuDisplayImage>
produceFixtureDisplay(bloom::render::GpuSolid& solid, bloom::render::GpuResidentDisplay& display,
                      Expectations& expectations) {
    const auto dataWindow = makeFixtureWindow(0, 0, 16, 8);
    const auto displayWindow = makeFixtureWindow(-2, 3, 16, 8);
    const auto aspect = bloom::core::PixelAspectRatio::create(4, 3);
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(
        bloom::core::Color4d{0.25, 0.5, 0.75, 1.0});
    if (!pixel || !dataWindow || !displayWindow || !aspect) {
        expectations.expect(false, "the solid primitive and geometry build");
        return nullptr;
    }
    const auto solidBegin = solid.begin(
        bloom::render::GpuSolidParameters{*pixel.value(), *dataWindow, *displayWindow, *aspect},
        std::uint64_t{1} << 32);
    if (solidBegin.code != bloom::render::GpuSolidDiagnosticCode::None) {
        expectations.expect(false, "the solid begin is accepted: " + solidBegin.message);
        return nullptr;
    }
    bloom::render::GpuSolidPollResult solidPoll = bloom::render::GpuSolidPollResult::Pending;
    while (solidPoll == bloom::render::GpuSolidPollResult::Pending) {
        solidPoll = solid.poll();
    }
    if (solidPoll != bloom::render::GpuSolidPollResult::Ready) {
        expectations.expect(false, "the solid job completes");
        return nullptr;
    }
    auto input = std::make_shared<const bloom::render::GpuImage>(solid.takeImage());
    const auto displayBegin = display.begin(input, std::uint64_t{1} << 32);
    if (displayBegin.code != bloom::render::GpuResidentDisplayDiagnosticCode::None) {
        expectations.expect(false, "the resident display begin is accepted");
        return nullptr;
    }
    bloom::render::GpuResidentDisplayPollResult displayPoll =
        bloom::render::GpuResidentDisplayPollResult::Pending;
    while (displayPoll == bloom::render::GpuResidentDisplayPollResult::Pending) {
        displayPoll = display.poll();
    }
    if (displayPoll != bloom::render::GpuResidentDisplayPollResult::Ready) {
        expectations.expect(false, "the resident display job completes");
        return nullptr;
    }
    auto image = std::make_shared<const bloom::render::GpuDisplayImage>(display.takeImage());
    expectations.expect(image->isValid(), "the resident display image is valid");
    return image->isValid() ? image : nullptr;
}

} // namespace bloom::ui::test
