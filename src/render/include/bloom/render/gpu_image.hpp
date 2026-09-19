#pragma once

// Bloom-owned, Qt-free and Vulkan-free ownership of one GPU-resident RGBA32F
// image.
//
// This is the resident resource the GPU compositing path will retain and later
// read as a transform or source-over input. No native handle, Vulkan type, or
// Qt type appears here. The image is device-generation-scoped: destroying it on
// the owner thread releases the image and its view, and it co-owns the device
// allocator generation so the allocator cannot be torn down underneath it. The
// normal path never reads it back; readbackResidentImage() exists only for
// test/oracle parity.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {

class GpuDevice;
struct GpuImageImpl;
enum class GpuImageReadbackCode : std::uint8_t;
struct GpuImageReadback;

class GpuImage final {
  public:
    GpuImage() noexcept = default;
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    GpuImage(GpuImage&& other) noexcept;
    GpuImage& operator=(GpuImage&& other) noexcept;
    ~GpuImage();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    // Meaningful only while isValid(); empty for an invalid/moved-from image.
    [[nodiscard]] std::optional<ImageWindow> dataWindow() const noexcept;
    [[nodiscard]] std::optional<ImageWindow> displayWindow() const noexcept;
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;

    // True only for the exact GpuDevice generation this image was created from,
    // on that device's owner thread; false for a moved-from image, a different
    // device, a stub, or a foreign thread.
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;

  private:
    friend GpuImage makeGpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept;
    friend GpuImageReadback readbackResidentImage(const GpuImage& image,
                                                  std::uint64_t byteBudget) noexcept;
    friend const GpuImageImpl* gpuImageImpl(const GpuImage& image) noexcept;
    explicit GpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept;
    void releaseOwnedImpl() noexcept;

    std::unique_ptr<GpuImageImpl> impl_;
};

// The one internal construction path; not a public success-report factory.
[[nodiscard]] GpuImage makeGpuImage(std::unique_ptr<GpuImageImpl> impl) noexcept;

enum class GpuImageReadbackCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    ReadbackFailed,
};

// Test/oracle-only host readback of a resident image. Owner-thread; allocates a
// host-visible staging buffer bounded by byteBudget, copies, and returns
// premultiplied RGBA32F pixels. Normal compositing paths must not call this:
// display/readback is not the GPU-resident result.
struct GpuImageReadback final {
    GpuImageReadbackCode code = GpuImageReadbackCode::None;
    std::string message;
    std::vector<Rgba32f> pixels;

    [[nodiscard]] bool hasValue() const noexcept { return code == GpuImageReadbackCode::None; }
    explicit operator bool() const noexcept { return hasValue(); }
};

[[nodiscard]] GpuImageReadback readbackResidentImage(const GpuImage& image,
                                                     std::uint64_t byteBudget) noexcept;

} // namespace bloom::render
