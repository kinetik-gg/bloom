// OCIO ProcessEffect execution for GpuSceneExecutor: acquire (or reuse) the retained native program
// for a command identity, dispatch one effect job on the device owner thread, and report
// retirement. Kept in its own translation unit so gpu_scene_executor_execution.cpp stays cohesive
// and under the project's line budget. It never reads back a full frame and never compiles or
// re-writes shader text; the immutable PreparedGpuOcioCommand was compiled off the UI thread before
// dispatch.

#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_ocio_program_retained_bytes.hpp"
#include "gpu_scene_executor_private.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::diagnosticFromOcio;
using gpu_scene_executor_detail::makeDiagnostic;

[[nodiscard]] std::vector<std::uint32_t> spirvWords(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    if (!words.empty()) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }
    return words;
}

} // namespace

render::GpuOcioProgram*
GpuSceneExecutor::Impl::acquireOcioProgram(const PreparedGpuOcioCommand& command) {
    if (deviceEpoch != device->ownershipEpoch()) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic =
            makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidArgument,
                           "the OCIO executor belongs to another device generation");
        return nullptr;
    }
    const auto phase = device->retainedProcessingPhase();
    if (phase && phase->isUnproven()) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic = makeDiagnostic(GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                                               "the retained processing authority is unproven");
        return nullptr;
    }
    const auto root = phase ? phase->rootIdentity() : render::GpuProcessingRootIdentity{};
    // Keyed by everything but the per-request data bytes; startOcioStep uploads the command's own.
    const auto& identity = command.nativeProgramKey();
    const auto found = ocioPrograms.find(identity);
    if (found != ocioPrograms.end()) {
        if (found->second.root != root || found->second.epoch != device->ownershipEpoch() ||
            !found->second.program->isBoundTo(*device) ||
            found->second.program->creationRootIdentity() != root) {
            ++counters.ocioProgramRefusals;
            ocioAcquireDiagnostic = makeDiagnostic(
                GpuSceneExecutorDiagnosticCode::InvalidArgument,
                "the cached OCIO program belongs to another processing root or generation");
            return nullptr;
        }
        found->second.serial = ++ocioProgramSerial;
        ++counters.ocioProgramReuses;
        return found->second.program.get();
    }
    if (budgets.maxOcioPrograms == 0 || budgets.maxOcioRetainedProgramBytes == 0) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic = makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                                               "the OCIO native-program cache is disabled");
        return nullptr;
    }
    auto words = spirvWords(command.artifact().spirv);
    const render::GpuOcioProgramBudgets programBudgets{
        .maxOwnedBytes = budgets.maxOcioOwnedBytesPerProgram,
        .maxLutBytes = budgets.maxOcioLutBytesPerProgram,
    };
    auto created = render::GpuOcioProgram::create(
        *device, command.program(), words, programBudgets, [this]() { return cancelRequested; },
        phase);
    if (!created) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic = diagnosticFromOcio(created.diagnostic);
        return nullptr;
    }
    // Charge the ACTUAL native VMA retained allocation bytes, never a descriptor sample estimate.
    const std::uint64_t retainedBytes =
        gpu_ocio_detail::nativeRetainedAllocationBytes(*created.program);
    if (retainedBytes > budgets.maxOcioRetainedProgramBytes) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic =
            makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                           "the OCIO program's retained allocation exceeds the cache byte budget");
        return nullptr;
    }
    while (!ocioPrograms.empty() &&
           (ocioPrograms.size() >= budgets.maxOcioPrograms ||
            ocioProgramBytes > budgets.maxOcioRetainedProgramBytes - retainedBytes)) {
        auto victim = ocioPrograms.begin();
        for (auto candidate = ocioPrograms.begin(); candidate != ocioPrograms.end(); ++candidate) {
            if (candidate->second.serial < victim->second.serial) {
                victim = candidate;
            }
        }
        ocioProgramBytes -= victim->second.retainedBytes;
        ocioPrograms.erase(victim);
        ++counters.ocioProgramEvictions;
    }
    auto inserted = ocioPrograms.emplace(
        identity, OcioProgramEntry{std::move(created.program), root, device->ownershipEpoch(),
                                   retainedBytes, ++ocioProgramSerial});
    if (!inserted.second) {
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic = makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                               "the OCIO program cache insertion did not succeed");
        return nullptr;
    }
    ocioProgramBytes += retainedBytes;
    if (!inserted.first->second.program->publishSampledStorage()) {
        ocioProgramBytes -= retainedBytes;
        ocioPrograms.erase(inserted.first);
        counters.ocioRetainedProgramBytes = ocioProgramBytes;
        ++counters.ocioProgramRefusals;
        ocioAcquireDiagnostic = makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                               "the retained LUT storage could not be published");
        return nullptr;
    }
    ++counters.ocioProgramCreations;
    counters.ocioRetainedProgramBytes = ocioProgramBytes;
    return inserted.first->second.program.get();
}

GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::startOcioStep(const GpuSceneExecutorStep& step) {
    if (step.ocioCommand == nullptr || step.input == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(step.input) >= images.size() || images[step.input] == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "an OCIO effect step is incomplete before dispatch");
    }
    render::GpuOcioProgram* const program = acquireOcioProgram(*step.ocioCommand);
    if (program == nullptr) {
        return ocioAcquireDiagnostic;
    }
    const auto& data = step.ocioCommand->program().uniformBufferData;
    const auto native = program->beginEffect(images[step.input],
                                             std::span<const std::byte>(data.data(), data.size()),
                                             remainingBudget());
    if (native.code != render::GpuOcioProgramDiagnosticCode::None) {
        return diagnosticFromOcio(native);
    }
    nativeOcioProgram = program;
    ++counters.ocioEffectDispatches;
    return {};
}

std::optional<render::GpuImage> GpuSceneExecutor::Impl::takeOcioOutput() {
    if (nativeOcioProgram == nullptr) {
        return std::nullopt;
    }
    nativeOcioLastJobBytes = nativeOcioProgram->lastJobAllocationBytes();
    auto output = nativeOcioProgram->takeEffectOutput();
    nativeOcioProgram = nullptr;
    if (output == nullptr) {
        return std::nullopt;
    }
    return std::optional<render::GpuImage>(std::move(*output));
}

bool GpuSceneExecutor::Impl::ocioProgramsUnretired() const noexcept {
    for (const auto& [key, entry] : ocioPrograms) {
        static_cast<void>(key);
        if (entry.program != nullptr && entry.program->hasUnretiredSubmission()) {
            return true;
        }
    }
    return false;
}

void GpuSceneExecutor::Impl::drainOcioPrograms() noexcept {
    for (auto& [key, entry] : ocioPrograms) {
        static_cast<void>(key);
        if (entry.program != nullptr) {
            static_cast<void>(entry.program->poll());
        }
    }
}

} // namespace bloom::runtime
