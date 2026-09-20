#include <bloom/render/gpu_blend.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// Portable CPU-unavailable stub for the additive GPU blend operation. Carries the exact same public
// API and includes no Vulkan header, so a build without GPU dependencies still links and the CPU
// reference path (blendLinearRec709SceneRow) remains the oracle.

namespace bloom::render {

// Complete only in a stub build so GpuImage's default constructor/destructor can instantiate in
// this translation unit. This matches the composite/solid/upload stubs' identical definition; a
// class definition may appear in multiple translation units. It carries no Vulkan type.
struct GpuImageImpl final {};

struct GpuBlend::Impl final {};

GpuBlend::GpuBlend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuBlend::GpuBlend(GpuBlend&& other) noexcept = default;
GpuBlend& GpuBlend::operator=(GpuBlend&& other) noexcept = default;
GpuBlend::~GpuBlend() = default;

GpuBlendCreateResult GpuBlend::create(GpuDevice&, const GpuBlendBudgets&) {
    return {nullptr,
            {GpuBlendDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GPU blending is unavailable"}};
}

GpuBlendJobState GpuBlend::state() const noexcept { return GpuBlendJobState::Idle; }
const GpuBlendDiagnostic& GpuBlend::diagnostic() const noexcept {
    static const GpuBlendDiagnostic unavailable{
        GpuBlendDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GPU blending is unavailable"};
    return unavailable;
}
bool GpuBlend::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuBlend::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuBlend::lastJobAllocationBytes() const noexcept { return 0; }
GpuBlendDiagnostic GpuBlend::beginBlend(const GpuBlendParameters&, std::uint64_t) {
    return diagnostic();
}
GpuBlendPollResult GpuBlend::poll() { return GpuBlendPollResult::Failure; }
const GpuImage* GpuBlend::image() const noexcept { return nullptr; }
GpuImage GpuBlend::takeImage() noexcept { return GpuImage{}; }
void GpuBlend::cancel() noexcept {}
bool GpuBlend::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
