#include <bloom/render/gpu_path_coverage.hpp>

#include "vulkan/gpu_path_coverage_fault.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Portable CPU-unavailable stub for the GpuPathCoverage vector coverage
// producer. Carries the exact same public API and includes no Vulkan header, so
// a build without GPU dependencies still links. No mask is ever resident; every
// operation reports a typed unavailable diagnostic.

namespace bloom::render {

struct GpuPathCoverageImpl final {};

GpuPathCoverage::GpuPathCoverage(std::unique_ptr<GpuPathCoverageImpl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuPathCoverage::GpuPathCoverage(GpuPathCoverage&& other) noexcept = default;
GpuPathCoverage& GpuPathCoverage::operator=(GpuPathCoverage&& other) noexcept = default;
GpuPathCoverage::~GpuPathCoverage() = default;

void GpuPathCoverage::releaseImpl() noexcept { impl_.reset(); }

GpuPathCoverageCreateResult GpuPathCoverage::create(GpuDevice&, const GpuPathCoverageBudgets&) {
    return {nullptr,
            {GpuPathCoverageDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; GpuPathCoverage is unavailable"}};
}

GpuPathCoverageJobState GpuPathCoverage::state() const noexcept {
    return GpuPathCoverageJobState::Idle;
}
const GpuPathCoverageDiagnostic& GpuPathCoverage::diagnostic() const noexcept {
    static const GpuPathCoverageDiagnostic unavailable{
        GpuPathCoverageDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; GpuPathCoverage is unavailable"};
    return unavailable;
}
bool GpuPathCoverage::isBoundTo(GpuDevice&) const noexcept { return false; }
bool GpuPathCoverage::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuPathCoverage::lastJobAllocationBytes() const noexcept { return 0; }
std::uint32_t GpuPathCoverage::coverageWidth() const noexcept { return 0; }
std::uint32_t GpuPathCoverage::coverageHeight() const noexcept { return 0; }
std::uint64_t GpuPathCoverage::nativeDispatchCount() noexcept { return 0; }
GpuPathCoverageDiagnostic GpuPathCoverage::begin(const GpuPathCoverageParameters&,
                                                 const PathRasterCoverageGeometry&, std::uint64_t) {
    return diagnostic();
}
GpuPathCoveragePollResult GpuPathCoverage::poll() { return GpuPathCoveragePollResult::Failure; }
GpuPathCoverageReadback GpuPathCoverage::readback(std::uint64_t) noexcept {
    GpuPathCoverageReadback result;
    result.code = GpuPathCoverageReadbackCode::DeviceUnavailable;
    result.message = "Bloom was built without Vulkan dependencies";
    return result;
}
void GpuPathCoverage::cancel() noexcept {}
bool GpuPathCoverage::teardownDrainIncomplete() noexcept { return false; }

std::shared_ptr<GpuPathCoverageMaskOwner> gpuPathCoverageMask(const GpuPathCoverage&) noexcept {
    return nullptr;
}

namespace path_coverage_detail {

std::atomic<std::uint8_t>& pathCoverageFault() noexcept {
    static std::atomic<std::uint8_t> fault{0};
    return fault;
}
std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountXOverride() noexcept {
    static std::atomic<std::uint32_t> value{0};
    return value;
}
std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountYOverride() noexcept {
    static std::atomic<std::uint32_t> value{0};
    return value;
}
bool pathCoverageQuarantineOccupied() noexcept { return false; }
bool retirePathCoverageQuarantineForOwner() noexcept { return false; }
bool acquireResidentSlot(GpuPathCoverageImpl*) noexcept { return false; }
void releaseResidentSlot(GpuPathCoverageImpl*) noexcept {}
std::size_t pathCoverageResidentCapacity() noexcept { return 0; }
std::size_t pathCoverageResidentInUse() noexcept { return 0; }
std::size_t pathCoverageResidentOrphaned() noexcept { return 0; }
std::uint64_t pathCoverageResidentRefusals() noexcept { return 0; }
std::uint64_t pathCoverageResidentRetired() noexcept { return 0; }
void drainPathCoverageResidentOrphansOnOwnerThread() noexcept {}

} // namespace path_coverage_detail

} // namespace bloom::render
