#include "gpu_ocio_program_private.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bloom::render {
namespace {

[[nodiscard]] GpuOcioProgramDiagnostic makeDiagnostic(const GpuOcioProgramDiagnosticCode code,
                                                      std::string message) noexcept {
    GpuOcioProgramDiagnostic diagnostic;
    diagnostic.code = code;
    try {
        diagnostic.message = std::move(message);
    } catch (...) {
        diagnostic.message.clear();
    }
    return diagnostic;
}

[[nodiscard]] GpuOcioProgramDiagnostic beginDiagnostic(const GpuOcioProgramDiagnostic& source) {
    GpuOcioProgramDiagnostic copy;
    copy.code = source.code;
    try {
        copy.message = source.message;
    } catch (...) {
        copy.message.clear();
    }
    return copy;
}

} // namespace

GpuOcioProgram::GpuOcioProgram(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuOcioProgram::GpuOcioProgram(GpuOcioProgram&& other) noexcept : impl_(std::move(other.impl_)) {}
GpuOcioProgram& GpuOcioProgram::operator=(GpuOcioProgram&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuOcioProgram::~GpuOcioProgram() { releaseImpl(); }

void GpuOcioProgram::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->control == nullptr || !impl_->onOwnerThread()) {
        // A foreign-thread teardown can never safely touch Vulkan; count it and latch the process
        // fuse (typed, bounded) rather than silently leaking uncounted.
        ocio_program_detail::noteQuarantine();
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    if (impl_->queueSubmitted && !impl_->drainAndRetire()) {
        ocio_program_detail::noteQuarantine();
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    impl_.reset();
}

GpuOcioProgramJobState GpuOcioProgram::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuOcioProgramJobState::Failure;
}
const GpuOcioProgramDiagnostic& GpuOcioProgram::diagnostic() const noexcept {
    static const GpuOcioProgramDiagnostic unavailable =
        makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable, "no pipeline was created");
    return impl_ != nullptr ? impl_->jobDiagnostic : unavailable;
}
bool GpuOcioProgram::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto state = GpuRendererAccess::state(device);
    return state != nullptr && state == impl_->control;
}

GpuOcioProgramDiagnostic GpuOcioProgram::beginEffect(std::shared_ptr<const GpuImage> input,
                                                     std::span<const std::byte> uniformBytes,
                                                     const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                              "no pipeline was created");
    }
    auto& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.displayArm) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                              "this program was extracted for the display arm");
    }
    if (impl.jobState == GpuOcioProgramJobState::Pending) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::Busy, "a job is already in flight");
    }
    // A failure that left a submission unretired (e.g. an unexpected fence status) must not be
    // cleared: clearJob() would resize/destroy the packed buffer or publish nothing while the GPU
    // may still read it. Refuse typed Busy until the owner proves retirement or loses the device.
    if (impl.queueSubmitted) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::Busy,
                              "the previous submission is not proven retired");
    }
    if (impl.jobState != GpuOcioProgramJobState::Idle) {
        impl.clearJob();
    }
    impl.beginImpl(false, input, uniformBytes, byteBudget);
    return beginDiagnostic(impl.jobDiagnostic);
}

GpuOcioProgramDiagnostic GpuOcioProgram::beginDisplay(std::shared_ptr<const GpuImage> input,
                                                      std::span<const std::byte> uniformBytes,
                                                      const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                              "no pipeline was created");
    }
    auto& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (!impl.displayArm) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                              "this program was extracted for the effect arm");
    }
    if (impl.jobState == GpuOcioProgramJobState::Pending) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::Busy, "a job is already in flight");
    }
    // A failure that left a submission unretired (e.g. an unexpected fence status) must not be
    // cleared: clearJob() would resize/destroy the packed buffer or publish nothing while the GPU
    // may still read it. Refuse typed Busy until the owner proves retirement or loses the device.
    if (impl.queueSubmitted) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::Busy,
                              "the previous submission is not proven retired");
    }
    if (impl.jobState != GpuOcioProgramJobState::Idle) {
        impl.clearJob();
    }
    impl.beginImpl(true, input, uniformBytes, byteBudget);
    return beginDiagnostic(impl.jobDiagnostic);
}

GpuOcioProgramPollResult GpuOcioProgram::poll() {
    if (impl_ == nullptr) {
        return GpuOcioProgramPollResult::Failure;
    }
    auto& impl = *impl_;
    if (!impl.onOwnerThread()) {
        impl.fail(GpuOcioProgramDiagnosticCode::WrongThread, "poll must run on the owner thread");
        return GpuOcioProgramPollResult::Failure;
    }
    if (impl.jobState != GpuOcioProgramJobState::Pending) {
        return impl.jobState == GpuOcioProgramJobState::Ready ? GpuOcioProgramPollResult::Ready
                                                              : GpuOcioProgramPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return GpuOcioProgramPollResult::Pending;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.fail(GpuOcioProgramDiagnosticCode::DeviceLost, "the device was lost during dispatch");
        return GpuOcioProgramPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        impl.fail(GpuOcioProgramDiagnosticCode::NativeTimeout, "the fence query failed");
        return GpuOcioProgramPollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (impl.discardRequested.load()) {
        impl.fail(GpuOcioProgramDiagnosticCode::Cancelled, "the job was cancelled");
        return GpuOcioProgramPollResult::Failure;
    }
    if (!impl.checkStatus()) {
        impl.jobState = GpuOcioProgramJobState::Failure;
        return GpuOcioProgramPollResult::Failure;
    }
    impl.jobState = GpuOcioProgramJobState::Ready;
    return GpuOcioProgramPollResult::Ready;
}

std::shared_ptr<GpuImage> GpuOcioProgram::takeEffectOutput() noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuOcioProgramJobState::Ready ||
        impl_->effectOutput == nullptr) {
        return {};
    }
    auto output = std::make_shared<GpuImage>(std::move(*impl_->effectOutput));
    impl_->effectOutput.reset();
    impl_->inputRetained.reset();
    impl_->jobState = GpuOcioProgramJobState::Idle;
    impl_->jobDiagnostic = {};
    return output;
}

GpuDisplayImage GpuOcioProgram::takeDisplayOutput() noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuOcioProgramJobState::Ready ||
        impl_->displayOutput == nullptr) {
        return {};
    }
    GpuDisplayImage output = std::move(*impl_->displayOutput);
    impl_->displayOutput.reset();
    impl_->inputRetained.reset();
    impl_->jobState = GpuOcioProgramJobState::Idle;
    impl_->jobDiagnostic = {};
    return output;
}

bool GpuOcioProgram::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}
std::uint64_t GpuOcioProgram::retainedAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->retainedResourceBytes : 0;
}
std::uint64_t GpuOcioProgram::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}
void GpuOcioProgram::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}
bool GpuOcioProgram::teardownDrainIncomplete() noexcept {
    return ocio_program_detail::teardownIncomplete();
}

} // namespace bloom::render
