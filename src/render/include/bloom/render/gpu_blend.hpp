#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU blend operation: BlendV1. It combines two GPU-resident
// RGBA32F images under any core::BlendMode value and writes a NEW resident image; the normal path
// never reads back a full frame.
//
// This header is intentionally narrow and additive. It does not create a device or a service and it
// does not touch GpuComposite: it consumes the existing GpuDevice and resident GpuImage ownership
// from gpu_image.hpp, and its inputs are read-only. Native work runs on the device owner thread and
// fails closed from any other thread. One job is outstanding at a time.
//
// The CPU primitive render::blendLinearRec709SceneRow() in cpu_image_primitives.hpp remains the
// correctness oracle. The result is compared to it under the documented per-finite-component 2e-6
// absolute-or-relative gate (docs/architecture/gpu-backend.md), never claimed bit-exact.
//
// Geometry contract (identical to SourceOverV1): the destination data window is the output window;
// the source is sampled at destLocal + sourceOffset where sourceOffset =
// destinationDataWindow.origin - sourceDataWindow.origin. The output display window and pixel
// aspect are preserved from the destination. Normal reproduces the retained source-over behaviour
// exactly.

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::render {

enum class GpuBlendJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuBlendPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuBlendDiagnosticCode : std::uint8_t {
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
    // The kernel's status/error flag was nonzero: the frame must not be published and the caller
    // falls back to the CPU reference path.
    StatusFlagRejected,
};

struct GpuBlendDiagnostic final {
    GpuBlendDiagnosticCode code = GpuBlendDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuBlendDiagnostic&, const GpuBlendDiagnostic&) = default;
};

// Validated inputs for BlendV1. `source` is the foreground; `destination` the backdrop. Both are
// shared immutable ownership retained for the job's lifetime and never mutated. `mode` is the
// durable core::BlendMode value; an unknown stored integer cannot reach this type.
struct GpuBlendParameters final {
    std::shared_ptr<const GpuImage> source;
    std::shared_ptr<const GpuImage> destination;
    core::BlendMode mode = core::BlendMode::Normal;
};

struct GpuBlendBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maxMetadataBytes = 16ULL * 1024ULL * 1024ULL;
};

struct GpuBlendCreateResult;

class GpuBlend final {
  public:
    GpuBlend(const GpuBlend&) = delete;
    GpuBlend& operator=(const GpuBlend&) = delete;
    GpuBlend(GpuBlend&& other) noexcept;
    GpuBlend& operator=(GpuBlend&& other) noexcept;
    ~GpuBlend();

    // Builds the cached pipeline, layout, descriptor set, command pool, and fence once, on the
    // device owner thread. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuBlendCreateResult create(GpuDevice& device,
                                                     const GpuBlendBudgets& budgets = {});

    [[nodiscard]] GpuBlendJobState state() const noexcept;
    [[nodiscard]] const GpuBlendDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/budgets/device limits, prepares the resident output image, and submits one
    // dispatch. None when accepted.
    [[nodiscard]] GpuBlendDiagnostic beginBlend(const GpuBlendParameters& parameters,
                                                std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the status flag word has been checked; a nonzero flag
    // publishes Failure(StatusFlagRejected) instead of Ready.
    [[nodiscard]] GpuBlendPollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident output image out and returns the pipeline to Idle. Only valid when Ready.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight job within its
    // bounded budget and retained (did not destroy) busy Vulkan objects.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuBlend(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuBlendCreateResult final {
    std::unique_ptr<GpuBlend> blend;
    GpuBlendDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return blend != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
