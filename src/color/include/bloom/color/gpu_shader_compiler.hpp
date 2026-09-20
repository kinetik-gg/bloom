#pragma once

#include <bloom/render/gpu_shader_artifact.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::color {

// Caller-provided, already-validated qualified paths to the pinned tools. The adapter never
// consults PATH, a package registry, or an ambient environment variable.
struct GpuShaderToolPaths {
    std::string glslangValidator;
    std::string spirvVal;
};

// Pre-allocation ceilings. A caller may lower these; the implementation rejects a request that
// raises any of them above the hard ceiling rather than silently accepting an unbounded value.
struct GpuShaderCompileLimits {
    std::size_t maxSourceBytes = 4u * 1024u * 1024u;
    std::size_t maxSpirvBytes = 64u * 1024u * 1024u;
    std::size_t maxDiagnosticBytes = 16u * 1024u;
    std::chrono::milliseconds deadline{10000};
    std::uint64_t addressSpaceBytes = 512ull * 1024ull * 1024ull;
    std::uint32_t openFiles = 64;
};

enum class GpuShaderCompileError {
    Unavailable,
    InvalidTool,
    LimitsInvalid,
    SourceTooLarge,
    MalformedSource,
    MissingEntryPoint,
    UnsupportedTarget,
    WrongSpirvVersion,
    ValidationFailed,
    OutputTooLarge,
    Timeout,
    Cancelled,
    SpawnFailure,
    IoFailure,
    ToolCrashed,
};

struct GpuShaderCompileFailure {
    GpuShaderCompileError code = GpuShaderCompileError::IoFailure;
    std::string diagnostic;
    std::vector<std::uint8_t> toolDiagnostic;
    int toolExitStatus = 0;
};

struct GpuShaderCompileRequest {
    std::string computeShaderText;
    std::string entryPoint = "main";
    std::string targetEnvironment = "vulkan1.2";
    GpuShaderToolPaths tools;
    GpuShaderCompileLimits limits;
};

enum class GpuShaderCompileStatus { Compiled, Failed, Cancelled };

struct GpuShaderCompileResult {
    GpuShaderCompileStatus status = GpuShaderCompileStatus::Failed;
    std::optional<render::CompiledGpuShader> artifact;
    std::optional<GpuShaderCompileFailure> failure;
};

using GpuShaderCancellation = std::function<bool()>;
using GpuShaderDeadline = std::chrono::steady_clock::time_point;

struct GpuShaderCompilerStats {
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t compiles = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t cacheBytes = 0;
};

// Runs glslangValidator and spirv-val as isolated, bounded child processes and returns the
// validated SPIR-V. Blocking: call only from a blocking-I/O worker, never the UI thread or a
// native GPU owner thread. Thread-safe.
class GpuShaderCompiler final {
  public:
    // The result cache is keyed on the complete source bytes, entry point, target environment, and
    // compiler/validator executable identities. `maxCacheEntries` and `maxCacheBytes` bound both
    // the entry count and the resident SPIR-V bytes actually retained (an artifact larger than the
    // byte budget is returned but not cached).
    explicit GpuShaderCompiler(std::size_t maxCacheEntries = 64,
                               std::size_t maxCacheBytes = 64u * 1024u * 1024u);
    ~GpuShaderCompiler();
    GpuShaderCompiler(const GpuShaderCompiler&) = delete;
    GpuShaderCompiler& operator=(const GpuShaderCompiler&) = delete;

    [[nodiscard]] GpuShaderCompileResult compile(const GpuShaderCompileRequest& request,
                                                 const GpuShaderCancellation& cancel = {});
    [[nodiscard]] GpuShaderCompilerStats stats() const;

  private:
    struct State;
    struct ToolResolution {
        std::optional<render::GpuShaderToolIdentity> identity;
        std::optional<GpuShaderCompileError> error;
    };
    [[nodiscard]] ToolResolution resolveTool(const std::string& role, const std::string& path,
                                             const GpuShaderCompileLimits& limits,
                                             GpuShaderDeadline deadline,
                                             const GpuShaderCancellation& cancel);
    [[nodiscard]] std::optional<render::CompiledGpuShader> lookupCache(const std::string& key);
    void insertCache(std::string key, const render::CompiledGpuShader& artifact);
    std::unique_ptr<State> state_;
};

} // namespace bloom::color
