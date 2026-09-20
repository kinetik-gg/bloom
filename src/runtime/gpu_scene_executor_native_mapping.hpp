#ifndef BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_NATIVE_MAPPING_HPP
#define BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_NATIVE_MAPPING_HPP

// Private to src/runtime. Translates the native render-pipeline diagnostics and poll results onto
// the executor's own diagnostic and poll vocabulary, including the executor's
// device/allocation/shader failure classification. Kept out of gpu_scene_executor_private.hpp so
// that header stays a cohesive shared step vocabulary; this is the single place that knows every
// native pipeline's failure codes.

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/gpu_point_resample.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace bloom::runtime::gpu_scene_executor_detail {

[[nodiscard]] inline GpuSceneExecutorDiagnostic
makeDiagnostic(const GpuSceneExecutorDiagnosticCode code, std::string message) {
    return GpuSceneExecutorDiagnostic{code, std::move(message)};
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromSolid(const render::GpuSolidDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuSolidDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuSolidDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuSolidDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromComposite(const render::GpuCompositeDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuCompositeDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuCompositeDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuCompositeDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromUpload(const render::GpuImageUploadDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuImageUploadDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::DeviceUnavailable:
    case render::GpuImageUploadDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::Unsupported:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromAffine(const render::GpuAffineDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuAffineDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuAffineDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuAffineDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuAffineDiagnosticCode::DeviceUnavailable:
    case render::GpuAffineDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuAffineDiagnosticCode::Unsupported:
    case render::GpuAffineDiagnosticCode::ShaderRejected:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromBlend(const render::GpuBlendDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuBlendDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuBlendDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuBlendDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuBlendDiagnosticCode::DeviceUnavailable:
    case render::GpuBlendDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuBlendDiagnosticCode::Unsupported:
    case render::GpuBlendDiagnosticCode::ShaderRejected:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromPointResample(const render::GpuPointResampleDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuPointResampleDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuPointResampleDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuPointResampleDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuPointResampleDiagnosticCode::DeviceUnavailable:
    case render::GpuPointResampleDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuPointResampleDiagnosticCode::Unsupported:
    case render::GpuPointResampleDiagnosticCode::ShaderRejected:
    case render::GpuPointResampleDiagnosticCode::InvalidArgument:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromOcio(const render::GpuOcioProgramDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuOcioProgramDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuOcioProgramDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuOcioProgramDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuOcioProgramDiagnosticCode::DeviceUnavailable:
    case render::GpuOcioProgramDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuOcioProgramDiagnosticCode::Unsupported:
    case render::GpuOcioProgramDiagnosticCode::ShaderRejected:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

enum class NativePoll : std::uint8_t { Pending, Ready, Failure, WrongThread };

[[nodiscard]] inline NativePoll mapSolid(const render::GpuSolidPollResult result) noexcept {
    switch (result) {
    case render::GpuSolidPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuSolidPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuSolidPollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuSolidPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapComposite(const render::GpuCompositePollResult result) noexcept {
    switch (result) {
    case render::GpuCompositePollResult::Pending:
        return NativePoll::Pending;
    case render::GpuCompositePollResult::Ready:
        return NativePoll::Ready;
    case render::GpuCompositePollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuCompositePollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapUpload(const render::GpuImageUploadPollResult result) noexcept {
    switch (result) {
    case render::GpuImageUploadPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuImageUploadPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuImageUploadPollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuImageUploadPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapAffine(const render::GpuAffinePollResult result) noexcept {
    switch (result) {
    case render::GpuAffinePollResult::Pending:
        return NativePoll::Pending;
    case render::GpuAffinePollResult::Ready:
        return NativePoll::Ready;
    case render::GpuAffinePollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuAffinePollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapBlend(const render::GpuBlendPollResult result) noexcept {
    switch (result) {
    case render::GpuBlendPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuBlendPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuBlendPollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuBlendPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapOcio(const render::GpuOcioProgramPollResult result) noexcept {
    switch (result) {
    case render::GpuOcioProgramPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuOcioProgramPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuOcioProgramPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll
mapPointResample(const render::GpuPointResamplePollResult result) noexcept {
    switch (result) {
    case render::GpuPointResamplePollResult::Pending:
        return NativePoll::Pending;
    case render::GpuPointResamplePollResult::Ready:
        return NativePoll::Ready;
    case render::GpuPointResamplePollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuPointResamplePollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

} // namespace bloom::runtime::gpu_scene_executor_detail

#endif // BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_NATIVE_MAPPING_HPP
