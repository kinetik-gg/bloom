#include <bloom/render/gpu_resident_display.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Portable CPU-unavailable stub for the GPU-resident display operation. No Vulkan header; every
// operation reports a typed unavailable diagnostic and no image is ever resident.

namespace bloom::render {

struct GpuDisplayImageImpl final {};

GpuDisplayImage::GpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuDisplayImage::GpuDisplayImage(GpuDisplayImage&& other) noexcept = default;
GpuDisplayImage& GpuDisplayImage::operator=(GpuDisplayImage&& other) noexcept = default;
GpuDisplayImage::~GpuDisplayImage() = default;
bool GpuDisplayImage::isValid() const noexcept { return false; }
std::uint32_t GpuDisplayImage::width() const noexcept { return 0; }
std::uint32_t GpuDisplayImage::height() const noexcept { return 0; }
std::optional<ImageWindow> GpuDisplayImage::dataWindow() const noexcept { return std::nullopt; }
std::optional<ImageWindow> GpuDisplayImage::displayWindow() const noexcept { return std::nullopt; }
core::PixelAspectRatio GpuDisplayImage::pixelAspect() const noexcept {
    return core::PixelAspectRatio::square();
}
std::uint32_t GpuDisplayImage::generation() const noexcept { return 0; }
bool GpuDisplayImage::isBoundTo(GpuDevice&) const noexcept { return false; }
void GpuDisplayImage::releaseOwnedImpl() noexcept {}
GpuDisplayImage makeGpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept {
    return GpuDisplayImage(std::move(impl));
}

GpuDisplayImageReadback readbackResidentDisplayImage(const GpuDisplayImage&,
                                                     std::uint64_t) noexcept {
    GpuDisplayImageReadback result;
    result.code = GpuDisplayImageReadbackCode::DeviceUnavailable;
    result.message = "Bloom was built without Vulkan dependencies";
    return result;
}

struct GpuResidentDisplay::Impl final {};

GpuResidentDisplay::GpuResidentDisplay(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuResidentDisplay::GpuResidentDisplay(GpuResidentDisplay&& other) noexcept = default;
GpuResidentDisplay& GpuResidentDisplay::operator=(GpuResidentDisplay&& other) noexcept = default;
GpuResidentDisplay::~GpuResidentDisplay() = default;
void GpuResidentDisplay::releaseImpl() noexcept {}

GpuResidentDisplayCreateResult GpuResidentDisplay::create(GpuDevice&,
                                                          const GpuResidentDisplayBudgets&) {
    return {nullptr,
            {GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; resident display is unavailable"}};
}
GpuResidentDisplayJobState GpuResidentDisplay::state() const noexcept {
    return GpuResidentDisplayJobState::Idle;
}
const GpuResidentDisplayDiagnostic& GpuResidentDisplay::diagnostic() const noexcept {
    static const GpuResidentDisplayDiagnostic unavailable{
        GpuResidentDisplayDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; resident display is unavailable"};
    return unavailable;
}
bool GpuResidentDisplay::isBoundTo(GpuDevice&) const noexcept { return false; }
GpuResidentDisplayDiagnostic GpuResidentDisplay::begin(std::shared_ptr<const GpuImage>,
                                                       std::uint64_t) {
    return diagnostic();
}
GpuResidentDisplayPollResult GpuResidentDisplay::poll() {
    return GpuResidentDisplayPollResult::Failure;
}
const GpuDisplayImage* GpuResidentDisplay::image() const noexcept { return nullptr; }
GpuDisplayImage GpuResidentDisplay::takeImage() noexcept { return GpuDisplayImage{}; }
GpuDisplayImageReadback GpuResidentDisplay::readback() noexcept {
    return readbackResidentDisplayImage(GpuDisplayImage{}, 0);
}
void GpuResidentDisplay::cancel() noexcept {}
bool GpuResidentDisplay::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
