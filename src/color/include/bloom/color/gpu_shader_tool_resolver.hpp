#pragma once

#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/core/sha256.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::color {

// Immutable packaging facts the application composition root constructs from the packaging macros
// (BLOOM_GPU_TOOLS_*). Nothing here is inferred from PATH or an environment variable.
struct GpuShaderToolPackage {
    std::string toolsDirectory; // BLOOM_GPU_TOOLS_DIR ("bloom-gpu-tools")
    std::string inventoryName;  // BLOOM_GPU_TOOLS_INVENTORY_NAME ("inventory.json")
    std::string glslangValidatorName;
    std::string spirvValName;
    bool relocated = false;                                // staged bytes rewritten after copy
    bool bundleRelative = false;                           // macOS .app layout (exe-relative)
    std::optional<core::Sha256Digest> glslangStagedDigest; // expected bytes when !relocated
    std::optional<core::Sha256Digest> spirvValStagedDigest;
};

struct GpuShaderToolResolveLimits {
    std::size_t maxToolBytes = std::size_t{512} * 1024u * 1024u;
    std::size_t maxInventoryBytes = std::size_t{256} * 1024u;
    std::chrono::milliseconds deadline{5000};
};

enum class GpuShaderToolResolveError {
    Disabled,
    InvalidPackage,
    LimitsInvalid,
    InvalidExecutable,
    ToolsMissing,
    OutsideToolsDirectory,
    DigestMismatch,
    InventoryInvalid,
    InventoryMismatch,
    SourceStagedMismatch,
    IoFailure,
    SizeLimit,
    Timeout,
    Cancelled,
};

struct GpuShaderToolResolveFailure {
    GpuShaderToolResolveError code = GpuShaderToolResolveError::IoFailure;
    std::string diagnostic;
};

struct GpuShaderToolResolveResult {
    bool ok = false;
    GpuShaderToolPaths paths;
    std::optional<GpuShaderToolResolveFailure> failure;
};

struct GpuShaderToolResolverStats {
    std::uint64_t resolves = 0;
    std::uint64_t cacheHits = 0;
};

// Blocking, off-UI startup qualification of the packaged shader tools: hashes the staged bytes once
// and caches the verdict for the resolver's lifetime. Never launches a process, consults PATH, or
// reads outside the executable's owned tools directory. Thread-safe; call only on a blocking-I/O
// worker. The accepted GpuShaderCompiler still performs its own identity/hash cache protection.
class GpuShaderToolResolver final {
  public:
    GpuShaderToolResolver();
    ~GpuShaderToolResolver();
    GpuShaderToolResolver(const GpuShaderToolResolver&) = delete;
    GpuShaderToolResolver& operator=(const GpuShaderToolResolver&) = delete;

    [[nodiscard]] GpuShaderToolResolveResult
    resolve(const std::filesystem::path& applicationExecutable, const GpuShaderToolPackage& package,
            const GpuShaderToolResolveLimits& limits = {},
            const GpuShaderCancellation& cancel = {});

    void reset();
    [[nodiscard]] GpuShaderToolResolverStats stats() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::color
