#include <bloom/render/gpu_composite.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Portable CPU-unavailable stub for the GPU compositing operations. Carries the exact same public
// API and includes no Vulkan header, so a build without GPU dependencies still links.
//
// prepareTranslationAxis() is pure host arithmetic (Float64 subtraction/floor/factor) with no GPU
// dependency, so it is implemented identically here; a caller can prepare metadata even in a
// CPU-only build, but no dispatch is ever available.

namespace bloom::render {

// Complete only in a stub build so GpuImage's default constructor/destructor can instantiate. This
// matches the solid stub's identical definition; a class definition may appear in multiple
// translation units. It carries no Vulkan type.
struct GpuImageImpl final {};

// The minimal P2 seam in a CPU-only build: no resident image ever exists, so the actual allocation
// size is always zero. Defined here (the composite stub is always compiled in a stub build) so the
// runtime cache test links unchanged against the portable API.
std::uint64_t GpuImage::allocationBytes() const noexcept { return 0; }

std::vector<GpuAxisSample> prepareTranslationAxis(const std::uint32_t outputExtent,
                                                  const std::uint32_t sourceExtent,
                                                  const double translation) {
    std::vector<GpuAxisSample> axis(outputExtent);
    for (std::uint32_t local = 0; local < outputExtent; ++local) {
        const auto sample = static_cast<double>(local) - translation;
        if (sample <= -1.0 || sample >= static_cast<double>(sourceExtent)) {
            axis[local] = GpuAxisSample{kGpuAxisOutOfRange, 0.0F};
            continue;
        }
        const auto base = static_cast<std::int64_t>(std::floor(sample));
        axis[local] = GpuAxisSample{static_cast<std::int32_t>(base),
                                    static_cast<float>(sample - static_cast<double>(base))};
    }
    return axis;
}

struct GpuComposite::Impl final {};

GpuComposite::GpuComposite(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuComposite::GpuComposite(GpuComposite&& other) noexcept = default;
GpuComposite& GpuComposite::operator=(GpuComposite&& other) noexcept = default;
GpuComposite::~GpuComposite() = default;

GpuCompositeCreateResult GpuComposite::create(GpuDevice&, const GpuCompositeBudgets&) {
    return {nullptr,
            {GpuCompositeDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GPU compositing is unavailable"}};
}

GpuCompositeJobState GpuComposite::state() const noexcept { return GpuCompositeJobState::Idle; }
const GpuCompositeDiagnostic& GpuComposite::diagnostic() const noexcept {
    static const GpuCompositeDiagnostic unavailable{
        GpuCompositeDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GPU compositing is unavailable"};
    return unavailable;
}
bool GpuComposite::isBoundTo(GpuDevice&) const noexcept { return false; }
GpuCompositeDiagnostic GpuComposite::beginTranslation(const GpuTranslationParameters&,
                                                      std::uint64_t) {
    return diagnostic();
}
GpuCompositeDiagnostic GpuComposite::beginSourceOver(const GpuSourceOverParameters&,
                                                     std::uint64_t) {
    return diagnostic();
}
GpuCompositePollResult GpuComposite::poll() { return GpuCompositePollResult::Failure; }
const GpuImage* GpuComposite::image() const noexcept { return nullptr; }
GpuImage GpuComposite::takeImage() noexcept { return GpuImage{}; }
void GpuComposite::cancel() noexcept {}
bool GpuComposite::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
