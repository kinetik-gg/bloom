#pragma once

// Bloom-owned, Qt-free and Vulkan-free presentation sampling parameters for the P2
// GpuPresentationTarget. It describes how a resident GpuDisplayImage (packed straight RGBA8 sRGB)
// and an optional premultiplied RGBA8 overlay are composed into the already-acquired swapchain
// image: display-space background/checkerboard, the viewer channel remap, and an explicit affine
// destination-rectangle / source-image-window mapping. No Qt or Vk type is public.
//
// The caller derives `destination` from the live view transform (zoom/pan/fit and pixel aspect),
// exactly as ViewerEditor::viewTransformedDisplayRect() does, and supplies the source window in
// display pixels. The presenter never reads back the display image and never uploads a full frame.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>

namespace bloom::render {

class GpuDisplayImage;

// Mirrors bloom::ui::ViewerChannel exactly (same order and meaning). The presenter applies it to
// the already display-referred RGBA8 bytes, before compositing, exactly like
// ViewerEditor::remapChannels.
enum class GpuPresentChannel : std::uint8_t {
    Rgba,
    Rgb,
    Red,
    Green,
    Blue,
    Alpha,
};

// Mirrors bloom::ui::ViewerBackground exactly.
enum class GpuPresentBackground : std::uint8_t {
    Solid,
    Checkerboard,
    Black,
    White,
};

struct GpuPresentColor final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

// Destination rectangle in target pixels (x, y from the top-left, width/height in pixels).
struct GpuPresentRect final {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

// Source window in display-image pixels.
struct GpuPresentSourceWindow final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// Optional premultiplied RGBA8 overlay. `token` identifies one overlay generation; the presenter
// uploads only when the token changes, so the UI/CPU-worker side never forces a per-frame upload.
// `pixels == nullptr` or `token == 0` means no overlay.
struct GpuPresentOverlay final {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowStrideBytes = 0;
    bool premultiplied = true;
    std::uint64_t token = 0;
};

struct GpuPresentImageParams final {
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    // Caller-derived: the affine destination of the display image in target pixels (zoom/pan/fit,
    // pixel-aspect ratio already folded in, exactly what drawImage() receives today).
    GpuPresentRect destination;
    // Caller-derived source window over the resident display image, in display pixels.
    GpuPresentSourceWindow source;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    GpuPresentChannel channel = GpuPresentChannel::Rgba;
    GpuPresentBackground background = GpuPresentBackground::Solid;
    // The composition background, already composited to opaque by the caller for Solid.
    GpuPresentColor backgroundColor{0.0F, 0.0F, 0.0F, 1.0F};
    // Checkerboard tiles, matching the viewer's Surface (base) and SurfaceRaised (raised) roles.
    GpuPresentColor checkerColorA{0.0F, 0.0F, 0.0F, 1.0F};
    GpuPresentColor checkerColorB{1.0F, 1.0F, 1.0F, 1.0F};
    // Tile size and origin in target pixels (caller multiplies the 22px viewer token by the device
    // pixel ratio and passes the canvas-surround origin so the pattern aligns exactly).
    float checkerTilePixels = 22.0F;
    float checkerOriginX = 0.0F;
    float checkerOriginY = 0.0F;
};

} // namespace bloom::render
