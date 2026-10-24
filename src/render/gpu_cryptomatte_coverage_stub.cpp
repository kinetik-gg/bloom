#include <bloom/render/gpu_cryptomatte_coverage.hpp>

#include <memory>

// Portable CPU-unavailable stub for CryptomatteCoverageV1. Carries the exact same public API and
// includes no Vulkan header, so a build without GPU dependencies still links. The pure host
// axis-map preparation lives in gpu_cryptomatte_coverage_host.cpp and is shared with the Vulkan
// build, so a CPU-only caller can still prepare and inspect the metadata cost; no dispatch is ever
// available.

namespace bloom::render {

// Complete only in a stub build so GpuImage's default constructor can instantiate. Matches the
// identical definition in gpu_composite_stub.cpp; a class definition may appear in multiple
// translation units and carries no Vulkan type.
struct GpuImageImpl final {};

struct GpuCryptomatteCoverage::Impl final {};

GpuCryptomatteCoverage::GpuCryptomatteCoverage(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuCryptomatteCoverage::GpuCryptomatteCoverage(GpuCryptomatteCoverage&& other) noexcept = default;
GpuCryptomatteCoverage&
GpuCryptomatteCoverage::operator=(GpuCryptomatteCoverage&& other) noexcept = default;
GpuCryptomatteCoverage::~GpuCryptomatteCoverage() = default;

GpuCryptomatteCoverageCreateResult
GpuCryptomatteCoverage::create(GpuDevice&, const GpuCryptomatteCoverageBudgets&) {
    return {nullptr,
            {GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GPU point resampling is unavailable"}};
}

GpuCryptomatteCoverageJobState GpuCryptomatteCoverage::state() const noexcept {
    return GpuCryptomatteCoverageJobState::Idle;
}
const GpuCryptomatteCoverageDiagnostic& GpuCryptomatteCoverage::diagnostic() const noexcept {
    static const GpuCryptomatteCoverageDiagnostic unavailable{
        GpuCryptomatteCoverageDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GPU point resampling is unavailable"};
    return unavailable;
}
bool GpuCryptomatteCoverage::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuCryptomatteCoverage::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuCryptomatteCoverage::lastJobAllocationBytes() const noexcept { return 0; }
GpuCryptomatteCoverageDiagnostic GpuCryptomatteCoverage::begin(const GpuCryptomatteCoverageRequest&,
                                                               std::uint64_t) {
    return diagnostic();
}
GpuCryptomatteCoveragePollResult GpuCryptomatteCoverage::poll() {
    return GpuCryptomatteCoveragePollResult::Failure;
}
const GpuImage* GpuCryptomatteCoverage::image() const noexcept { return nullptr; }
GpuImage GpuCryptomatteCoverage::take() noexcept { return GpuImage{}; }
void GpuCryptomatteCoverage::cancel() noexcept {}
bool GpuCryptomatteCoverage::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
