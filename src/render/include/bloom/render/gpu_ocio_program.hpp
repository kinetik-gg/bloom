#pragma once

// Bloom-owned, Qt-free and Vulkan-free native executor for one extracted OCIO GPU program
// (render::OcioGpuProgramDesc). It consumes a caller-compiled SPIR-V module (the complete wrapper +
// OCIO GLSL). It never compiles or re-writes shader text, never reads OCIO config, and never sees
// an OCIO/Vulkan/Qt type in this surface.
//
// Two arms, selected by OcioGpuProgramDesc::stage:
//  * ProcessEffect: resident RGBA32F in -> resident RGBA32F out. The wrapper un-premultiplies,
//    applies the OCIO function to straight RGB, preserves alpha, and re-premultiplies.
//  * DisplayPacking: resident RGBA32F in -> resident RGBA8 out. The wrapper un-premultiplies,
//    applies the OCIO function, applies the caller's view adjust, then exactly quantizes straight
//    RGBA8. Only the 4-byte status word is read back on this arm.
//
// OCIO resources (LUT textures, uniform buffer) occupy descriptor set 0 with the exact bindings
// OCIO declared; Bloom I/O occupies set 1. Textures are uploaded once per program+device generation
// and are never re-resampled or approximated. Uniform bytes are an exact caller snapshot written
// into the mapped UBO before dispatch. Owner-thread begin/poll/take/cancel/destruction; one
// outstanding job; no resource is destroyed while its submission is unretired.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace bloom::render {

enum class GpuOcioProgramJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuOcioProgramPollResult : std::uint8_t { Pending, Ready, Failure };

enum class GpuOcioProgramDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    Unsupported,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    ShaderRejected,
    AllocationFailed,
    Cancelled,
    NativeTimeout,
    ForeignInput,
};

struct GpuOcioProgramDiagnostic final {
    GpuOcioProgramDiagnosticCode code = GpuOcioProgramDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuOcioProgramDiagnostic&,
                           const GpuOcioProgramDiagnostic&) = default;
};

// `maxOwnedBytes` bounds this program's persistent LUT/UBO bytes plus one job's transient packed
// buffer; `maxLutBytes` bounds the aggregate sampled-texture bytes. Both are checked before every
// allocation.
struct GpuOcioProgramBudgets final {
    std::uint64_t maxOwnedBytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t maxLutBytes = 256ULL * 1024ULL * 1024ULL;
};

struct GpuOcioProgramCreateResult;

// Owner-side cancellation probe. Called only on the device owner thread, before allocations,
// between LUTs, before submission, and on bounded wait ticks. It must be non-blocking and must not
// touch UI. A cancel observed after submission retains every resource until fence proof and never
// publishes a program.
using GpuOcioProgramCancellation = std::function<bool()>;

class GpuOcioProgram final {
  public:
    GpuOcioProgram(const GpuOcioProgram&) = delete;
    GpuOcioProgram& operator=(const GpuOcioProgram&) = delete;
    GpuOcioProgram(GpuOcioProgram&& other) noexcept;
    GpuOcioProgram& operator=(GpuOcioProgram&& other) noexcept;
    ~GpuOcioProgram();

    [[nodiscard]] static GpuOcioProgramCreateResult
    create(GpuDevice& device, OcioGpuProgramDesc program, std::span<const std::uint32_t> spirv,
           const GpuOcioProgramBudgets& budgets = {}, GpuOcioProgramCancellation cancellation = {});

    [[nodiscard]] GpuOcioProgramJobState state() const noexcept;
    [[nodiscard]] const GpuOcioProgramDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;

    // Ownership of `input` is retained for the whole job. `uniformBytes` is the complete UBO
    // snapshot; an empty span selects the descriptor's immutable snapshot and must match
    // uniformBufferSize exactly. Returns None when the job was accepted and submitted.
    [[nodiscard]] GpuOcioProgramDiagnostic beginEffect(std::shared_ptr<const GpuImage> input,
                                                       std::span<const std::byte> uniformBytes,
                                                       std::uint64_t byteBudget);
    [[nodiscard]] GpuOcioProgramDiagnostic beginDisplay(std::shared_ptr<const GpuImage> input,
                                                        std::span<const std::byte> uniformBytes,
                                                        std::uint64_t byteBudget);

    [[nodiscard]] GpuOcioProgramPollResult poll();

    // Valid only after a Ready effect job; moves the resident RGBA32F result out.
    [[nodiscard]] std::shared_ptr<GpuImage> takeEffectOutput() noexcept;
    // Valid only after a Ready display job; moves the resident RGBA8 result out.
    [[nodiscard]] GpuDisplayImage takeDisplayOutput() noexcept;

    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    void cancel() noexcept;

    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuOcioProgram(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuOcioProgramCreateResult final {
    std::unique_ptr<GpuOcioProgram> program;
    GpuOcioProgramDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return program != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
