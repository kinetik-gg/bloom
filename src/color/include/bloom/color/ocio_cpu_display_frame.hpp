#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

// Boundary product 3 of the "CPU Display Processor Boundary" contract ("PreparedDisplayFrame, an
// immutable packed display buffer and its display identity") and the "Alpha And Pixel Flow"
// seven-step implementation (issue #95 design decisions 5 and 6). Public API is Bloom render
// value types and the existing DisplayProcessorIdentityV1 serializer only.

namespace bloom::color {

// A tiny non-owning callable reference, matching bloom_render's row-primitive convention that
// "their caller owns cancellation checks at row/chunk boundaries" rather than this module taking
// a dependency on src/runtime's CancellationToken (which would invert the intended module
// layering -- a future runtime staging layer calls into bloom_color_ocio, not the reverse). A
// caller with no cancellation source may omit the argument; a default-constructed instance never
// reports cancellation.
class CancellationPredicateRef final {
  public:
    constexpr CancellationPredicateRef() noexcept = default;

    template <typename F>
        requires(std::is_invocable_r_v<bool, F&>)
    constexpr CancellationPredicateRef(F& f) noexcept // NOLINT(google-explicit-constructor)
        : context_(&f), invoke_(&invokeTrampoline<F>) {}

    [[nodiscard]] bool operator()() const noexcept {
        return invoke_ != nullptr && invoke_(context_);
    }

  private:
    template <typename F> [[nodiscard]] static bool invokeTrampoline(void* context) noexcept {
        return (*static_cast<F*>(context))();
    }

    void* context_ = nullptr;
    bool (*invoke_)(void*) noexcept = nullptr;
};

class PreparedDisplayFrame;

// Applies the prepared Bloom Neutral CPU display processor to one chunk of `n` premultiplied
// RGBA32F source pixels, producing `n` straight RGBA8 sRGB destination pixels, implementing the
// contract's seven-step alpha/pixel flow for that chunk:
//
//   1. read finite premultiplied RGBA32F (sourcePixels; non-finite input is rejected);
//   2. exact-binary32-zero alpha -> exact positive-zero RGB, otherwise one binary32
//      round-to-nearest-ties-even divide per RGB component, rejecting non-finite results
//      (written into `scratchRgba`, 4 floats per pixel: R, G, B, then the unchanged alpha);
//   3. the OCIO CPU processor is applied to `scratchRgba`'s R/G/B lanes only, using a stride
//      that skips the alpha lane entirely, so OCIO's own code never sees or touches alpha;
//   4. alpha is therefore untouched by construction;
//   5. non-finite processor RGB output is rejected;
//   6. the declared "straight-rgba8" packing clamps each component to [0, 1], converts exactly
//      to binary64, multiplies by 255, and quantizes as floor(value + 0.5) (frame-output.md's PNG
//      packing rule, reused here for the shared straight-rgba8 sRGB packing intent); and
//   7. no partial chunk is retained by the caller on any failure -- this function performs no
//      allocation and never mutates `destinationPixels` after returning a failure for that call.
//
// `scratchRgba` is caller-owned mutable working storage of exactly `4 * sourcePixels.size()`
// floats; its contents are overwritten and never retained across calls. No allocation, no
// cancellation check, and no partial-chunk publication semantics beyond this contract -- the
// orchestrating caller (produceBloomNeutralDisplayFrame below) owns cancellation between chunk
// calls and discards its whole in-progress frame on any chunk failure, exactly like
// bloom_render's row primitives own their caller's cancellation checks at row boundaries.
[[nodiscard]] render::ImageStatus applyBloomNeutralDisplayChunk(
    const PreparedCpuDisplayProcessorHandle& handle, std::span<const render::Rgba32f> sourcePixels,
    std::span<float> scratchRgba, std::span<render::Rgba8> destinationPixels) noexcept;

// Runs the complete seven-step flow over `source`'s full data window in bounded chunks of
// `chunkPixelCount` pixels (the last chunk may be shorter), checking `isCancelled` between
// chunks. Different `chunkPixelCount` values must produce bit-identical output (tested). The
// output pixel-storage budget is charged through the same idiom as
// bloom::render::ReferenceDisplayBufferBuilder::create: the required byte count is checked
// against `pixelStorageByteLimit` before allocation, and a chunk-scratch allocation failure or a
// budget/chunk-size/cancellation/pixel failure at any point publishes no PreparedDisplayFrame --
// the in-progress owned pixel storage is simply dropped.
[[nodiscard]] render::ImageResult<PreparedDisplayFrame>
produceBloomNeutralDisplayFrame(const PreparedCpuDisplayProcessorHandle& handle,
                                render::Rgba32fImageView source, std::size_t chunkPixelCount,
                                std::size_t pixelStorageByteLimit,
                                CancellationPredicateRef isCancelled = {}) noexcept;

class PreparedDisplayFrame final {
  public:
    PreparedDisplayFrame(PreparedDisplayFrame&&) noexcept;
    // Move-construct only: its DisplayProcessorIdentityV1 member is not move-assignable, so
    // move-assignment is explicitly deleted rather than silently omitted.
    PreparedDisplayFrame& operator=(PreparedDisplayFrame&&) = delete;
    PreparedDisplayFrame(const PreparedDisplayFrame&) = delete;
    PreparedDisplayFrame& operator=(const PreparedDisplayFrame&) = delete;
    ~PreparedDisplayFrame() = default;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] render::ImageWindow displayWindow() const noexcept { return displayWindow_; }
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept { return pixelAspect_; }
    [[nodiscard]] const render::PackedImageLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] std::span<const render::Rgba8> pixels() const& noexcept { return pixels_; }
    [[nodiscard]] std::span<const render::Rgba8> pixels() const&& = delete;
    [[nodiscard]] const DisplayProcessorIdentityV1& identity() const& noexcept { return identity_; }
    [[nodiscard]] const DisplayProcessorIdentityV1& identity() const&& = delete;

  private:
    friend render::ImageResult<PreparedDisplayFrame>
    produceBloomNeutralDisplayFrame(const PreparedCpuDisplayProcessorHandle&,
                                    render::Rgba32fImageView, std::size_t, std::size_t,
                                    CancellationPredicateRef) noexcept;

    PreparedDisplayFrame(render::ImageWindow displayWindow, core::PixelAspectRatio pixelAspect,
                         render::PackedImageLayout layout, std::vector<render::Rgba8> pixels,
                         DisplayProcessorIdentityV1 identity) noexcept;

    render::ImageWindow displayWindow_;
    core::PixelAspectRatio pixelAspect_;
    render::PackedImageLayout layout_;
    std::vector<render::Rgba8> pixels_;
    DisplayProcessorIdentityV1 identity_;
};

} // namespace bloom::color
