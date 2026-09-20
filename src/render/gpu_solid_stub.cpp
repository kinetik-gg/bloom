#include <bloom/render/gpu_solid.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

// Portable CPU-unavailable stub for the SolidV1 / CoveredSolidV1 GPU-resident
// operations. Carries the exact same public API and includes no Vulkan header,
// so a build without GPU dependencies still links. No image is ever resident;
// every operation reports a typed unavailable diagnostic.

namespace bloom::render {

struct GpuImageImpl final {};

GpuImage::GpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept : impl_(std::move(impl)) {}
GpuImage::GpuImage(GpuImage&& other) noexcept = default;
GpuImage& GpuImage::operator=(GpuImage&& other) noexcept = default;
GpuImage::~GpuImage() = default;

bool GpuImage::isValid() const noexcept { return false; }
std::uint32_t GpuImage::width() const noexcept { return 0; }
std::uint32_t GpuImage::height() const noexcept { return 0; }
std::optional<ImageWindow> GpuImage::dataWindow() const noexcept { return std::nullopt; }
std::optional<ImageWindow> GpuImage::displayWindow() const noexcept { return std::nullopt; }
core::PixelAspectRatio GpuImage::pixelAspect() const noexcept {
    return core::PixelAspectRatio::square();
}
std::uint32_t GpuImage::generation() const noexcept { return 0; }
bool GpuImage::isBoundTo(GpuDevice&) const noexcept { return false; }

GpuImage makeGpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept {
    return GpuImage(std::move(impl));
}

GpuImageReadback readbackResidentImage(const GpuImage&, std::uint64_t) noexcept {
    GpuImageReadback result;
    result.code = GpuImageReadbackCode::DeviceUnavailable;
    result.message = "Bloom was built without Vulkan dependencies";
    return result;
}

struct GpuSolid::Impl final {};

GpuSolid::GpuSolid(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuSolid::GpuSolid(GpuSolid&& other) noexcept = default;
GpuSolid& GpuSolid::operator=(GpuSolid&& other) noexcept = default;
GpuSolid::~GpuSolid() = default;

GpuSolidCreateResult GpuSolid::create(GpuDevice&, const GpuSolidBudgets&) {
    return {nullptr,
            {GpuSolidDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; SolidV1 is unavailable"}};
}

GpuSolidJobState GpuSolid::state() const noexcept { return GpuSolidJobState::Idle; }
const GpuSolidDiagnostic& GpuSolid::diagnostic() const noexcept {
    static const GpuSolidDiagnostic unavailable{
        GpuSolidDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; SolidV1 is unavailable"};
    return unavailable;
}
bool GpuSolid::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuSolid::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuSolid::lastJobAllocationBytes() const noexcept { return 0; }
GpuSolidDiagnostic GpuSolid::begin(const GpuSolidParameters&, std::uint64_t) {
    return diagnostic();
}
GpuSolidDiagnostic GpuSolid::beginCovered(const GpuSolidParameters&, std::span<const std::uint8_t>,
                                          float, std::uint64_t) {
    return diagnostic();
}
GpuSolidDiagnostic GpuSolid::beginCoveredResident(const GpuSolidParameters&, const GpuPathCoverage&,
                                                  float, std::uint64_t) {
    return diagnostic();
}
GpuSolidPollResult GpuSolid::poll() { return GpuSolidPollResult::Failure; }
const GpuImage* GpuSolid::image() const noexcept { return nullptr; }
GpuImage GpuSolid::takeImage() noexcept { return GpuImage{}; }
GpuImageReadback GpuSolid::readback() noexcept { return readbackResidentImage(GpuImage{}, 0); }
void GpuSolid::cancel() noexcept {}
bool GpuSolid::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
