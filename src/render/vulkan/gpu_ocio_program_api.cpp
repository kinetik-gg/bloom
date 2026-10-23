#include "gpu_ocio_program_private.hpp"
#include "ocio_gpu_program_fault.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace bloom::render::ocio_program_detail {
std::atomic<std::uint32_t>& ocioFaultPublicationAfter() noexcept {
    static std::atomic<std::uint32_t> after{std::numeric_limits<std::uint32_t>::max()};
    return after;
}
std::atomic<bool>& ocioFaultJobFenceUnknown() noexcept {
    static std::atomic<bool> unknown{false};
    return unknown;
}
} // namespace bloom::render::ocio_program_detail

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
    if (!impl_->onOwnerThread() || !impl_->retirementReady(*impl_->control)) {
        impl_->enqueueRetirement();
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    impl_.reset();
}

bool GpuOcioProgram::Impl::registerRetirement() noexcept {
    return registration.attach(
        control, this,
        [](void* program, vulkan_detail::DeviceAllocatorState& state) noexcept {
            return static_cast<Impl*>(program)->retirementReady(state);
        },
        [](void* program) noexcept { delete static_cast<Impl*>(program); });
}

void GpuOcioProgram::Impl::enqueueRetirement() noexcept {
    // The final owner has exclusive access to Impl. No foreign-thread access to the native
    // generation's owner-only phase pointer or latch is required.
    if (queueSubmitted) {
        if (auto phase = submittedPhase.lock()) {
            phase->markUnproven();
        }
    }
    registration.enqueue();
}

bool GpuOcioProgram::Impl::retirementReady(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (control.get() != &state || !onOwnerThread() ||
        state.ownerLifetimeToken != vulkan_detail::deviceOwnerLifetimeToken() ||
        state.presentationEpoch != registration.node.epoch) {
        return false;
    }
    if (!queueSubmitted) {
        return true;
    }
    if (auto phase = submittedPhase.lock();
        phase && phase->isUnproven() && phase == state.processingPhase) {
        state.processingPhaseUnproven = true;
    }
    if (!*fence) {
        return false;
    }
    const VkResult fenceStatus = state.device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*state.device), static_cast<VkFence>(*fence));
    if (fenceStatus != VK_SUCCESS && fenceStatus != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    deviceLost = fenceStatus == VK_ERROR_DEVICE_LOST;
    queueSubmitted = false;
    return true;
}

GpuOcioProgramJobState GpuOcioProgram::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuOcioProgramJobState::Failure;
}
const GpuOcioProgramDiagnostic& GpuOcioProgram::diagnostic() const noexcept {
    static const GpuOcioProgramDiagnostic unavailable =
        makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable, "no pipeline was created");
    if (impl_ == nullptr) {
        return unavailable;
    }
    return impl_->jobDiagnostic;
}
GpuProcessingRootIdentity GpuOcioProgram::creationRootIdentity() const noexcept {
    return impl_ != nullptr && impl_->onOwnerThread() ? impl_->creationRoot
                                                      : GpuProcessingRootIdentity{};
}
bool GpuOcioProgram::publishSampledStorage() noexcept {
    if (!impl_ || !impl_->onOwnerThread() || impl_->control->processingPhaseUnproven) {
        return false;
    }
    const auto& current = impl_->control->processingPhase;
    if ((current && current->isUnproven()) ||
        (current ? current->rootIdentity() : GpuProcessingRootIdentity{}) != impl_->creationRoot) {
        return false;
    }
    // Validate the whole retained buffer set before transferring any member out of the phase.
    for (const auto* buffer : {&impl_->uniformBuffer, &impl_->status}) {
        if (buffer->storageToken.bytes() != 0 && !buffer->storageToken.canPromote())
            return false;
    }
    std::uint32_t promoted = 0;
    for (auto& texture : impl_->textures) {
        if (promoted >=
            ocio_program_detail::ocioFaultPublicationAfter().load(std::memory_order_acquire)) {
            return false;
        }
        if (texture.storage.bytes() != 0 && !texture.storage.promote()) {
            return false;
        }
        ++promoted;
    }
    // The retained descriptor set owns these buffers for the same cache lifetime as its LUTs.
    // Failed partial publication remains internal: the destructor folds the proved destroyed
    // prefix into the exact creating phase, including already-promoted buffers.
    for (auto* buffer : {&impl_->uniformBuffer, &impl_->status}) {
        if (buffer->buffer == VK_NULL_HANDLE)
            continue;
        if (promoted >=
            ocio_program_detail::ocioFaultPublicationAfter().load(std::memory_order_acquire))
            return false;
        if (buffer->storageToken.bytes() != 0 && !buffer->storageToken.promote())
            return false;
        ++promoted;
    }
    impl_->sampledPublished = true;
    return true;
}

bool GpuOcioProgram::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto state = GpuRendererAccess::state(device);
    return state != nullptr && state == impl_->control;
}

GpuOcioProgramDiagnostic GpuOcioProgram::Impl::authorityDiagnostic() const {
    const auto& phase = control->processingPhase;
    if (control->processingPhaseUnproven || (phase && phase->isUnproven())) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                              "the retained processing authority is unproven");
    }
    const auto currentRoot = phase ? phase->rootIdentity() : GpuProcessingRootIdentity{};
    if (currentRoot != creationRoot) {
        return makeDiagnostic(GpuOcioProgramDiagnosticCode::InvalidArgument,
                              "the OCIO program belongs to another processing root");
    }
    return {};
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
    auto authority = impl.authorityDiagnostic();
    if (authority.code != GpuOcioProgramDiagnosticCode::None) {
        return authority;
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
    impl.beginImpl(false, std::move(input), uniformBytes, byteBudget);
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
    auto authority = impl.authorityDiagnostic();
    if (authority.code != GpuOcioProgramDiagnosticCode::None) {
        return authority;
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
    impl.beginImpl(true, std::move(input), uniformBytes, byteBudget);
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
    const bool pending = impl.jobState == GpuOcioProgramJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return impl.jobState == GpuOcioProgramJobState::Ready ? GpuOcioProgramPollResult::Ready
                                                              : GpuOcioProgramPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult observed = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    const VkResult status =
        observed != VK_ERROR_DEVICE_LOST &&
                ocio_program_detail::ocioFaultJobFenceUnknown().load(std::memory_order_acquire)
            ? VK_ERROR_UNKNOWN
            : observed;
    if (status == VK_NOT_READY) {
        return pending ? GpuOcioProgramPollResult::Pending : GpuOcioProgramPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.fail(GpuOcioProgramDiagnosticCode::DeviceLost, "the device was lost during dispatch");
        return GpuOcioProgramPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (pending)
            impl.fail(GpuOcioProgramDiagnosticCode::NativeTimeout, "the fence query failed");
        return GpuOcioProgramPollResult::Failure;
    }
    impl.queueSubmitted = false;
    // A failed job polls only for retirement; successful proof never resurrects its pixels.
    if (!pending)
        return GpuOcioProgramPollResult::Failure;
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
    // A cancellation observed after Ready belongs to the consumed job, not the next reuse.
    impl_->discardRequested.store(false);
    impl_->jobState = GpuOcioProgramJobState::Idle;
    impl_->jobDiagnostic = {};
    return output;
}

GpuDisplayImage GpuOcioProgram::takeDisplayOutput() noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread() ||
        impl_->jobState != GpuOcioProgramJobState::Ready || impl_->displayOutput == nullptr) {
        return {};
    }
    const auto& current = impl_->control->processingPhase;
    if (impl_->control->processingPhaseUnproven || (current && current->isUnproven()) ||
        (current ? current->rootIdentity() : GpuProcessingRootIdentity{}) != impl_->creationRoot)
        return {};
    // The packed intermediate survives in the retained program for same-size reuse. Only actual
    // fence-ready output publication transfers it out of this job's phase; an unknown/pending
    // submission keeps its provisional lease through the existing owner quarantine.
    if (impl_->sampledPublished && impl_->packed.storageToken.bytes() != 0 &&
        (!impl_->packed.storageToken.canPromote() || !impl_->packed.storageToken.promote()))
        return {};
    GpuDisplayImage output = std::move(*impl_->displayOutput);
    impl_->displayOutput.reset();
    impl_->inputRetained.reset();
    // A cancellation observed after Ready belongs to the consumed job, not the next reuse.
    impl_->discardRequested.store(false);
    impl_->jobState = GpuOcioProgramJobState::Idle;
    impl_->jobDiagnostic = {};
    return output;
}

bool GpuOcioProgram::discardRetiredFailure(GpuDevice& device) noexcept {
    if (!impl_ || !impl_->onOwnerThread() || !isBoundTo(device) ||
        impl_->jobState != GpuOcioProgramJobState::Failure || impl_->queueSubmitted ||
        impl_->deviceLost || impl_->control->processingPhaseUnproven)
        return false;
    impl_->effectOutput.reset();
    impl_->displayOutput.reset();
    static_cast<void>(retireFailedAttemptGpuImage(impl_->inputRetained, device));
    impl_->releaseTransient();
    return true;
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
