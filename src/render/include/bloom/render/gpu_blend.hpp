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
#include <string_view>

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

// Selects the kernel for the six general separable modes. `Auto` uses the exact Float64 companion
// when the device advertised and enabled the core shaderFloat64 feature, and otherwise the portable
// compensated-Float32 kernel; Normal and Add always use the exact Float32 kernel. `PortableFloat32`
// forces the portable kernel even on a Float64-capable device. This is a narrow production option
// policy, not a capability claim: it never advertises a feature the device does not have, and a
// caller that forces the portable kernel simply declines the Float64 companion.
enum class GpuBlendKernelPolicy : std::uint8_t {
    Auto,
    PortableFloat32,
};

// Canonical, render-owned BlendV1 shader artifact identity. `float64Selected` is the ACTUAL
// general-mode pipeline selection: the device's shaderFloat64 capability combined with the caller's
// kernel policy, never a capability claim by itself. Normal and Add always map to the exact Float32
// kernel; the six general modes map to the exact Float64 companion when selected and to the
// portable compensated-Float32 kernel otherwise. The executor derives its blend cache key from
// this, so a capability or policy change can never serve a wrongly keyed image.
[[nodiscard]] std::string_view gpuBlendShaderIdentity(core::BlendMode mode,
                                                      bool float64Selected) noexcept;

struct GpuBlendCreateResult;

class GpuBlend final {
  public:
    GpuBlend(const GpuBlend&) = delete;
    GpuBlend& operator=(const GpuBlend&) = delete;
    GpuBlend(GpuBlend&& other) noexcept;
    GpuBlend& operator=(GpuBlend&& other) noexcept;
    ~GpuBlend();

    // Validates the device/budgets/policy on the device owner thread and returns a lazy,
    // native-free instance. Wrong thread returns WrongThread. `policy` chooses the general-mode
    // kernel; the portable Float32 kernel is built whenever it may be selected. The pipelines,
    // layout, descriptor set, command pool, and fence are built once on the first begin, under one
    // bounded resident-pool slot acquired before any native allocation; an idle instance allocates
    // nothing.
    [[nodiscard]] static GpuBlendCreateResult
    create(GpuDevice& device, const GpuBlendBudgets& budgets = {},
           GpuBlendKernelPolicy policy = GpuBlendKernelPolicy::Auto);

    // The actual shader artifact identity this pipeline selects for `mode`: Normal and Add are
    // "blend-v1-f32"; the six general modes are "blend-v1-f64" when the Float64 companion was built
    // and selected, and "blend-v1-f32-portable" otherwise. The executor folds this into its blend
    // semantic key, so a device capability or kernel-policy change can never serve a wrongly keyed
    // cached image. The empty view is returned for an uninitialized pipeline.
    [[nodiscard]] std::string_view shaderIdentity(core::BlendMode mode) const noexcept;

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

    // Recoverable pressure, not a permanent fuse: true while a foreign-released or unproven
    // submission is retained in the bounded process-wide resident pool, and false again once the
    // rightful owner thread drains it.
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
