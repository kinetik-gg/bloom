#pragma once

// Runtime OCIO program preparation service: the off-UI half of the GPU OCIO command lifecycle.
//
// It owns the bounded content cache of immutable PreparedGpuOcioCommand values and a persistent
// color::GpuShaderCompiler. A prepare() call runs extraction (color's accepted OCIO GPU program
// builders), wrapper generation (runtime/gpu_ocio_wrapper.hpp), and GLSL -> SPIR-V compilation
// through the injected tool paths. It is BLOCKING and must never run on the UI thread or a native
// GPU owner thread. A warm prepare with the same semantic request returns the cached immutable
// command without re-extracting OCIO, re-generating the wrapper, or invoking glslang.
//
// The compiler tools are explicit inputs: this API never consults PATH, an ambient environment
// variable, or a hardcoded workspace path. Production callers supply qualified tool paths from
// their dependency/qualification profile; a caller that supplies none gets a typed
// InvalidRequest/CompileFailed refusal.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bloom::runtime {

enum class GpuOcioTransformKind : std::uint8_t {
    // Explicit colour-space transform: fromId -> toId in the exact resolved config.
    Cst = 1,
    // Display/view extraction using the resolved config's enumerated pair.
    Display = 2,
    // OCIO dynamic exposure/contrast. The accepted "view adjust" fixture: the extracted program
    // carries real OCIO uniforms whose values are part of the command's immutable snapshot.
    ExposureContrast = 3,
};

struct GpuOcioTransformSpec final {
    GpuOcioTransformKind kind = GpuOcioTransformKind::Cst;
    std::string fromId;
    std::string toId;
    std::string display;
    std::string view;
    double exposure = 0.0;
    double contrast = 1.0;
};

// Explicitly injected compiler configuration. `glslangValidatorPath` and `spirvValPath` must be
// already-qualified paths to the pinned tools.
struct GpuOcioCompileOptions final {
    std::string glslangValidatorPath;
    std::string spirvValPath;
    std::string targetEnvironment = "vulkan1.2";
    std::uint64_t maxSourceBytes = 4ULL * 1024ULL * 1024ULL;
    std::uint64_t maxSpirvBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maxDiagnosticBytes = 16ULL * 1024ULL;
    std::chrono::milliseconds deadline{10000};
};

enum class GpuOcioPreparationError : std::uint8_t {
    None,
    InvalidRequest,
    ExtractionFailed,
    IdentityTransform,
    WrapperFailed,
    CompileFailed,
    CompileCancelled,
    CommandInvalid,
    OverBudget,
};

[[nodiscard]] std::string_view gpuOcioPreparationErrorName(GpuOcioPreparationError error) noexcept;

struct GpuOcioPreparerBudgets final {
    std::size_t maxEntries = 32;
    std::uint64_t maxBytes = 128ULL * 1024ULL * 1024ULL;
    std::size_t compilerCacheEntries = 64;
    std::size_t compilerCacheBytes = 64ULL * 1024ULL * 1024ULL;
};

struct GpuOcioPreparerCounters final {
    std::uint64_t preparations = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t extractions = 0;
    std::uint64_t compiles = 0;
    std::uint64_t compileFailures = 0;
    std::uint64_t evictions = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t cacheBytes = 0;
};

struct GpuOcioPreparationResult final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    GpuOcioPreparationError error = GpuOcioPreparationError::None;
    std::string diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return command != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

using GpuOcioCancellation = std::function<bool()>;

// Blocking preparation service. Thread-safe for concurrent prepare() calls (the compiler and cache
// are internally synchronized); the returned command is immutable and shared.
class GpuOcioProgramPreparer final {
  public:
    explicit GpuOcioProgramPreparer(GpuOcioPreparerBudgets budgets = {});
    ~GpuOcioProgramPreparer();
    GpuOcioProgramPreparer(const GpuOcioProgramPreparer&) = delete;
    GpuOcioProgramPreparer& operator=(const GpuOcioProgramPreparer&) = delete;

    [[nodiscard]] GpuOcioPreparationResult prepare(const color::ResolvedBloomNeutralConfig& config,
                                                   const GpuOcioTransformSpec& transform,
                                                   GpuOcioCommandGeometry geometry,
                                                   const GpuOcioCompileOptions& options,
                                                   const GpuOcioCancellation& cancel = {});

    [[nodiscard]] GpuOcioPreparerCounters counters() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::runtime
