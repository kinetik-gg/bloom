#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/viewer_gpu_resident.hpp>

#include <QTimer>

namespace bloom::ui {
void ViewerEditor::releasePackedFramesForShutdown() {
    if (packedHostShutdown_)
        return;
    packedHostShutdown_ = true;
    disconnect(&previewController_, nullptr, this, nullptr);
    if (gpuResidentTimer_)
        gpuResidentTimer_->stop();
    // These handles have already reached terminal state. Native targets are already safe;
    // dropping the presenter here cannot race an outstanding native surface mutation.
    ++gpuNativeRetireGeneration_;
    ++gpuHostMutationGeneration_;
    gpuNativeRetirePending_ = false;
    gpuHostMutationPending_ = false;
    gpuHostMutationWaiter_.reset();
    clearGpuContainer();
    gpuResident_.reset();
    residentActive_ = false;
    adjustTask_.reset();
    adjustIdentity_.reset();
    adjustedFrame_.reset();
    cpuFallbackTask_.reset();
    cpuFallbackIdentity_.reset();
    cpuFallbackFailedIdentity_.reset();
    cpuFallbackFrame_.reset();
    lastCpuFrame_.reset();
    gpuRequestedFrame_.reset();
    cpuPaintedDisplay_.reset();
    cpuCoverDisplay_.reset();
    coverCaptureDisplay_.reset();
    probeTask_.reset();
    probeIdentity_.reset();
    probeTaskFrame_.reset();
    probeCacheFrame_.reset();
    probeCacheReference_.reset();
    probeCacheDisplayLinear_.reset();
    probeCacheDisplay_.reset();
    channelTask_.reset();
    channelTaskFrame_.reset();
    channelWantedFrame_.reset();
    channelView_.reset();
    gpuPublicationDomain_.reset();
}
} // namespace bloom::ui
