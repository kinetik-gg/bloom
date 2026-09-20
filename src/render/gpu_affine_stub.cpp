#include <bloom/render/gpu_affine.hpp>

#include <memory>

// Portable CPU-unavailable stub for AffineBilinearV1. Carries the exact same public API and
// includes no Vulkan header, so a build without GPU dependencies still links. The pure host
// metadata preparation and the input factories live in gpu_affine_host.cpp and are shared with the
// Vulkan build, so a CPU-only caller can still prepare and inspect the sample-upload cost; no
// dispatch is ever available.

namespace bloom::render {

// Complete only in a stub build so GpuImage's default constructor can instantiate. Matches the
// identical definition in gpu_composite_stub.cpp; a class definition may appear in multiple
// translation units and carries no Vulkan type.
struct GpuImageImpl final {};

struct GpuAffine::Impl final {};

GpuAffine::GpuAffine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuAffine::GpuAffine(GpuAffine&& other) noexcept = default;
GpuAffine& GpuAffine::operator=(GpuAffine&& other) noexcept = default;
GpuAffine::~GpuAffine() = default;

GpuAffineCreateResult GpuAffine::create(GpuDevice&, const GpuAffineBudgets&) {
    return {nullptr,
            {GpuAffineDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GPU affine resampling is unavailable"}};
}

GpuAffineJobState GpuAffine::state() const noexcept { return GpuAffineJobState::Idle; }
const GpuAffineDiagnostic& GpuAffine::diagnostic() const noexcept {
    static const GpuAffineDiagnostic unavailable{
        GpuAffineDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GPU affine resampling is unavailable"};
    return unavailable;
}
bool GpuAffine::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuAffine::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuAffine::lastJobAllocationBytes() const noexcept { return 0; }
GpuAffineDiagnostic GpuAffine::beginAffine(const GpuAffineParameters&, std::uint64_t) {
    return diagnostic();
}
GpuAffineDiagnostic GpuAffine::beginAffineMatrix(const GpuAffineMatrixParameters&, std::uint64_t) {
    return diagnostic();
}
GpuAffinePollResult GpuAffine::poll() { return GpuAffinePollResult::Failure; }
const GpuImage* GpuAffine::image() const noexcept { return nullptr; }
GpuImage GpuAffine::takeImage() noexcept { return GpuImage{}; }
void GpuAffine::cancel() noexcept {}
bool GpuAffine::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
