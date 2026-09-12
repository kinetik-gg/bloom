#ifndef BLOOM_RENDER_CPU_IMAGE_PRIMITIVES_HPP
#define BLOOM_RENDER_CPU_IMAGE_PRIMITIVES_HPP

#include <bloom/core/color.hpp>
#include <bloom/render/image.hpp>

#include <cstdint>
#include <span>

namespace bloom::render {

// Bumped to 3 by the CPU text path (issue #130 follow-up, task S3): the primitive set now also
// covers coverageSolidRow() below and the glyph coverage bloom/render/text_raster.hpp produces, so
// a change in either one has to invalidate previously published frames. This is the one number
// ProcessFrameIdentity carries for every CPU pixel primitive (imagePrimitiveSemanticsVersion); the
// text rasterizer deliberately does not define a second version of its own, which could drift out
// of that identity.
inline constexpr std::uint32_t kCpuImagePrimitiveSemanticsVersion = 3;

// Checked authored layer parameters. Translation remains Float64 pixel-center displacement;
// opacity is deliberately rounded once to the Float32 process precision.
class TranslationOpacity final {
  public:
    [[nodiscard]] static ImageResult<TranslationOpacity>
    create(double translationX, double translationY, double opacity) noexcept;

    [[nodiscard]] constexpr double translationX() const noexcept { return translationX_; }
    [[nodiscard]] constexpr double translationY() const noexcept { return translationY_; }
    [[nodiscard]] constexpr float opacity() const noexcept { return opacity_; }

    friend constexpr bool operator==(const TranslationOpacity&,
                                     const TranslationOpacity&) noexcept = default;

  private:
    constexpr TranslationOpacity(const double translationX, const double translationY,
                                 const float opacity) noexcept
        : translationX_(translationX), translationY_(translationY), opacity_(opacity) {}

    double translationX_ = 0.0;
    double translationY_ = 0.0;
    float opacity_ = 1.0F;
};

// Converts one straight v1 solid authoring color into the canonical premultiplied
// lin_rec709_scene Float32 process pixel. The v1 authoring primaries are numerically identical to
// the process primaries; premultiplication occurs in Float64 before checked Float32 rounding.
[[nodiscard]] ImageResult<Rgba32f>
solidPixelFromStraightLinearRec709Scene(core::Color4d color) noexcept;

// Row primitives never allocate or schedule. Their caller owns cancellation checks at row
// boundaries and discards partially constructed outputs after any error.
void fillSolidRow(std::span<Rgba32f> output, Rgba32f pixel) noexcept;

// Pixel centers have integer local coordinates. The inverse mapping is output minus translation;
// bilinear taps outside the source data window are exact transparent black. Output storage must
// not overlap the source image.
[[nodiscard]] ImageStatus
translateOpacityBilinearRow(Rgba32fImageView source, ImageWindow outputWindow, std::int64_t outputY,
                            TranslationOpacity parameters, std::span<Rgba32f> output) noexcept;

// Composites source over destination in lin_rec709_scene process space. Destination is the
// in-place output. Source storage must not overlap destination storage. Both spans contain
// premultiplied pixels; process RGB is never clamped.
[[nodiscard]] ImageStatus sourceOverLinearRec709SceneRow(std::span<const Rgba32f> source,
                                                         std::span<Rgba32f> destination) noexcept;

// Writes one run of rasterized text as premultiplied lin_rec709_scene process pixels: output[i] is
// `pixel` with every component scaled by coverage[i] / 255. `pixel` is the already-premultiplied
// process pixel for the text color (solidPixelFromStraightLinearRec709Scene() of the authored
// color), so scaling all four components by the coverage fraction is exactly premultiplied
// compositing of color over nothing at partial coverage -- it needs no unpremultiply/repremultiply
// round trip and cannot produce alpha above the color's own.
//
// Gamma: a coverage byte is a LINEAR area fraction (coverage / 255 exactly), not a gamma-encoded
// intensity, so it is used directly as linear alpha with no transfer function applied. See
// bloom/render/text_raster.hpp for the full rule. The spans must have equal length and must not
// overlap; the caller clips the run to its output window before calling.
[[nodiscard]] ImageStatus coverageSolidRow(std::span<const std::uint8_t> coverage, Rgba32f pixel,
                                           std::span<Rgba32f> output) noexcept;

// Maps premultiplied lin_rec709_scene process pixels to straight packed sRGB display pixels.
// Pixels outside the process data window are transparent; display clipping happens only here.
[[nodiscard]] ImageStatus mapLinearRec709SceneToSrgbRow(Rgba32fImageView source,
                                                        ImageWindow displayWindow,
                                                        std::int64_t outputY,
                                                        std::span<Rgba8> output) noexcept;

} // namespace bloom::render

#endif // BLOOM_RENDER_CPU_IMAGE_PRIMITIVES_HPP
