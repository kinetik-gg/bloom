#include <bloom/runtime/gpu_ocio_program_executor.hpp>

#include "gpu_ocio_program_retained_bytes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuOcioExecutorDiagnostic makeDiagnostic(const GpuOcioExecutorDiagnosticCode code,
                                                       std::string message) {
    GpuOcioExecutorDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] GpuOcioExecutorDiagnosticCode
mapProgramCode(const render::GpuOcioProgramDiagnosticCode code) noexcept {
    switch (code) {
    case render::GpuOcioProgramDiagnosticCode::None:
        return GpuOcioExecutorDiagnosticCode::None;
    case render::GpuOcioProgramDiagnosticCode::InvalidArgument:
        return GpuOcioExecutorDiagnosticCode::InvalidArgument;
    case render::GpuOcioProgramDiagnosticCode::Unsupported:
        return GpuOcioExecutorDiagnosticCode::Unsupported;
    case render::GpuOcioProgramDiagnosticCode::OverBudget:
        return GpuOcioExecutorDiagnosticCode::OverBudget;
    case render::GpuOcioProgramDiagnosticCode::Busy:
        return GpuOcioExecutorDiagnosticCode::Busy;
    case render::GpuOcioProgramDiagnosticCode::WrongThread:
        return GpuOcioExecutorDiagnosticCode::WrongThread;
    case render::GpuOcioProgramDiagnosticCode::DeviceUnavailable:
        return GpuOcioExecutorDiagnosticCode::DeviceUnavailable;
    case render::GpuOcioProgramDiagnosticCode::DeviceLost:
        return GpuOcioExecutorDiagnosticCode::DeviceLost;
    case render::GpuOcioProgramDiagnosticCode::ShaderRejected:
    case render::GpuOcioProgramDiagnosticCode::AllocationFailed:
        return GpuOcioExecutorDiagnosticCode::ProgramRefused;
    case render::GpuOcioProgramDiagnosticCode::Cancelled:
        return GpuOcioExecutorDiagnosticCode::Cancelled;
    case render::GpuOcioProgramDiagnosticCode::NativeTimeout:
        return GpuOcioExecutorDiagnosticCode::NativeTimeout;
    case render::GpuOcioProgramDiagnosticCode::ForeignInput:
        return GpuOcioExecutorDiagnosticCode::InvalidArgument;
    }
    return GpuOcioExecutorDiagnosticCode::InternalInvariant;
}

[[nodiscard]] std::vector<std::uint32_t> toWords(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    if (!words.empty()) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }
    return words;
}

struct ProgramEntry final {
    std::unique_ptr<render::GpuOcioProgram> program;
    std::uint64_t bytes = 0;
    std::uint64_t serial = 0;
};

} // namespace

struct GpuOcioProgramExecutor::Impl final {
    [[nodiscard]] bool onOwnerThread() const noexcept {
        return device != nullptr && device->isOwnerThread();
    }

    void fail(const GpuOcioExecutorDiagnosticCode code, std::string message) {
        jobState = GpuOcioExecutorJobState::Failure;
        jobDiagnostic = makeDiagnostic(code, std::move(message));
    }

    void clearJob() {
        jobState = GpuOcioExecutorJobState::Idle;
        jobDiagnostic = {};
        active = nullptr;
        activeCommand.reset();
        activeInput.reset();
        discardRequested.store(false);
        deadlineExpired = false;
    }

    [[nodiscard]] bool anyUnretired() const noexcept {
        if (active != nullptr && active->hasUnretiredSubmission()) {
            return true;
        }
        for (const auto& [key, entry] : programs) {
            static_cast<void>(key);
            if (entry.program != nullptr && entry.program->hasUnretiredSubmission()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] render::GpuOcioProgram*
    getOrCreateProgram(const PreparedGpuOcioCommand& command) {
        const auto& identity = command.identity();
        const auto found = programs.find(identity);
        if (found != programs.end()) {
            found->second.serial = ++serial;
            ++counters.programReuses;
            return found->second.program.get();
        }
        if (budgets.maxPrograms == 0 || budgets.maxRetainedProgramBytes == 0) {
            ++counters.programRefusals;
            fail(GpuOcioExecutorDiagnosticCode::OverBudget,
                 "the retained-program cache is disabled");
            return nullptr;
        }
        auto words = toWords(command.artifact().spirv);
        const render::GpuOcioProgramBudgets programBudgets{
            .maxOwnedBytes = budgets.maxOwnedBytesPerProgram,
            .maxLutBytes = budgets.maxLutBytesPerProgram,
        };
        auto created =
            render::GpuOcioProgram::create(*device, command.program(), words, programBudgets,
                                           [this]() { return discardRequested.load(); });
        if (!created) {
            ++counters.programRefusals;
            fail(mapProgramCode(created.diagnostic.code), created.diagnostic.message);
            return nullptr;
        }
        // Charge the ACTUAL native VMA retained allocation bytes, never a descriptor-declared
        // sample estimate. A budget below the actual bytes refuses here and the just-created
        // program is destroyed (its bounded drain runs) without entering the cache.
        const std::uint64_t retainedBytes =
            gpu_ocio_detail::nativeRetainedAllocationBytes(*created.program);
        if (retainedBytes > budgets.maxRetainedProgramBytes) {
            ++counters.programRefusals;
            fail(GpuOcioExecutorDiagnosticCode::OverBudget,
                 "the native program's retained allocation exceeds the cache byte budget");
            return nullptr;
        }
        while (!programs.empty() &&
               (programs.size() >= budgets.maxPrograms ||
                totalBytes > budgets.maxRetainedProgramBytes - retainedBytes)) {
            auto victim = programs.begin();
            for (auto candidate = programs.begin(); candidate != programs.end(); ++candidate) {
                if (candidate->second.serial < victim->second.serial) {
                    victim = candidate;
                }
            }
            totalBytes -= victim->second.bytes;
            programs.erase(victim);
            ++counters.programEvictions;
        }
        totalBytes += retainedBytes;
        auto inserted = programs.emplace(
            identity, ProgramEntry{std::move(created.program), retainedBytes, ++serial});
        ++counters.programCreations;
        return inserted.first->second.program.get();
    }

    render::GpuDevice* device = nullptr;
    std::uint64_t epoch = 0;
    GpuOcioExecutorBudgets budgets;
    std::map<core::Sha256Digest, ProgramEntry> programs;
    std::uint64_t totalBytes = 0;
    std::uint64_t serial = 0;

    render::GpuOcioProgram* active = nullptr;
    GpuOcioOutputEncoding activeEncoding = GpuOcioOutputEncoding::FinalRgba32f;
    std::shared_ptr<const PreparedGpuOcioCommand> activeCommand;
    std::shared_ptr<const render::GpuImage> activeInput;
    std::chrono::steady_clock::time_point jobStart;
    std::atomic<bool> discardRequested{false};
    bool deadlineExpired = false;
    bool deviceLost = false;
    GpuOcioExecutorJobState jobState = GpuOcioExecutorJobState::Idle;
    GpuOcioExecutorDiagnostic jobDiagnostic;
    GpuOcioExecutorCounters counters;
};

GpuOcioProgramExecutor::GpuOcioProgramExecutor(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuOcioProgramExecutor::GpuOcioProgramExecutor(GpuOcioProgramExecutor&& other) noexcept
    : impl_(std::move(other.impl_)) {}
GpuOcioProgramExecutor& GpuOcioProgramExecutor::operator=(GpuOcioProgramExecutor&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuOcioProgramExecutor::~GpuOcioProgramExecutor() { releaseImpl(); }

void GpuOcioProgramExecutor::releaseImpl() noexcept { impl_.reset(); }

GpuOcioExecutorCreateResult GpuOcioProgramExecutor::create(render::GpuDevice& device,
                                                           const GpuOcioExecutorBudgets& budgets) {
    if (device.state() != render::GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuOcioExecutorDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (!device.isOwnerThread()) {
        return {nullptr, makeDiagnostic(GpuOcioExecutorDiagnosticCode::WrongThread,
                                        "the executor must be created on the device owner thread")};
    }
    if (device.ownershipEpoch() == 0) {
        return {nullptr, makeDiagnostic(GpuOcioExecutorDiagnosticCode::DeviceUnavailable,
                                        "the device has no ownership identity")};
    }
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->epoch = device.ownershipEpoch();
    impl->budgets = budgets;
    return {std::unique_ptr<GpuOcioProgramExecutor>(new GpuOcioProgramExecutor(std::move(impl))),
            GpuOcioExecutorDiagnostic{}};
}

GpuOcioExecutorJobState GpuOcioProgramExecutor::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuOcioExecutorJobState::Failure;
}
const GpuOcioExecutorDiagnostic& GpuOcioProgramExecutor::diagnostic() const noexcept {
    if (impl_ != nullptr) {
        return impl_->jobDiagnostic;
    }
    static const GpuOcioExecutorDiagnostic unavailable =
        makeDiagnostic(GpuOcioExecutorDiagnosticCode::DeviceUnavailable, "no executor was created");
    return unavailable;
}
bool GpuOcioProgramExecutor::isBoundTo(const render::GpuDevice& device) const noexcept {
    return impl_ != nullptr && impl_->device == &device && impl_->epoch == device.ownershipEpoch();
}
GpuOcioExecutorCounters GpuOcioProgramExecutor::counters() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    GpuOcioExecutorCounters result = impl_->counters;
    result.cacheEntries = impl_->programs.size();
    result.cacheBytes = impl_->totalBytes;
    return result;
}
bool GpuOcioProgramExecutor::deviceLost() const noexcept {
    return impl_ == nullptr || impl_->deviceLost;
}
bool GpuOcioProgramExecutor::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->anyUnretired();
}
bool GpuOcioProgramExecutor::teardownDrainIncomplete() noexcept {
    return render::GpuOcioProgram::teardownDrainIncomplete();
}

GpuOcioExecutorDiagnostic
GpuOcioProgramExecutor::begin(std::shared_ptr<const PreparedGpuOcioCommand> command,
                              std::shared_ptr<const render::GpuImage> input,
                              const std::span<const std::byte> uniformOverride,
                              const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::DeviceUnavailable,
                              "no executor was created");
    }
    auto& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::DeviceLost,
                              "the device generation was lost");
    }
    if (impl.jobState == GpuOcioExecutorJobState::Pending) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::Busy, "a job is already in flight");
    }
    if (impl.anyUnretired()) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::OwnerDrainRequired,
                              "a previous submission is not proven retired");
    }
    if (impl.jobState != GpuOcioExecutorJobState::Idle) {
        impl.clearJob();
    }
    if (command == nullptr || input == nullptr) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::InvalidArgument,
                              "the command and input are required");
    }
    if (!input->isValid() || !input->isBoundTo(*impl.device)) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::InvalidArgument,
                              "the input is not a resident image of this device");
    }
    const auto geometry = command->geometry();
    if (input->width() != geometry.width || input->height() != geometry.height) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::IdentityMismatch,
                              "the input geometry does not match the command geometry");
    }
    const auto& program = command->program();
    if (!uniformOverride.empty() && uniformOverride.size() != program.uniformBufferSize) {
        return makeDiagnostic(GpuOcioExecutorDiagnosticCode::InvalidArgument,
                              "the uniform override does not match the declared UBO size");
    }
    render::GpuOcioProgram* const native = impl.getOrCreateProgram(*command);
    if (native == nullptr) {
        return impl.jobDiagnostic;
    }
    const bool display = command->encoding() == GpuOcioOutputEncoding::DisplayRgba8;
    const auto nativeDiagnostic = display ? native->beginDisplay(input, uniformOverride, byteBudget)
                                          : native->beginEffect(input, uniformOverride, byteBudget);
    if (nativeDiagnostic.code != render::GpuOcioProgramDiagnosticCode::None) {
        const auto code = mapProgramCode(nativeDiagnostic.code);
        if (code == GpuOcioExecutorDiagnosticCode::OverBudget) {
            ++impl.counters.budgetRefusals;
        }
        return makeDiagnostic(code, nativeDiagnostic.message);
    }
    impl.active = native;
    impl.activeEncoding = command->encoding();
    impl.activeCommand = std::move(command);
    impl.activeInput = std::move(input);
    impl.jobStart = std::chrono::steady_clock::now();
    impl.discardRequested.store(false);
    impl.deadlineExpired = false;
    impl.jobState = GpuOcioExecutorJobState::Pending;
    impl.jobDiagnostic = {};
    ++impl.counters.commandsAccepted;
    ++impl.counters.dispatches;
    if (display) {
        ++impl.counters.displayDispatches;
    } else {
        ++impl.counters.effectDispatches;
    }
    return {};
}

GpuOcioExecutorPollResult GpuOcioProgramExecutor::poll() {
    if (impl_ == nullptr) {
        return GpuOcioExecutorPollResult::Failure;
    }
    auto& impl = *impl_;
    if (!impl.onOwnerThread()) {
        impl.fail(GpuOcioExecutorDiagnosticCode::WrongThread, "poll must run on the owner thread");
        return GpuOcioExecutorPollResult::WrongThread;
    }
    if (impl.jobState != GpuOcioExecutorJobState::Pending) {
        return impl.jobState == GpuOcioExecutorJobState::Ready ? GpuOcioExecutorPollResult::Ready
                                                               : GpuOcioExecutorPollResult::Failure;
    }
    const auto deadline = std::chrono::milliseconds(impl.budgets.jobDeadlineMilliseconds);
    if (!impl.deadlineExpired && deadline.count() > 0 &&
        std::chrono::steady_clock::now() - impl.jobStart > deadline) {
        impl.deadlineExpired = true;
        impl.active->cancel();
    }
    const auto nativeResult = impl.active->poll();
    if (nativeResult == render::GpuOcioProgramPollResult::Pending) {
        return GpuOcioExecutorPollResult::Pending;
    }
    if (nativeResult == render::GpuOcioProgramPollResult::Ready) {
        impl.jobState = GpuOcioExecutorJobState::Ready;
        ++impl.counters.jobCompletions;
        return GpuOcioExecutorPollResult::Ready;
    }
    const auto nativeDiagnostic = impl.active->diagnostic();
    const auto code = impl.deadlineExpired ? GpuOcioExecutorDiagnosticCode::NativeTimeout
                                           : mapProgramCode(nativeDiagnostic.code);
    if (code == GpuOcioExecutorDiagnosticCode::DeviceLost) {
        impl.deviceLost = true;
    }
    if (code == GpuOcioExecutorDiagnosticCode::Cancelled) {
        ++impl.counters.jobCancellations;
    } else if (code == GpuOcioExecutorDiagnosticCode::NativeTimeout) {
        ++impl.counters.jobTimeouts;
    } else {
        ++impl.counters.jobFailures;
    }
    impl.fail(code, nativeDiagnostic.message);
    return GpuOcioExecutorPollResult::Failure;
}

std::shared_ptr<render::GpuImage> GpuOcioProgramExecutor::takeEffectOutput() noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuOcioExecutorJobState::Ready ||
        impl_->activeEncoding != GpuOcioOutputEncoding::FinalRgba32f || impl_->active == nullptr) {
        return {};
    }
    auto output = impl_->active->takeEffectOutput();
    impl_->clearJob();
    return output;
}

std::optional<render::GpuDisplayImage> GpuOcioProgramExecutor::takeDisplayOutput() noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuOcioExecutorJobState::Ready ||
        impl_->activeEncoding != GpuOcioOutputEncoding::DisplayRgba8 || impl_->active == nullptr) {
        return std::nullopt;
    }
    auto output = impl_->active->takeDisplayOutput();
    impl_->clearJob();
    return std::optional<render::GpuDisplayImage>(std::move(output));
}

void GpuOcioProgramExecutor::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
        if (impl_->active != nullptr) {
            impl_->active->cancel();
        }
    }
}

} // namespace bloom::runtime
