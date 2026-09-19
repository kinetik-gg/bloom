#include <bloom/render/gpu_presentation_target.hpp>

#include <memory>
#include <string>
#include <utility>

// Portable CPU-unavailable stub. It carries the exact Bloom-owned presentation-target API and
// includes no Vulkan header, so a prefix with no GPU dependencies still links. Every entry point
// reports a typed PresentationUnavailable and the caller keeps the CPU fallback.

namespace bloom::render {

struct GpuPresentationTarget::Impl final {};

GpuPresentationTarget::GpuPresentationTarget(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuPresentationTarget::GpuPresentationTarget(GpuPresentationTarget&&) noexcept = default;

GpuPresentationTarget& GpuPresentationTarget::operator=(GpuPresentationTarget&&) noexcept = default;

GpuPresentationTarget::~GpuPresentationTarget() = default;

GpuPresentationTargetResult GpuPresentationTarget::create(GpuDevice&,
                                                          const GpuPresentationTargetDescription&) {
    return {nullptr, GpuPresentationTargetCode::PresentationUnavailable,
            "Bloom was built without Vulkan dependencies; the CPU path remains active"};
}

GpuPresentationTargetCode GpuPresentationTarget::acquire() {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetCode GpuPresentationTarget::present(GpuClearColor) {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetCode GpuPresentationTarget::pollRetirement() {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetCode GpuPresentationTarget::recreate(const GpuPresentationTargetDescription&) {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetCode GpuPresentationTarget::beginRetire() {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetCode GpuPresentationTarget::retireState() const noexcept {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

GpuPresentationTargetInfo GpuPresentationTarget::info() const noexcept { return {}; }

std::uint32_t GpuPresentationTarget::acquiredImageIndex() const noexcept { return 0; }

GpuPresentationTargetCode GpuPresentationTarget::lastCode() const noexcept {
    return GpuPresentationTargetCode::PresentationUnavailable;
}

const std::string& GpuPresentationTarget::lastMessage() const noexcept {
    static const std::string kUnavailable =
        "Bloom was built without Vulkan dependencies; the CPU path remains active";
    return kUnavailable;
}

} // namespace bloom::render
