#include <bloom/render/gpu_device.hpp>

#include <cassert>
#include <memory>
#include <thread>
#include <utility>

// Portable CPU-unavailable stub. This translation unit carries the exact same Bloom-owned public
// API as the Vulkan implementation and includes no Vulkan or VMA header, so a prefix with no GPU
// dependencies still builds, links, and runs the render module. It deliberately publishes a typed
// Unavailable diagnostic instead of a device: the CPU reference path remains the correctness
// oracle, and this backend is honestly labeled rather than pretending to be a software renderer.

namespace bloom::render {
namespace {
const GpuCapabilityReport kEmptyCapabilityReport{};
} // namespace

struct GpuBufferAllocation::Impl final {
    GpuBufferInfo info;
};

struct GpuDevice::Impl final {
    std::thread::id owner;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuCapabilityReport report;
};

GpuDevice::GpuDevice(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GpuDevice::GpuDevice(GpuDevice&& other) noexcept = default;

GpuDevice& GpuDevice::operator=(GpuDevice&& other) noexcept = default;

GpuDevice::~GpuDevice() {
    if (impl_ != nullptr) {
        assert(impl_->owner == std::this_thread::get_id());
    }
}

GpuDeviceState GpuDevice::state() const noexcept {
    return impl_ != nullptr ? impl_->state : GpuDeviceState::Unavailable;
}

bool GpuDevice::isOwnerThread() const noexcept { return false; }

std::uint64_t GpuDevice::ownershipEpoch() const noexcept { return 0U; }

const GpuCapabilityReport& GpuDevice::capabilityReport() const noexcept {
    if (impl_ != nullptr) {
        return impl_->report;
    }
    return kEmptyCapabilityReport;
}

GpuAllocationBudget GpuDevice::availableAllocationBudget() const noexcept { return {}; }

GpuQualification GpuDevice::qualificationFor(const GpuOperationId operation,
                                             const GpuPrecision precision) const noexcept {
    if (impl_ == nullptr) {
        return GpuQualification::Unavailable;
    }
    for (const GpuOperationCapability& capability : impl_->report.operations) {
        if (capability.operation == operation && capability.precision == precision) {
            return capability.qualification;
        }
    }
    return GpuQualification::Unavailable;
}

GpuPresentationStatus GpuDevice::presentationStatus() const noexcept {
    if (impl_ != nullptr) {
        return impl_->report.presentation;
    }
    return {};
}

GpuBorrowedInstanceView GpuDevice::borrowedInstanceView() const noexcept { return {}; }

GpuSurfaceSupportResult GpuDevice::validateBorrowedSurface(const GpuBorrowedSurface&) const {
    return {GpuSurfaceSupport::PresentationUnavailable,
            "Bloom was built without Vulkan dependencies; presentation is unavailable"};
}

GpuDeviceCreationResult GpuDevice::create(const GpuDeviceCreationOptions&) {
    return {nullptr,
            {GpuDiagnosticCode::BackendNotBuilt,
             "Bloom was built without Vulkan dependencies; the CPU reference path remains active"}};
}

GpuBufferAllocationResult GpuDevice::allocateHostBuffer(const std::uint64_t) {
    return {GpuBufferAllocation{},
            {GpuDiagnosticCode::BackendNotBuilt,
             "no GPU allocator exists in a build without Vulkan dependencies"}};
}

GpuBufferAllocation::GpuBufferAllocation() noexcept = default;

GpuBufferAllocation::GpuBufferAllocation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuBufferAllocation::GpuBufferAllocation(GpuBufferAllocation&& other) noexcept = default;

GpuBufferAllocation& GpuBufferAllocation::operator=(GpuBufferAllocation&& other) noexcept = default;

GpuBufferAllocation::~GpuBufferAllocation() = default;

bool GpuBufferAllocation::isValid() const noexcept { return impl_ != nullptr; }

GpuBufferInfo GpuBufferAllocation::info() const noexcept {
    return impl_ != nullptr ? impl_->info : GpuBufferInfo{};
}

} // namespace bloom::render
