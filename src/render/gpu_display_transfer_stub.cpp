#include <bloom/render/gpu_display_transfer.hpp>

namespace bloom::render {
GpuDisplayTransferRetentionSnapshot GpuDisplayTransferRetention::snapshot() const noexcept {
    return {};
}
std::shared_ptr<const GpuDisplayTransferRetention> GpuDisplayTransfer::retention() const noexcept {
    return {};
}

struct GpuDisplayTransfer::Impl final {
    GpuDisplayTransferState state = GpuDisplayTransferState::Idle;
    GpuDisplayTransferDiagnostic diagnostic;
    GpuDisplayTransferAccounting accounting;
};

GpuDisplayTransfer::GpuDisplayTransfer() : impl_(std::make_unique<Impl>()) {}
GpuDisplayTransfer::~GpuDisplayTransfer() = default;

bool GpuDisplayTransfer::begin(GpuDevice&, std::shared_ptr<const GpuDisplayImage>, std::uint64_t,
                               const std::shared_ptr<GpuProcessingPhaseLease>&) noexcept {
    impl_->state = GpuDisplayTransferState::Failure;
    impl_->diagnostic = {GpuDisplayTransferCode::DeviceUnavailable,
                         "GPU final-display transfer was built without Vulkan"};
    return false;
}

GpuDisplayTransferState GpuDisplayTransfer::poll() noexcept { return impl_->state; }
GpuDisplayTransferState GpuDisplayTransfer::state() const noexcept { return impl_->state; }
const GpuDisplayTransferDiagnostic& GpuDisplayTransfer::diagnostic() const noexcept {
    return impl_->diagnostic;
}
const GpuDisplayTransferAccounting& GpuDisplayTransfer::accounting() const noexcept {
    return impl_->accounting;
}
std::optional<GpuDisplayTransferResult> GpuDisplayTransfer::take() noexcept { return std::nullopt; }
void GpuDisplayTransfer::cancel() noexcept {}
bool GpuDisplayTransfer::reset() noexcept {
    impl_->state = GpuDisplayTransferState::Idle;
    impl_->diagnostic = {};
    return true;
}
bool GpuDisplayTransfer::hasUnretiredSubmission() const noexcept { return false; }
bool GpuDisplayTransfer::isOwnerThread() const noexcept { return false; }
bool GpuDisplayTransferResult::isBoundTo(GpuDevice&) const noexcept { return false; }

} // namespace bloom::render
