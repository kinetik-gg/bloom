#include <bloom/render/gpu_point_resample.hpp>

#include <memory>

// Portable CPU-unavailable stub for PointResampleV1. Carries the exact same public API and includes
// no Vulkan header, so a build without GPU dependencies still links. The pure host axis-map
// preparation lives in gpu_point_resample_host.cpp and is shared with the Vulkan build, so a
// CPU-only caller can still prepare and inspect the metadata cost; no dispatch is ever available.

namespace bloom::render {

// Complete only in a stub build so GpuImage's default constructor can instantiate. Matches the
// identical definition in gpu_composite_stub.cpp; a class definition may appear in multiple
// translation units and carries no Vulkan type.
struct GpuImageImpl final {};

struct GpuPointResample::Impl final {};

GpuPointResample::GpuPointResample(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuPointResample::GpuPointResample(GpuPointResample&& other) noexcept = default;
GpuPointResample& GpuPointResample::operator=(GpuPointResample&& other) noexcept = default;
GpuPointResample::~GpuPointResample() = default;

GpuPointResampleCreateResult GpuPointResample::create(GpuDevice&, const GpuPointResampleBudgets&) {
    return {nullptr,
            {GpuPointResampleDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GPU point resampling is unavailable"}};
}

GpuPointResampleJobState GpuPointResample::state() const noexcept {
    return GpuPointResampleJobState::Idle;
}
const GpuPointResampleDiagnostic& GpuPointResample::diagnostic() const noexcept {
    static const GpuPointResampleDiagnostic unavailable{
        GpuPointResampleDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GPU point resampling is unavailable"};
    return unavailable;
}
bool GpuPointResample::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuPointResample::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuPointResample::lastJobAllocationBytes() const noexcept { return 0; }
GpuPointResampleDiagnostic GpuPointResample::begin(const GpuPointResampleRequest&, std::uint64_t) {
    return diagnostic();
}
GpuPointResamplePollResult GpuPointResample::poll() { return GpuPointResamplePollResult::Failure; }
const GpuImage* GpuPointResample::image() const noexcept { return nullptr; }
GpuImage GpuPointResample::take() noexcept { return GpuImage{}; }
void GpuPointResample::cancel() noexcept {}
bool GpuPointResample::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
