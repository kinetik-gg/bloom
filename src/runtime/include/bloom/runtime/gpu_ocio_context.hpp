#pragma once

// Shared runtime OCIO context resolver: the one production place that turns the packaged GPU
// shader tools into a GpuSceneOcioContext for every production route (viewer/RAM preview,
// effects, export/CLI/MCP).
//
// The resolver is deliberately inert to construct: it stores an immutable request and performs no
// hashing, no filesystem access, no OCIO configuration, and no process work. resolve() is
// BLOCKING, must run on a CPU worker (never the UI thread or a GPU owner thread), and lazily
// qualifies the packaged executable-relative tools through color::GpuShaderToolResolver, fills the
// validated tool paths, creates exactly one GpuOcioProgramPreparer, and caches the immutable
// GpuSceneOcioContext. Every later and concurrent resolve() reuses that one preparer.
//
// It never consults PATH, an environment variable, or a workspace path, and it never fixes a colour
// configuration: the per-request project config stays caller-resolved and is supplied to
// GpuOcioProgramPreparer::prepare by the consumer. An empty/disabled package is a typed
// ToolsUnavailable refusal so a caller keeps the existing CPU reference path; a malformed packaged
// inventory is a typed InventoryInvalid refusal; a cancelled or failed resolve caches nothing, so a
// retry can succeed and can never observe a poisoned context.

#include <bloom/color/gpu_shader_tool_resolver.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace bloom::runtime {

// Defined in prepared_gpu_scene.hpp; the resolver returns the existing producer context rather than
// duplicating it. Consumers that use the result include prepared_gpu_scene.hpp.
struct GpuSceneOcioContext;

// Immutable, caller-supplied request. Construction of the resolver stores this and does no work.
struct GpuOcioContextRequest final {
    // Absolute path of the application/CLI/MCP (or test) executable whose own directory contains
    // the packaged tools directory. Never a guessed or ambient path.
    std::filesystem::path applicationExecutable;
    // Immutable packaging facts built from the executable target's BLOOM_GPU_TOOLS_* definitions.
    color::GpuShaderToolPackage toolPackage;
    // One-time qualification ceilings and deadline for the staged tool bytes and inventory.
    color::GpuShaderToolResolveLimits toolLimits{};
    // Budgets of the single shared preparer created by the first successful resolve().
    GpuOcioPreparerBudgets preparerBudgets{};
    // Template compile options. resolve() overwrites glslangValidatorPath/spirvValPath with the
    // validated executable-relative paths; targetEnvironment, byte ceilings, and deadline are kept.
    GpuOcioCompileOptions compileOptions{};
};

enum class GpuOcioContextError : std::uint8_t {
    None = 0,
    // Malformed request rejected before any tool work.
    InvalidRequest,
    // No packaged tools configured/available: the caller keeps the CPU reference path.
    ToolsUnavailable,
    InvalidExecutable,
    InvalidPackage,
    ToolsMissing,
    OutsideToolsDirectory,
    DigestMismatch,
    // The packaged inventory is structurally malformed.
    InventoryInvalid,
    InventoryMismatch,
    SourceStagedMismatch,
    IoFailure,
    SizeLimit,
    Timeout,
    Cancelled,
    // The caller's compile-options template is invalid after the validated paths were filled in.
    CompileOptionsInvalid,
    InternalInvariant,
};

[[nodiscard]] std::string_view gpuOcioContextErrorName(GpuOcioContextError error) noexcept;

struct GpuOcioContextResult final {
    // Non-null exactly on success. Immutable and shared: the same object (and the same preparer) is
    // returned to every later caller.
    std::shared_ptr<const GpuSceneOcioContext> context;
    GpuOcioContextError error = GpuOcioContextError::None;
    std::string diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return context != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

struct GpuOcioContextCounters final {
    // One-time successful tool qualifications.
    std::uint64_t resolves = 0;
    // Calls served the cached shared context.
    std::uint64_t cacheHits = 0;
};

// Internally synchronized. Thread-safe for concurrent resolve()/preparer()/counters() calls; the
// returned context is immutable. A default-constructed resolver has an empty request and therefore
// reports ToolsUnavailable.
class GpuOcioContextResolver final {
  public:
    GpuOcioContextResolver();
    explicit GpuOcioContextResolver(GpuOcioContextRequest request);
    ~GpuOcioContextResolver();
    GpuOcioContextResolver(const GpuOcioContextResolver&) = delete;
    GpuOcioContextResolver& operator=(const GpuOcioContextResolver&) = delete;

    // BLOCKING. Call on a CPU worker. Lazily resolves the packaged tools once and returns the one
    // shared immutable context. Failures/cancellation are typed and cache nothing.
    [[nodiscard]] GpuOcioContextResult resolve(const color::GpuShaderCancellation& cancel = {});

    // The shared preparer for the display arm (also reachable as context->preparer). Null before a
    // successful resolve.
    [[nodiscard]] std::shared_ptr<GpuOcioProgramPreparer> preparer() const;
    [[nodiscard]] GpuOcioContextCounters counters() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::runtime
