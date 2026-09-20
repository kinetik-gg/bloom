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
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
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
    // Isolated file-transform extraction. The exact LUT bytes and their content digest come from a
    // color::LutFile produced by color::readLutFile; the isolated bloom-color-worker rebuilds the
    // OCIO FileTransform processor with the requested interpolation/direction. The LUT digest,
    // format, interpolation, direction, and process/working space ids are part of the content
    // identity and the preparer warm key.
    FileTransform = 4,
};

struct GpuOcioTransformSpec final {
    GpuOcioTransformKind kind = GpuOcioTransformKind::Cst;
    std::string fromId;
    std::string toId;
    std::string display;
    std::string view;
    double exposure = 0.0;
    double contrast = 1.0;
    // FileTransform only. The shared, immutable LUT resource handle; the reservation keeps the
    // exact opened bytes alive for the whole extraction. Null is an InvalidRequest refusal.
    std::shared_ptr<const color::LutFile> lutFile;
    color::LutInterpolation interpolation = color::LutInterpolation::Best;
    color::LutDirection direction = color::LutDirection::Forward;
    std::string processSpaceId;
    std::string workingSpaceId;
    // DisplayPacking only. The exact post-display exposure/gamma applied by the production wrapper.
    // Neutral for every other kind; a non-neutral adjustment on a non-display kind is refused. Both
    // fields are folded into the preparer warm key.
    ViewAdjust viewAdjust{};
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

// Self-contained validation of an injected options value. Mirrors color::GpuShaderCompiler's own
// hard ceilings and non-positive-deadline refusal so a warm cache hit can never bypass the checks a
// cold compile would enforce. `hardSourceBytes`/`hardSpirvBytes`/`hardDiagnosticBytes` are the
// compiler's fixed ceilings; the caller may lower them but not raise them.
[[nodiscard]] bool validGpuOcioCompileOptions(const GpuOcioCompileOptions& options) noexcept;

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
