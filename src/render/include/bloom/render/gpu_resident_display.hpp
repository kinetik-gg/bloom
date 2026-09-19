#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU-resident Bloom Neutral v1 display operation.
//
// Consumes a resident RGBA32F GpuImage (from bloom-solid) and produces a resident RGBA8 display
// image entirely on the device: RGBA32F image -> device-local SSBO, the accepted embedded Neutral
// V1 compute shader, packed device buffer -> RGBA8_UNORM resident image. The status word is the
// only host readback on the normal path; there is no full-frame CPU transfer. Readback of the whole
// display image exists for parity tests only.
//
// The input image is retained by shared ownership for the whole job lifetime; it is never copied to
// the host on the normal path. One pipeline per device, one outstanding job, owner-thread
// begin/poll/image/takeImage/readback/cancel and destruction. Wrong-thread calls fail closed
// without mutating owned state. No resource is destroyed while a submission references it: on a
// bounded wait that cannot prove retirement, the entire job and its input are retained.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {

class GpuDisplayImage;
struct GpuDisplayImageImpl;
enum class GpuDisplayImageReadbackCode : std::uint8_t;
struct GpuDisplayImageReadback;

// Opaque, move-only ownership of one GPU-resident RGBA8 display image. Owner-thread destruction;
// co-owns the device allocator generation.
class GpuDisplayImage final {
  public:
    GpuDisplayImage() noexcept = default;
    GpuDisplayImage(const GpuDisplayImage&) = delete;
    GpuDisplayImage& operator=(const GpuDisplayImage&) = delete;
    GpuDisplayImage(GpuDisplayImage&& other) noexcept;
    GpuDisplayImage& operator=(GpuDisplayImage&& other) noexcept;
    ~GpuDisplayImage();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::optional<ImageWindow> dataWindow() const noexcept;
    [[nodiscard]] std::optional<ImageWindow> displayWindow() const noexcept;
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;

  private:
    friend GpuDisplayImage makeGpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept;
    friend GpuDisplayImageReadback readbackResidentDisplayImage(const GpuDisplayImage& image,
                                                                std::uint64_t byteBudget) noexcept;
    // Read-only seam for the in-module presentation sampler: exposes the resident VkImage and its
    // generation to another src/render translation unit without any native handle reaching a public
    // consumer. Declared and defined inside src/render only.
    friend const GpuDisplayImageImpl* gpuDisplayImageImpl(const GpuDisplayImage& image) noexcept;
    explicit GpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept;
    void releaseOwnedImpl() noexcept;

    std::unique_ptr<GpuDisplayImageImpl> impl_;
};

[[nodiscard]] GpuDisplayImage
makeGpuDisplayImage(std::unique_ptr<GpuDisplayImageImpl> impl) noexcept;

enum class GpuDisplayImageReadbackCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    ReadbackFailed,
};

// Debug/parity-only host readback of the resident RGBA8 display image. Normal paths never call
// this.
struct GpuDisplayImageReadback final {
    GpuDisplayImageReadbackCode code = GpuDisplayImageReadbackCode::None;
    std::string message;
    std::vector<Rgba8> pixels;

    [[nodiscard]] bool hasValue() const noexcept {
        return code == GpuDisplayImageReadbackCode::None;
    }
    explicit operator bool() const noexcept { return hasValue(); }
};

[[nodiscard]] GpuDisplayImageReadback
readbackResidentDisplayImage(const GpuDisplayImage& image, std::uint64_t byteBudget) noexcept;

struct GpuResidentDisplayBudgets final {
    // Hard ceiling on bytes this job owns: temporary RGBA32F input buffer + packed output buffer +
    // 4-byte status + the actual RGBA8 output image allocation. The input GpuImage is caller-owned
    // and external to this budget.
    std::uint64_t maxOwnedBytes = 320ULL * 1024ULL * 1024ULL;
};

enum class GpuResidentDisplayJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuResidentDisplayPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuResidentDisplayDiagnosticCode : std::uint8_t {
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
};

struct GpuResidentDisplayDiagnostic final {
    GpuResidentDisplayDiagnosticCode code = GpuResidentDisplayDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuResidentDisplayDiagnostic&,
                           const GpuResidentDisplayDiagnostic&) = default;
};

struct GpuResidentDisplayCreateResult;

class GpuResidentDisplay final {
  public:
    GpuResidentDisplay(const GpuResidentDisplay&) = delete;
    GpuResidentDisplay& operator=(const GpuResidentDisplay&) = delete;
    GpuResidentDisplay(GpuResidentDisplay&& other) noexcept;
    GpuResidentDisplay& operator=(GpuResidentDisplay&& other) noexcept;
    ~GpuResidentDisplay();

    [[nodiscard]] static GpuResidentDisplayCreateResult
    create(GpuDevice& device, const GpuResidentDisplayBudgets& budgets = {});

    [[nodiscard]] GpuResidentDisplayJobState state() const noexcept;
    [[nodiscard]] const GpuResidentDisplayDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;

    // Begins one job. The input is retained for the whole job; it must be bound to the same device.
    [[nodiscard]] GpuResidentDisplayDiagnostic begin(std::shared_ptr<const GpuImage> input,
                                                     std::uint64_t byteBudget);

    [[nodiscard]] GpuResidentDisplayPollResult poll();

    [[nodiscard]] const GpuDisplayImage* image() const noexcept;
    [[nodiscard]] GpuDisplayImage takeImage() noexcept;

    // Debug/parity-only readback of the resident RGBA8 output.
    [[nodiscard]] GpuDisplayImageReadback readback() noexcept;

    void cancel() noexcept;

    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuResidentDisplay(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuResidentDisplayCreateResult final {
    std::unique_ptr<GpuResidentDisplay> display;
    GpuResidentDisplayDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return display != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
