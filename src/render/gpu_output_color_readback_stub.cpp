// CPU-unavailable stub for the production combined output-colour readback primitive. It defines the
// same public symbols as the Vulkan implementation so a build without Vulkan headers/VMA links
// unchanged; every begin() fails closed with a typed DeviceUnavailable diagnostic and the caller
// takes the CPU reference path.

#include <bloom/render/gpu_output_color_readback.hpp>

#include <memory>
#include <utility>

namespace bloom::render {

struct GpuOutputColorReadback::Impl final {
    GpuOutputColorReadbackState state = GpuOutputColorReadbackState::Idle;
    GpuOutputColorReadbackDiagnostic diagnostic;
};

GpuOutputColorReadback::GpuOutputColorReadback() : impl_(std::make_unique<Impl>()) {}

GpuOutputColorReadback::~GpuOutputColorReadback() = default;

GpuOutputColorReadbackState GpuOutputColorReadback::state() const noexcept {
    return impl_ == nullptr ? GpuOutputColorReadbackState::Idle : impl_->state;
}

const GpuOutputColorReadbackDiagnostic& GpuOutputColorReadback::diagnostic() const noexcept {
    static const GpuOutputColorReadbackDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuOutputColorReadback::isOwnerThread() const noexcept { return false; }

bool GpuOutputColorReadback::begin(std::shared_ptr<const GpuImage>, std::shared_ptr<const GpuImage>,
                                   std::optional<GpuDisplayImage>, const std::uint64_t) noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->state = GpuOutputColorReadbackState::Failure;
    impl_->diagnostic = {GpuOutputColorReadbackCode::DeviceUnavailable,
                         "the combined readback primitive was built without Vulkan"};
    return false;
}

GpuOutputColorReadbackState GpuOutputColorReadback::poll() noexcept {
    return impl_ == nullptr ? GpuOutputColorReadbackState::Idle : impl_->state;
}

GpuOutputColorReadbackPayloads GpuOutputColorReadback::take() noexcept { return {}; }

void GpuOutputColorReadback::cancel() noexcept {}

bool GpuOutputColorReadback::hasUnretiredSubmission() const noexcept { return false; }

} // namespace bloom::render
