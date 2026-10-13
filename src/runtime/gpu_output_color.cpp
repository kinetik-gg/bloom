#include <bloom/runtime/gpu_output_color.hpp>

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuOutputColorDiagnosticCode
mapExecutorBegin(const GpuOcioExecutorDiagnosticCode code) noexcept {
    switch (code) {
    case GpuOcioExecutorDiagnosticCode::None:
        return GpuOutputColorDiagnosticCode::None;
    case GpuOcioExecutorDiagnosticCode::InvalidArgument:
    case GpuOcioExecutorDiagnosticCode::IdentityMismatch:
        return GpuOutputColorDiagnosticCode::InvalidArgument;
    case GpuOcioExecutorDiagnosticCode::WrongThread:
        return GpuOutputColorDiagnosticCode::WrongThread;
    case GpuOcioExecutorDiagnosticCode::DeviceUnavailable:
        return GpuOutputColorDiagnosticCode::DeviceUnavailable;
    case GpuOcioExecutorDiagnosticCode::DeviceLost:
        return GpuOutputColorDiagnosticCode::DeviceLost;
    case GpuOcioExecutorDiagnosticCode::Busy:
    case GpuOcioExecutorDiagnosticCode::OwnerDrainRequired:
        return GpuOutputColorDiagnosticCode::Busy;
    case GpuOcioExecutorDiagnosticCode::OverBudget:
        return GpuOutputColorDiagnosticCode::OverBudget;
    case GpuOcioExecutorDiagnosticCode::Unsupported:
    case GpuOcioExecutorDiagnosticCode::ProgramRefused:
    case GpuOcioExecutorDiagnosticCode::DispatchRefused:
        return GpuOutputColorDiagnosticCode::ExecutorRefused;
    case GpuOcioExecutorDiagnosticCode::Cancelled:
        return GpuOutputColorDiagnosticCode::Cancelled;
    case GpuOcioExecutorDiagnosticCode::NativeTimeout:
    case GpuOcioExecutorDiagnosticCode::InternalInvariant:
        return GpuOutputColorDiagnosticCode::ExecutorFailed;
    }
    return GpuOutputColorDiagnosticCode::InternalInvariant;
}

[[nodiscard]] GpuOutputColorDiagnosticCode
mapReadbackCode(const render::GpuOutputColorReadbackCode code) noexcept {
    switch (code) {
    case render::GpuOutputColorReadbackCode::None:
        return GpuOutputColorDiagnosticCode::None;
    case render::GpuOutputColorReadbackCode::WrongThread:
        return GpuOutputColorDiagnosticCode::WrongThread;
    case render::GpuOutputColorReadbackCode::DeviceUnavailable:
        return GpuOutputColorDiagnosticCode::DeviceUnavailable;
    case render::GpuOutputColorReadbackCode::DeviceLost:
        return GpuOutputColorDiagnosticCode::DeviceLost;
    case render::GpuOutputColorReadbackCode::OverBudget:
        return GpuOutputColorDiagnosticCode::OverBudget;
    case render::GpuOutputColorReadbackCode::InvalidArgument:
        return GpuOutputColorDiagnosticCode::InvalidArgument;
    case render::GpuOutputColorReadbackCode::ReadbackFailed:
        return GpuOutputColorDiagnosticCode::ReadbackFailed;
    case render::GpuOutputColorReadbackCode::Cancelled:
        return GpuOutputColorDiagnosticCode::Cancelled;
    }
    return GpuOutputColorDiagnosticCode::InternalInvariant;
}

} // namespace

GpuOutputColorStage::GpuOutputColorStage(render::GpuDevice& device,
                                         std::unique_ptr<GpuOcioProgramExecutor> executor) noexcept
    : device_(&device), ownershipEpoch_(device.ownershipEpoch()), executor_(std::move(executor)),
      ownerThread_(std::this_thread::get_id()) {}

GpuOutputColorStage::~GpuOutputColorStage() = default;

GpuOutputColorCreateResult GpuOutputColorStage::create(render::GpuDevice& device,
                                                       const GpuOutputColorBudgets& budgets) {
    if (!device.isOwnerThread()) {
        return {nullptr,
                {GpuOutputColorDiagnosticCode::WrongThread,
                 "the output-colour stage must be created on the device owner thread"}};
    }
    auto executor = GpuOcioProgramExecutor::create(device, budgets.executor);
    if (!executor.hasValue()) {
        return {nullptr, {mapExecutorBegin(executor.diagnostic.code), executor.diagnostic.message}};
    }
    auto stage = std::unique_ptr<GpuOutputColorStage>(
        new GpuOutputColorStage(device, std::move(executor.executor)));
    return {std::move(stage), {}};
}

GpuOutputColorStatus GpuOutputColorStage::state() const noexcept {
    switch (phase_) {
    case Phase::Idle:
        return GpuOutputColorStatus::Idle;
    case Phase::Dispatching:
    case Phase::ReadingBack:
        return GpuOutputColorStatus::Pending;
    case Phase::Ready:
        return GpuOutputColorStatus::Ready;
    case Phase::Failure:
        return GpuOutputColorStatus::Failure;
    }
    return GpuOutputColorStatus::Failure;
}

const GpuOutputColorDiagnostic& GpuOutputColorStage::diagnostic() const noexcept {
    return diagnostic_;
}

GpuOutputColorArm GpuOutputColorStage::arm() const noexcept { return arm_; }

bool GpuOutputColorStage::isBoundTo(const render::GpuDevice& device) const noexcept {
    return executor_ != nullptr && executor_->isBoundTo(device);
}

bool GpuOutputColorStage::hasUnretiredSubmission() const noexcept {
    return (executor_ != nullptr && executor_->hasUnretiredSubmission()) ||
           readback_.hasUnretiredSubmission();
}

bool GpuOutputColorStage::retirementSafe(
    const std::shared_ptr<render::GpuProcessingPhaseLease>& phase) const noexcept {
    if (!device_ || ownerThread_ != std::this_thread::get_id() || !device_->isOwnerThread() ||
        ownershipEpoch_ != device_->ownershipEpoch() || !executor_->isBoundTo(*device_) ||
        executor_->deviceLost() || hasUnretiredSubmission() ||
        phase != device_->retainedProcessingPhase() || phase != lastPhase_.lock() ||
        (phase && phase->isUnproven()) || device_->outputColorReadbackPending() ||
        device_->displayTransferPending())
        return false;
    const auto programs = device_->ocioProgramRetirementStats();
    return programs.pendingPrograms == 0 && programs.pendingUploads == 0;
}
void GpuOutputColorStage::discardProvenFailure() noexcept {
    executor_->discardRetiredFailures();
    frame_.reset();
    command_.reset();
    process_.reset();
    phaseLease_.reset();
}
bool GpuOutputColorStage::retiredBudgetRefusal(
    const std::shared_ptr<render::GpuProcessingPhaseLease>& phase) noexcept {
    if (!(lastBudgetRefusal_ || (phase_ == Phase::Failure &&
                                 diagnostic_.code == GpuOutputColorDiagnosticCode::OverBudget)) ||
        !retirementSafe(phase))
        return false;
    // Both begin refusal and asynchronous refusal have already unwound the actual staging guard.
    // No Ready product is consumed; only proven failed executor/transient aliases are detached.
    discardProvenFailure();
    lastBudgetRefusal_ = false;
    return true;
}

bool GpuOutputColorStage::retiredReadbackFailure(
    const std::shared_ptr<render::GpuProcessingPhaseLease>& phase) noexcept {
    // The exact owner/epoch/phase and all independent witnesses are checked before mutable state.
    if (!retirementSafe(phase) || phase_ != Phase::Failure ||
        diagnostic_.code != GpuOutputColorDiagnosticCode::ReadbackFailed)
        return false;
    discardProvenFailure();
    return true;
}

GpuOutputColorDiagnostic
GpuOutputColorStage::begin(std::shared_ptr<const PreparedGpuOcioCommand> command,
                           std::shared_ptr<const render::GpuImage> processImage,
                           const std::uint64_t byteBudget,
                           const std::shared_ptr<render::GpuProcessingPhaseLease>& phase) noexcept {
    try {
        // Owner identity is consulted FIRST: a foreign-thread call must not read or mutate any
        // owner state (phase, retained command/process/frame, diagnostic, counters).
        if (ownerThread_ != std::this_thread::get_id()) {
            return {GpuOutputColorDiagnosticCode::WrongThread,
                    "begin must run on the device owner thread"};
        }
        if (!device_ || !device_->isOwnerThread() || ownershipEpoch_ != device_->ownershipEpoch() ||
            !executor_->isBoundTo(*device_) || executor_->deviceLost() ||
            phase != device_->retainedProcessingPhase() || (phase && phase->isUnproven())) {
            return {GpuOutputColorDiagnosticCode::DeviceUnavailable,
                    "output colour requires the exact proven retained processing phase"};
        }
        if (phase_ != Phase::Idle) {
            // A genuinely retired failure is reusable; a live or unretired submission is refused.
            if (phase_ == Phase::Failure && !hasUnretiredSubmission()) {
                command_.reset();
                process_.reset();
                phaseLease_.reset();
                frame_.reset();
                diagnostic_ = {};
                phase_ = Phase::Idle;
            } else {
                return {GpuOutputColorDiagnosticCode::Busy, "the stage already has an active job"};
            }
        }
        if (processImage == nullptr || !processImage->isValid()) {
            return {GpuOutputColorDiagnosticCode::InvalidArgument,
                    "a resident process image is required"};
        }
        if (command != nullptr) {
            const auto geometry = command->geometry();
            if (geometry.width != processImage->width() ||
                geometry.height != processImage->height()) {
                return {GpuOutputColorDiagnosticCode::GeometryMismatch,
                        "the OCIO command geometry does not match the process image"};
            }
        }

        lastPhase_ = phase;
        lastBudgetRefusal_ = false;
        command_ = std::move(command);
        process_ = std::move(processImage);
        byteBudget_ = byteBudget;
        phaseLease_ = phase;
        frame_.reset();
        diagnostic_ = {};
        // Captured for EVERY begin, including the identity arm, so a subsequent identity-only job
        // reports zero new program creation/dispatch rather than a preceding colour job's counters.
        executorBefore_ = executor_->counters();

        if (command_ == nullptr) {
            arm_ = GpuOutputColorArm::None;
            if (!readback_.begin(process_, nullptr, std::nullopt, byteBudget_, phaseLease_)) {
                const auto& readbackDiagnostic = readback_.diagnostic();
                lastBudgetRefusal_ =
                    readbackDiagnostic.code == render::GpuOutputColorReadbackCode::OverBudget;
                command_.reset();
                process_.reset();
                phaseLease_.reset();
                return {mapReadbackCode(readbackDiagnostic.code), readbackDiagnostic.message};
            }
            phase_ = Phase::ReadingBack;
            return {};
        }

        arm_ = command_->encoding() == GpuOcioOutputEncoding::DisplayRgba8
                   ? GpuOutputColorArm::DisplayRgba8
                   : GpuOutputColorArm::ProcessEffect;
        const auto accepted = executor_->begin(command_, process_, {}, byteBudget_);
        if (accepted.code != GpuOcioExecutorDiagnosticCode::None) {
            lastBudgetRefusal_ = accepted.code == GpuOcioExecutorDiagnosticCode::OverBudget;
            // A refused begin admitted no submission; leave the stage Idle and reusable.
            command_.reset();
            process_.reset();
            phaseLease_.reset();
            return {mapExecutorBegin(accepted.code), accepted.message};
        }
        phase_ = Phase::Dispatching;
        return {};
    } catch (const std::bad_alloc&) {
        command_.reset();
        process_.reset();
        phaseLease_.reset();
        return {GpuOutputColorDiagnosticCode::InternalInvariant,
                "the output-colour begin could not be allocated"};
    } catch (...) {
        command_.reset();
        process_.reset();
        phaseLease_.reset();
        return {GpuOutputColorDiagnosticCode::InternalInvariant,
                "the output-colour begin failed unexpectedly"};
    }
}

GpuOutputColorPollResult GpuOutputColorStage::poll() {
    // Owner identity first: a foreign-thread poll must not read or mutate owner state.
    if (ownerThread_ != std::this_thread::get_id()) {
        return GpuOutputColorPollResult::WrongThread;
    }
    if (phase_ == Phase::Idle) {
        return GpuOutputColorPollResult::Pending;
    }
    if (phase_ == Phase::Ready) {
        return GpuOutputColorPollResult::Ready;
    }
    if (phase_ == Phase::Failure) {
        return GpuOutputColorPollResult::Failure;
    }
    const auto failPoll = [this](const GpuOutputColorDiagnosticCode code, std::string message) {
        diagnostic_.code = code;
        diagnostic_.message = std::move(message);
        phase_ = Phase::Failure;
        if (code == GpuOutputColorDiagnosticCode::Cancelled && retirementSafe(phaseLease_))
            discardProvenFailure();
        return GpuOutputColorPollResult::Failure;
    };

    if (phase_ == Phase::Dispatching) {
        const auto result = executor_->poll();
        if (result == GpuOcioExecutorPollResult::Pending) {
            return GpuOutputColorPollResult::Pending;
        }
        if (result == GpuOcioExecutorPollResult::WrongThread) {
            return GpuOutputColorPollResult::WrongThread;
        }
        if (result == GpuOcioExecutorPollResult::Failure) {
            const auto& executorDiagnostic = executor_->diagnostic();
            return failPoll(mapExecutorBegin(executorDiagnostic.code), executorDiagnostic.message);
        }
        if (arm_ == GpuOutputColorArm::ProcessEffect) {
            auto effect = executor_->takeEffectOutput();
            if (effect == nullptr) {
                return failPoll(GpuOutputColorDiagnosticCode::InternalInvariant,
                                "the effect dispatch published no resident output");
            }
            if (!readback_.begin(process_, std::move(effect), std::nullopt, byteBudget_,
                                 phaseLease_)) {
                const auto& readbackDiagnostic = readback_.diagnostic();
                return failPoll(mapReadbackCode(readbackDiagnostic.code),
                                readbackDiagnostic.message);
            }
        } else {
            auto display = executor_->takeDisplayOutput();
            if (!display.has_value()) {
                return failPoll(GpuOutputColorDiagnosticCode::InternalInvariant,
                                "the display dispatch published no resident output");
            }
            if (!readback_.begin(process_, nullptr, std::move(display), byteBudget_, phaseLease_)) {
                const auto& readbackDiagnostic = readback_.diagnostic();
                return failPoll(mapReadbackCode(readbackDiagnostic.code),
                                readbackDiagnostic.message);
            }
        }
        phase_ = Phase::ReadingBack;
        return GpuOutputColorPollResult::Pending;
    }

    // ReadingBack.
    const auto readbackState = readback_.poll();
    if (readbackState == render::GpuOutputColorReadbackState::Pending) {
        return GpuOutputColorPollResult::Pending;
    }
    if (readbackState != render::GpuOutputColorReadbackState::Ready) {
        const auto& readbackDiagnostic = readback_.diagnostic();
        return failPoll(mapReadbackCode(readbackDiagnostic.code), readbackDiagnostic.message);
    }

    auto payloads = readback_.take(false);
    GpuOutputColorFrame frame;
    frame.process = std::move(payloads.process);
    frame.effectRgba32f = std::move(payloads.encodedRgba32f);
    frame.displayRgba8 = std::move(payloads.encodedRgba8);
    frame.width = process_->width();
    frame.height = process_->height();
    frame.dataWindow = process_->dataWindow();
    frame.displayWindow = process_->displayWindow();
    frame.pixelAspect = process_->pixelAspect();
    frame.arm = arm_;
    if (command_ != nullptr) {
        frame.commandIdentity = command_->identity();
    }
    frame.counters.readbackSubmissions = payloads.counters.submissions;
    frame.counters.transferredPayloads = payloads.counters.payloads;
    frame.counters.transferredBytes = payloads.counters.bytes;
    frame.counters.processPayloadBytes = payloads.counters.processBytes;
    frame.counters.encodedPayloadBytes = payloads.counters.encodedBytes;
    frame.counters.commandAccepted = command_ != nullptr ? 1U : 0U;
    const auto after = executor_->counters();
    frame.counters.programCreations = after.programCreations - executorBefore_.programCreations;
    frame.counters.programReuses = after.programReuses - executorBefore_.programReuses;
    frame.counters.dispatches = after.dispatches - executorBefore_.dispatches;

    frame_ = std::move(frame);
    phase_ = Phase::Ready;
    return GpuOutputColorPollResult::Ready;
}

std::optional<GpuOutputColorFrame> GpuOutputColorStage::take(const bool publishStorage) noexcept {
    // Owner identity first: a foreign-thread take must never move/destroy the owner's frame.
    if (ownerThread_ != std::this_thread::get_id()) {
        return std::nullopt;
    }
    if (phase_ != Phase::Ready || !frame_.has_value()) {
        return std::nullopt;
    }
    if (publishStorage && frame_->process.authority() &&
        !render::PackedHostStorageAuthority::promoteFinalProduct(
            frame_->process.authority(), frame_->arm == GpuOutputColorArm::DisplayRgba8
                                             ? frame_->displayRgba8.authority()
                                             : frame_->effectRgba32f.authority())) {
        frame_.reset();
        phaseLease_.reset();
        process_.reset();
        command_.reset();
        phase_ = Phase::Failure;
        diagnostic_ = {GpuOutputColorDiagnosticCode::DeviceUnavailable,
                       "the complete final host product could not be published"};
        return std::nullopt;
    }
    std::optional<GpuOutputColorFrame> frame = std::move(frame_);
    frame_.reset();
    command_.reset();
    process_.reset();
    phaseLease_.reset();
    diagnostic_ = {};
    phase_ = Phase::Idle;
    return frame;
}

void GpuOutputColorStage::cancel() noexcept {
    // Owner identity first: a foreign-thread cancel must not touch the owner's in-flight job.
    if (ownerThread_ != std::this_thread::get_id()) {
        return;
    }
    if (phase_ == Phase::Dispatching) {
        executor_->cancel();
    } else if (phase_ == Phase::ReadingBack) {
        readback_.cancel();
    }
}

} // namespace bloom::runtime
