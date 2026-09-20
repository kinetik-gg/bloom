#pragma once

// Shared composition-root helper: turns a target's own BLOOM_GPU_TOOLS_* packaging definitions
// into the immutable GpuShaderToolPackage and an inert GpuOcioContextResolver. Each production
// root (desktop, bloom-cli, bloom-mcp) includes this header so the package it composes is derived
// from ITS OWN target definitions -- never PATH, an environment variable, or a workspace path.
//
// The resolver it returns is inert: constructing it performs no hashing, filesystem access, or
// compilation. The owning GpuExportProvider runs the one resolve() on a CPU worker during its
// bootstrap, so no resolver work ever touches the UI thread or a GPU owner thread.

#include <bloom/color/gpu_shader_tool_resolver.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace bloom::host {

[[nodiscard]] inline std::optional<core::Sha256Digest>
parsePackagedToolDigest(const std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + core::kSha256HexCharacters ||
        text.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return core::Sha256Digest::fromLowercaseHex(text.substr(prefix.size()));
}

// The immutable packaging descriptor this target was built with. Nothing is inferred at runtime.
[[nodiscard]] inline std::optional<color::GpuShaderToolPackage> packagedGpuShaderTools() {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    color::GpuShaderToolPackage package;
    package.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    package.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    package.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    package.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
#if defined(BLOOM_GPU_TOOLS_RELOCATED)
    package.relocated = BLOOM_GPU_TOOLS_RELOCATED != 0;
#endif
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
    package.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
    package.glslangStagedDigest = parsePackagedToolDigest(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256);
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
    package.spirvValStagedDigest = parsePackagedToolDigest(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256);
#endif
    return package;
#else
    return std::nullopt;
#endif
}

// The running executable's own absolute path. On every platform this is the process image itself,
// never an ambient lookup: the tools directory is resolved relative to this path by
// color::GpuShaderToolResolver.
[[nodiscard]] inline std::filesystem::path currentExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const auto length =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(size);
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer));
#else
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    return path;
#endif
}

// An inert resolver for this target's packaged tools. Returns null when the target was built
// without packaged tools (the explicit CPU-fallback build) so callers keep the references they
// already had. It performs no qualification work here.
[[nodiscard]] inline std::shared_ptr<runtime::GpuOcioContextResolver>
makePackagedGpuOcioResolver(std::filesystem::path applicationExecutable,
                            runtime::GpuOcioPreparerBudgets preparerBudgets = {}) {
    const auto package = packagedGpuShaderTools();
    if (!package.has_value() || applicationExecutable.empty()) {
        return nullptr;
    }
    runtime::GpuOcioContextRequest request;
    request.applicationExecutable = std::move(applicationExecutable);
    request.toolPackage = *package;
    request.preparerBudgets = preparerBudgets;
    return std::make_shared<runtime::GpuOcioContextResolver>(std::move(request));
}

} // namespace bloom::host
