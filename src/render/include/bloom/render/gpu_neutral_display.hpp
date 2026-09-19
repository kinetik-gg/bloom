#pragma once

// Bloom-owned, Qt-free and Vulkan-free compute surface for the fixed Bloom Neutral v1 display.
//
// This is the first GPU operation: it applies the offline-generated, structurally validated
// Bloom Neutral v1 display compute shader to premultiplied RGBA32F source pixels and returns packed
// straight RGBA8 sRGB bytes. It is deliberately small: one pipeline per device, one outstanding
// job, no graph execution, no presentation, and no runtime shader compilation. The embedded SPIR-V
// is a build-time constant; nothing here reads a shader or config from disk.
//
// Threading: a pipeline records the GpuDevice owner thread at creation. begin(), poll(),
// readback(), state(), diagnostic(), and destruction are owner-thread-only operations. begin(),
// poll(), and readback() fail closed with WrongThread from a foreign thread without touching owned
// job state; destruction from a foreign thread, or a bounded owner-thread drain that cannot confirm
// the queue retired, retains (leaks) the generation rather than destroying live Vulkan objects, and
// reports it through teardownDrainIncomplete(). cancel() only sets a discard flag and is safe from
// any thread. The destructor drains on the owner thread and never pretends a UI thread may run it.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bloom::render {

enum class GpuNeutralDisplayJobState : std::uint8_t {
    Idle,
    Pending,
    Ready,
    Failure,
};

enum class GpuNeutralDisplayPollResult : std::uint8_t {
    Pending,
    Ready,
    Failure,
};

enum class GpuNeutralDisplayDiagnosticCode : std::uint8_t {
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
};

struct GpuNeutralDisplayDiagnostic final {
    GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuNeutralDisplayDiagnostic&,
                           const GpuNeutralDisplayDiagnostic&) = default;
};

// Bounded, documented budgets. `maxInputBytes` caps the source (128 MiB admits a 3840x2160 RGBA32F
// frame at 16 bytes per pixel). `maxOwnedBytes` is the hard ceiling on the retained GPU buffer
// bytes the pipeline holds: input + output + status. A per-call byteBudget is an additional,
// tighter admission cost for that one request. Retained capacity is shrunk or grown so it always
// fits both ceilings, so a warmed-up pipeline cannot carry more GPU memory than a later, tighter
// request is charged for. The host readback vector is separate and is bounded because it is exactly
// 4 bytes per pixel and the pixel count is already capped by maxInputBytes.
struct GpuNeutralDisplayBudgets final {
    std::uint64_t maxInputBytes = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t maxOwnedBytes = 160ULL * 1024ULL * 1024ULL;
};

struct GpuNeutralDisplayReadback final {
    std::vector<Rgba8> pixels;
    GpuNeutralDisplayDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept {
        return diagnostic.code == GpuNeutralDisplayDiagnosticCode::None;
    }
    explicit operator bool() const noexcept { return hasValue(); }
};

struct GpuNeutralDisplayCreateResult;

class GpuNeutralDisplay final {
  public:
    GpuNeutralDisplay(const GpuNeutralDisplay&) = delete;
    GpuNeutralDisplay& operator=(const GpuNeutralDisplay&) = delete;
    GpuNeutralDisplay(GpuNeutralDisplay&& other) noexcept;
    GpuNeutralDisplay& operator=(GpuNeutralDisplay&& other) noexcept;
    ~GpuNeutralDisplay();

    // Builds the shader module, descriptor set layout, pipeline layout, compute pipeline, command
    // pool, and fence once, on the current (device owner) thread. The device must be Ready and the
    // caller must be its owner thread; a foreign thread returns WrongThread without touching Vulkan
    // state. An unsupported device, shader rejection, or allocation failure returns a typed
    // diagnostic.
    [[nodiscard]] static GpuNeutralDisplayCreateResult
    create(GpuDevice& device, const GpuNeutralDisplayBudgets& budgets = {});

    [[nodiscard]] GpuNeutralDisplayJobState state() const noexcept;
    [[nodiscard]] const GpuNeutralDisplayDiagnostic& diagnostic() const noexcept;

    // Copies `source` immediately, validates the pixel count, layout arithmetic, device limits, and
    // the byte budget, then records and submits one dispatch. Returns a None diagnostic when the
    // job was accepted; Busy/WrongThread/Unsupported/OverBudget otherwise. An empty source is
    // rejected. No borrowed pointer survives this call. `byteBudget` is an upper allowance for this
    // request and is clamped to the configured hard ceiling, so a generous allowance never rejects
    // an otherwise valid small job; the actual required and retained bytes still must fit the hard
    // ceiling.
    [[nodiscard]] GpuNeutralDisplayDiagnostic begin(std::span<const Rgba32f> source,
                                                    std::uint64_t byteBudget);

    // Non-blocking VkGetFenceStatus. Only Pending/Ready/Failure; call again after Pending.
    [[nodiscard]] GpuNeutralDisplayPollResult poll();

    // Valid only after poll() returns Ready. Moves the packed RGBA8 frame out and returns the
    // pipeline to Idle; on any other state returns a Failure diagnostic and publishes nothing.
    [[nodiscard]] GpuNeutralDisplayReadback readback();

    // Marks the outstanding job's result as discarded. Vulkan resources stay alive until the fence
    // signals; the job is never freed in flight.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight job within its
    // bounded budget and therefore abandoned (did not destroy) busy Vulkan resources.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuNeutralDisplay(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuNeutralDisplayCreateResult final {
    std::unique_ptr<GpuNeutralDisplay> display;
    GpuNeutralDisplayDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return display != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
