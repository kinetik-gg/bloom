// CPU-unavailable stub for the production final-output readback primitive. It defines the same
// public symbols as the Vulkan implementation so a build without Vulkan headers/VMA links
// unchanged; every begin() fails closed with a typed DeviceUnavailable diagnostic and the GPU
// final-render bridge takes the CPU reference path.

#include <bloom/render/gpu_process_readback.hpp>

#include <memory>
#include <string_view>

namespace bloom::render {

struct GpuProcessReadback::Impl final {
    GpuProcessReadbackState state = GpuProcessReadbackState::Idle;
    GpuProcessReadbackDiagnostic diagnostic;
};

GpuProcessReadback::GpuProcessReadback() : impl_(std::make_unique<Impl>()) {}

GpuProcessReadback::~GpuProcessReadback() = default;

GpuProcessReadbackState GpuProcessReadback::state() const noexcept {
    return impl_ == nullptr ? GpuProcessReadbackState::Idle : impl_->state;
}

const GpuProcessReadbackDiagnostic& GpuProcessReadback::diagnostic() const noexcept {
    static const GpuProcessReadbackDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuProcessReadback::begin(std::shared_ptr<const GpuImage>, const std::uint64_t) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->state = GpuProcessReadbackState::Failure;
    impl_->diagnostic = {GpuProcessReadbackCode::DeviceUnavailable,
                         "the readback primitive was built without Vulkan"};
    return false;
}

GpuProcessReadbackState GpuProcessReadback::poll() noexcept {
    return impl_ == nullptr ? GpuProcessReadbackState::Idle : impl_->state;
}

std::vector<Rgba32f> GpuProcessReadback::take() noexcept { return {}; }

void GpuProcessReadback::cancel() noexcept {}

bool GpuProcessReadback::isOwnerThread() const noexcept { return false; }

} // namespace bloom::render
