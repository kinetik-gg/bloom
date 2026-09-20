#pragma once

// Owner-thread executor for immutable prepared OCIO GPU commands (runtime::PreparedGpuOcioCommand).
//
// The executor binds to one render::GpuDevice ownership generation and drives exactly one
// bloom::render::GpuOcioProgram per command identity. Native programs (pipeline, OCIO LUT textures,
// uniform buffer, status buffer) are retained in a bounded cache keyed on the command's canonical
// identity, so a warm frame with the same identity performs ZERO program creation and ZERO resource
// upload; only the dispatch runs. A changed config, uniform, geometry, artifact, or output encoding
// is a different identity and therefore a different program.
//
// It owns no thread, service, Qt surface, or readback. begin/poll/take/cancel/destruction are
// owner-thread only; a foreign-thread call fails closed. The source GpuImage is retained for the
// whole job and is never read back to the CPU. Effect output is a resident RGBA32F GpuImage;
// display output is a resident RGBA8 GpuDisplayImage; the two are never interchanged.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace bloom::runtime {

enum class GpuOcioExecutorJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuOcioExecutorPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuOcioExecutorDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    // The command's geometry does not match the actual input image.
    IdentityMismatch,
    Busy,
    // A previous native submission is not proven retired; no new begin is admitted.
    OwnerDrainRequired,
    Unsupported,
    OverBudget,
    // The native program could not be created (resource/device/shader failure).
    ProgramRefused,
    // The native dispatch refused the request.
    DispatchRefused,
    Cancelled,
    NativeTimeout,
    InternalInvariant,
};

struct GpuOcioExecutorDiagnostic final {
    GpuOcioExecutorDiagnosticCode code = GpuOcioExecutorDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuOcioExecutorDiagnostic&,
                           const GpuOcioExecutorDiagnostic&) = default;
};

struct GpuOcioExecutorBudgets final {
    // Bounded native-program cache: entry count and the sum of the commands' descriptor-declared
    // retained bytes. Both are hard ceilings checked before insertion.
    std::uint64_t maxPrograms = 16;
    std::uint64_t maxRetainedProgramBytes = 512ULL * 1024ULL * 1024ULL;
    // Forwarded to each render::GpuOcioProgram create() as its own hard allocation ceilings.
    std::uint64_t maxOwnedBytesPerProgram = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t maxLutBytesPerProgram = 256ULL * 1024ULL * 1024ULL;
    // A pending native job that has not retired by this deadline is cancelled and failed closed.
    std::uint64_t jobDeadlineMilliseconds = 5000;
};

struct GpuOcioExecutorCounters final {
    std::uint64_t commandsAccepted = 0;
    std::uint64_t programCreations = 0;
    std::uint64_t programReuses = 0;
    std::uint64_t programEvictions = 0;
    std::uint64_t programRefusals = 0;
    std::uint64_t effectDispatches = 0;
    std::uint64_t displayDispatches = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t jobCompletions = 0;
    std::uint64_t jobFailures = 0;
    std::uint64_t jobCancellations = 0;
    std::uint64_t jobTimeouts = 0;
    std::uint64_t budgetRefusals = 0;
    // Always zero by construction: the executor never reads back a full frame.
    std::uint64_t readbacks = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t cacheBytes = 0;
};

struct GpuOcioExecutorCreateResult;

class GpuOcioProgramExecutor final {
  public:
    GpuOcioProgramExecutor(const GpuOcioProgramExecutor&) = delete;
    GpuOcioProgramExecutor& operator=(const GpuOcioProgramExecutor&) = delete;
    GpuOcioProgramExecutor(GpuOcioProgramExecutor&& other) noexcept;
    GpuOcioProgramExecutor& operator=(GpuOcioProgramExecutor&& other) noexcept;
    ~GpuOcioProgramExecutor();

    // Device owner thread only. A foreign thread returns WrongThread without touching Vulkan.
    [[nodiscard]] static GpuOcioExecutorCreateResult
    create(render::GpuDevice& device, const GpuOcioExecutorBudgets& budgets = {});

    [[nodiscard]] GpuOcioExecutorJobState state() const noexcept;
    [[nodiscard]] const GpuOcioExecutorDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(const render::GpuDevice& device) const noexcept;
    [[nodiscard]] GpuOcioExecutorCounters counters() const noexcept;
    [[nodiscard]] bool deviceLost() const noexcept;

    // True while the in-flight native program has a submission whose fence retirement is not
    // proven. begin() returns OwnerDrainRequired meanwhile; destroying the executor performs the
    // bounded drain/quarantine of the retained programs.
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;

    // Returns None when the command was accepted and submitted. `uniformOverride` is an optional
    // same-size replacement for the command's immutable OCIO uniform snapshot (empty selects the
    // snapshot). `byteBudget` bounds the native transient bytes.
    [[nodiscard]] GpuOcioExecutorDiagnostic
    begin(std::shared_ptr<const PreparedGpuOcioCommand> command,
          std::shared_ptr<const render::GpuImage> input, std::span<const std::byte> uniformOverride,
          std::uint64_t byteBudget);

    // Non-blocking; advances the single in-flight job.
    [[nodiscard]] GpuOcioExecutorPollResult poll();

    // Valid only after a Ready FinalRgba32f job; moves the resident RGBA32F output out.
    [[nodiscard]] std::shared_ptr<render::GpuImage> takeEffectOutput() noexcept;
    // Valid only after a Ready DisplayRgba8 job; moves the resident RGBA8 output out. Nullopt for
    // any other state. (GpuDisplayImage is not default-constructible outside src/render.)
    [[nodiscard]] std::optional<render::GpuDisplayImage> takeDisplayOutput() noexcept;

    // Requests cancellation. Resources are retained until the native submission retires; no output
    // is published after this call.
    void cancel() noexcept;

    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuOcioProgramExecutor(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuOcioExecutorCreateResult final {
    std::unique_ptr<GpuOcioProgramExecutor> executor;
    GpuOcioExecutorDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return executor != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
