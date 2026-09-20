#pragma once

// Shared OCIO context for the bounded GPU coverage fixtures. The exact production
// runtime::GpuOcioContextResolver is built from the packaged executable-relative shader tools
// staged beside THIS gate executable (bloom_package_gpu_shader_tools), never from PATH, an
// environment variable, or a guessed path. resolve() runs once and the immutable context (its
// single preparer and validated tool paths) is shared by every effect/working-space fixture.
// Without qualified tools the context is empty and a non-identity transform fails closed exactly as
// production does, so the gate can never fake a prepared colour transform.

#include <bloom/color/gpu_shader_tool_resolver.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace bloom::gpu_coverage_ocio {

[[nodiscard]] inline bool toolsAvailable() noexcept {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    return true;
#else
    return false;
#endif
}

// The typed build-time reason the packaged shader tools are absent (CPU stub, unqualified mode,
// missing prefix, or missing tools). Only meaningful when toolsAvailable() is false; it names the
// environmental cause so the gate reports an explicit NotRun rather than a generic hole. The
// value comes from bloom_package_gpu_shader_tools, never from probing PATH.
[[nodiscard]] inline std::string_view unavailableReason() noexcept {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    return {};
#elif defined(BLOOM_GPU_TOOLS_UNAVAILABLE_REASON)
    return BLOOM_GPU_TOOLS_UNAVAILABLE_REASON;
#else
    return "shader-tools-not-packaged";
#endif
}

// The one immutable context resolved from the real packaged tools. A function-local static keeps
// the single resolve and the shared preparer alive for the whole gate run.
[[nodiscard]] inline std::shared_ptr<const bloom::runtime::GpuSceneOcioContext> sharedContext() {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    static const std::shared_ptr<const bloom::runtime::GpuSceneOcioContext> context = [] {
        namespace color = bloom::color;
        bloom::runtime::GpuOcioContextRequest request;
        request.applicationExecutable = std::filesystem::path{BLOOM_GPU_COVERAGE_TEST_EXECUTABLE};
        request.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
        request.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
        request.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
        request.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
        request.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
        request.toolPackage.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
        request.toolPackage.glslangStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
            std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
        request.toolPackage.spirvValStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
            std::string_view{BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256}.substr(7));
#endif
        bloom::runtime::GpuOcioContextResolver resolver(std::move(request));
        const auto resolved = resolver.resolve();
        return resolved.hasValue() ? resolved.context
                                   : std::shared_ptr<const bloom::runtime::GpuSceneOcioContext>{};
    }();
    return context;
#else
    return {};
#endif
}

// A value copy for the CpuGpuSceneBuilder constructor; empty when the tools are unavailable.
[[nodiscard]] inline bloom::runtime::GpuSceneOcioContext context() {
    const auto shared = sharedContext();
    return shared != nullptr ? *shared : bloom::runtime::GpuSceneOcioContext{};
}

} // namespace bloom::gpu_coverage_ocio
