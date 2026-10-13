#include <bloom/render/gpu_operation_batch.hpp>

#include <utility>

namespace bloom::render {
struct GpuOperationBatch::Impl final {};
GpuOperationBatch::GpuOperationBatch(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuOperationBatch::GpuOperationBatch(GpuOperationBatch&&) noexcept = default;
GpuOperationBatch& GpuOperationBatch::operator=(GpuOperationBatch&&) noexcept = default;
GpuOperationBatch::~GpuOperationBatch() = default;
void GpuOperationBatch::releaseImpl() noexcept { impl_.reset(); }
std::unique_ptr<GpuOperationBatch>
GpuOperationBatch::create(GpuDevice&, const GpuOperationBatchBudgets&) noexcept {
    try {
        return std::unique_ptr<GpuOperationBatch>(new GpuOperationBatch(std::make_unique<Impl>()));
    } catch (...) {
        return {};
    }
}
GpuOperationBatchDiagnostic GpuOperationBatch::begin(std::span<const GpuBatchOperation>,
                                                     std::uint64_t) {
    return {GpuOperationBatchCode::Unsupported, "native batching backend is unavailable"};
}
GpuOperationBatchState GpuOperationBatch::poll() noexcept {
    return GpuOperationBatchState::Failure;
}
GpuOperationBatchState GpuOperationBatch::state() const noexcept {
    return GpuOperationBatchState::Failure;
}
GpuOperationBatchDiagnostic GpuOperationBatch::diagnostic() const {
    return {GpuOperationBatchCode::Unsupported, "native batching backend is unavailable"};
}
GpuOperationBatchStatistics GpuOperationBatch::statistics() const noexcept { return {}; }
bool GpuOperationBatch::hasUnretiredSubmission() const noexcept { return false; }
bool GpuOperationBatch::teardownDrainIncomplete() noexcept { return false; }
std::vector<GpuImage> GpuOperationBatch::takeImages() { return {}; }
void GpuOperationBatch::cancel() noexcept {}
} // namespace bloom::render
