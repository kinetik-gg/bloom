#pragma once

// Runtime GPU output-colour stage for the export boundary.
//
// One owner-thread stage consumes a retained, immutable prepared OCIO command supplied off the UI
// thread and a resident RGBA32F process image, drives the command through the runtime
// GpuOcioProgramExecutor, and reads back BOTH required payloads in one combined submission:
//   * the exact process RGBA32F bits (the analysis/identity payload, untouched), and
//   * the encoded output (straight RGBA8 display bytes, or an output-space RGBA32F effect result).
//
// A null command selects the identity arm: no OCIO program runs and only the process payload is
// transferred (one payload, no extra work). This stage performs no OCIO extraction, compilation, or
// shader generation; the command and its SPIR-V artifact are already frozen.
//
// The transfer accounting is explicit and never folds two payloads into one:
//   * `transferredPayloads` is 1 (identity) or 2 (process + encoded),
//   * `readbackSubmissions` is the single device-to-host submission,
//   * `processPayloadBytes` + `encodedPayloadBytes` sum to `transferredBytes`.
//
// Owner-thread only. begin/poll/take/cancel are non-blocking; a foreign-thread call fails closed
// without touching native state. A cancelled or faulted submission retains its resources until the
// fence is proven retired.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_output_color_readback.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime {

// Which output contract the active job produces. None is the identity arm (process payload only).
enum class GpuOutputColorArm : std::uint8_t {
    None = 0,
    ProcessEffect = 1,
    DisplayRgba8 = 2,
};

enum class GpuOutputColorStatus : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuOutputColorPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuOutputColorDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    Busy,
    OverBudget,
    GeometryMismatch,
    ExecutorRefused,
    ExecutorFailed,
    ReadbackRefused,
    ReadbackFailed,
    Cancelled,
    InternalInvariant,
};

struct GpuOutputColorDiagnostic final {
    GpuOutputColorDiagnosticCode code = GpuOutputColorDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuOutputColorDiagnostic&,
                           const GpuOutputColorDiagnostic&) = default;
};

struct GpuOutputColorBudgets final {
    GpuOcioExecutorBudgets executor;
};

// Per-completed-job counters. Program/dispatch counts are deltas for this job; transfer counts are
// the combined readback's actual submission/payload/byte figures.
struct GpuOutputColorCounters final {
    std::uint64_t readbackSubmissions = 0;
    std::uint64_t transferredPayloads = 0;
    std::uint64_t transferredBytes = 0;
    std::uint64_t processPayloadBytes = 0;
    std::uint64_t encodedPayloadBytes = 0;
    std::uint64_t commandAccepted = 0;
    std::uint64_t programCreations = 0;
    std::uint64_t programReuses = 0;
    std::uint64_t dispatches = 0;

    friend bool operator==(const GpuOutputColorCounters&, const GpuOutputColorCounters&) = default;
};

struct GpuOutputColorFrame final {
    // Exact process RGBA32F bits, unchanged. Always present.
    std::vector<render::Rgba32f> process;
    // Output-space RGBA32F effect result (ProcessEffect arm).
    std::vector<render::Rgba32f> effectRgba32f;
    // Straight RGBA8 display result (DisplayRgba8 arm).
    std::vector<render::Rgba8> displayRgba8;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<render::ImageWindow> dataWindow;
    std::optional<render::ImageWindow> displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();

    GpuOutputColorArm arm = GpuOutputColorArm::None;
    core::Sha256Digest commandIdentity{};
    GpuOutputColorCounters counters;
};

struct GpuOutputColorCreateResult;

class GpuOutputColorStage final {
  public:
    GpuOutputColorStage(const GpuOutputColorStage&) = delete;
    GpuOutputColorStage& operator=(const GpuOutputColorStage&) = delete;
    GpuOutputColorStage(GpuOutputColorStage&&) = delete;
    GpuOutputColorStage& operator=(GpuOutputColorStage&&) = delete;
    ~GpuOutputColorStage();

    // Device owner thread only. A foreign thread returns DeviceUnavailable without touching Vulkan.
    [[nodiscard]] static GpuOutputColorCreateResult
    create(render::GpuDevice& device, const GpuOutputColorBudgets& budgets = {});

    [[nodiscard]] GpuOutputColorStatus state() const noexcept;
    [[nodiscard]] const GpuOutputColorDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] GpuOutputColorArm arm() const noexcept;
    [[nodiscard]] bool isBoundTo(const render::GpuDevice& device) const noexcept;
    // True while the executor or the combined readback holds an unproven submission.
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;

    // Accepts one job. `command` may be null (identity arm, process payload only); otherwise its
    // geometry must match `processImage`. Returns None on acceptance. `byteBudget` bounds the
    // executor transient and the combined readback staging + host vectors.
    [[nodiscard]] GpuOutputColorDiagnostic
    begin(std::shared_ptr<const PreparedGpuOcioCommand> command,
          std::shared_ptr<const render::GpuImage> processImage, std::uint64_t byteBudget) noexcept;

    // Non-blocking owner-thread advance across the dispatch and readback phases.
    [[nodiscard]] GpuOutputColorPollResult poll();

    // Valid only in Ready; moves the frame out and returns to Idle.
    [[nodiscard]] std::optional<GpuOutputColorFrame> take() noexcept;

    // Requests cancellation of the in-flight phase; no payload is published after this call.
    void cancel() noexcept;

  private:
    enum class Phase : std::uint8_t { Idle, Dispatching, ReadingBack, Ready, Failure };

    GpuOutputColorStage(std::unique_ptr<GpuOcioProgramExecutor> executor) noexcept;

    std::unique_ptr<GpuOcioProgramExecutor> executor_;
    render::GpuOutputColorReadback readback_;
    std::thread::id ownerThread_{};

    Phase phase_ = Phase::Idle;
    GpuOutputColorDiagnostic diagnostic_;
    GpuOutputColorArm arm_ = GpuOutputColorArm::None;
    std::shared_ptr<const PreparedGpuOcioCommand> command_;
    std::shared_ptr<const render::GpuImage> process_;
    std::uint64_t byteBudget_ = 0;
    GpuOcioExecutorCounters executorBefore_{};
    std::optional<GpuOutputColorFrame> frame_;
};

struct GpuOutputColorCreateResult final {
    std::unique_ptr<GpuOutputColorStage> stage;
    GpuOutputColorDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return stage != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
