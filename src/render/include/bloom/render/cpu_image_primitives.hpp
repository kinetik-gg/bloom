#ifndef BLOOM_RENDER_CPU_IMAGE_PRIMITIVES_HPP
#define BLOOM_RENDER_CPU_IMAGE_PRIMITIVES_HPP

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/image.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace bloom::render {

// Bumped to 5 by the blend-mode slice: the Layer Stack stage now folds each layer with
// blendLinearRec709SceneRow() instead of sourceOverLinearRec709SceneRow() directly. A Normal layer
// is bit-identical -- the Normal case IS a call to the retained source-over kernel, not a
// re-derivation of it -- but the primitive that produced any published frame is a different one and
// the reachable picture space is strictly larger, so previously published frames must still be
// invalidated.
//
// Bumped to 4 by the layer transform breadth slice (task S4): layerTransformBilinearRow() below
// replaces translateOpacityBilinearRow() in the Layer Output stage, so a layer's pixels are now
// produced by an inverse-mapped affine resample rather than a translate-only one. Scale 1 /
// rotation 0 layers are bit-identical to version 3 (LayerTransform's translate-only path is the
// pre-S4 arithmetic, unchanged and pinned by a test against the retained reference primitive), but
// the primitive that produced them is a different one and the reachable picture space is strictly
// larger, so previously published frames must still be invalidated. This is the one number
// ProcessFrameIdentity carries for every CPU pixel primitive (imagePrimitiveSemanticsVersion); the
// text rasterizer deliberately does not define a second version of its own, which could drift out
// of that identity.
inline constexpr std::uint32_t kCpuImagePrimitiveSemanticsVersion = 5;

// Checked authored layer parameters for the RETAINED pre-S4 translate-only primitive. Translation
// remains Float64 pixel-center displacement; opacity is deliberately rounded once to the Float32
// process precision.
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

// The pre-S4 translate-only resample, RETAINED DELIBERATELY and frozen. The Layer Output stage no
// longer calls it: layerTransformBilinearRow() below does that job for every layer. It stays in the
// library, byte for byte as version 3 shipped it, as the reference a test pins the affine
// primitive's translate-only path against -- it is the actual former implementation, sharing the
// actual interpolation and sampling helpers, so a future edit to either that would silently change
// a translate-only layer's pixels fails that pin instead of shipping. A copy living in the test
// tree could drift apart from those helpers and prove nothing.
//
// Pixel centers have integer local coordinates. The inverse mapping is output minus translation;
// bilinear taps outside the source data window are exact transparent black. Output storage must
// not overlap the source image.
[[nodiscard]] ImageStatus
translateOpacityBilinearRow(Rgba32fImageView source, ImageWindow outputWindow, std::int64_t outputY,
                            TranslationOpacity parameters, std::span<Rgba32f> output) noexcept;

// One layer's complete affine placement: translation composed with rotation composed with scale,
// all turning about an authored anchor, plus the layer's opacity. Construction resolves the
// authored document values into the single inverse 2x2 matrix and pivot the resample needs, so the
// per-pixel path has no trigonometry, no division, and no branching on the authored values.
//
// FORWARD model, in full-resolution composition pixels, with `p` a point of the layer and `q` the
// point of the composition it lands on, both measured from the layer's own centre:
//
//     q = translation + anchor + R(rotation) * S(scale) * (p - anchor)
//
// so the anchor is the one point scale and rotation leave alone, and translation then carries the
// whole layer. `translation` is the displacement the position parameter means (position minus the
// composition-format centre) and `anchor` is the anchor parameter, already centre-relative -- see
// bloom/document/parameter.hpp. R is CLOCKWISE on screen for a positive angle, because Bloom's y
// axis points down.
//
// The layer centre the anchor is measured from is the centre of the layer's PIXEL AREA: pixel
// centres have integer local coordinates, so a w-wide layer occupies [-0.5, w - 0.5] and its centre
// is (w - 1) / 2. That is what makes a quarter turn land pixel centres on pixel centres.
//
// PROXY: a proxy frame is the same picture at a smaller extent, so the full-resolution transform is
// conjugated by the per-axis proxy scale rather than re-authored. With equal horizontal and
// vertical proxy factors -- every proportional proxy extent -- the device transform is therefore
// exactly the full-resolution one; with unequal factors, conjugation keeps the proxy a faithfully
// squashed picture of the full-resolution frame (a rotated layer included), which is the contract
// that matters, rather than pretending device pixels are square.
//
// ROTATION is defined in composition pixels, the convention every timeline compositor uses: a
// non-square composition pixel aspect is not divided out, so a rotated layer turns in pixel space.
//
// EXACTNESS: an exact multiple of 90 degrees uses exact 0 and +/-1 for cosine and sine instead of
// std::cos/std::sin of a rounded radian value, so a quarter turn lands pixel centres on pixel
// centres and interpolates nothing. Scale exactly 1 on both axes together with such a zero rotation
// is the translate-only path, which reproduces translateOpacityBilinearRow()'s arithmetic exactly
// -- the anchor cancels algebraically AND in floating point, because the path never computes with
// it.
class LayerTransform final {
  public:
    // Authored values, in full-resolution composition pixels and degrees.
    struct Authored final {
        double translationX = 0.0;
        double translationY = 0.0;
        double anchorX = 0.0;
        double anchorY = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        double rotationDegrees = 0.0;
        double opacity = 1.0;

        friend constexpr bool operator==(const Authored&, const Authored&) noexcept = default;
    };

    // One inverse-mapped sample coordinate, in source pixels LOCAL to `sourceWindow` (pixel centres
    // at integers, so 0 is the first column of the source data window).
    struct SamplePoint final {
        double x = 0.0;
        double y = 0.0;

        friend constexpr bool operator==(const SamplePoint&, const SamplePoint&) noexcept = default;
    };

    // `sourceWindow` is the layer source image's data window, whose centre is the layer centre the
    // authored values are measured from. `proxyScaleX`/`proxyScaleY` are the evaluated extent
    // divided by the composition format extent, and are exactly 1 for a composition-resolution
    // frame. Rejects a non-finite authored value, an opacity outside [0, 1], a non-positive or
    // non-finite proxy factor, and a collapsed layer -- a scale factor of exactly zero on either
    // axis has no inverse, so the caller must treat it as an empty layer rather than resample it.
    [[nodiscard]] static ImageResult<LayerTransform> create(Authored authored,
                                                            ImageWindow sourceWindow,
                                                            double proxyScaleX,
                                                            double proxyScaleY) noexcept;

    [[nodiscard]] constexpr ImageWindow sourceWindow() const noexcept {
        return state_.sourceWindow;
    }
    [[nodiscard]] constexpr float opacity() const noexcept { return state_.opacity; }
    // True when the resolved device transform is the identity linear map, i.e. the layer only
    // moves. This is the path that is bit-identical to the pre-S4 primitive.
    [[nodiscard]] constexpr bool isTranslationOnly() const noexcept {
        return state_.translationOnly;
    }

    // Inverse mapping of one output pixel centre, in ABSOLUTE composition coordinates, to the
    // source pixel coordinate that lands on it.
    [[nodiscard]] SamplePoint inverseMap(double outputX, double outputY) const noexcept;

    // The output pixels this transform can write a non-transparent value to, clipped to `clip`
    // (the composition's own window). No value means the layer's bilinear support misses `clip`
    // entirely, so the layer contributes nothing at all and needs no image. The bounds are
    // conservative by design: they are the integer bounding box of the forward image of the
    // bilinear support box, which can include a pixel the resample then writes as transparent, but
    // can never exclude one it would write as opaque.
    [[nodiscard]] std::optional<ImageWindow> supportBounds(ImageWindow clip) const noexcept;

  private:
    // Everything the per-pixel path needs, resolved once. Grouped into one aggregate because
    // ImageWindow has no default state to give a member initializer, and because this is exactly
    // the set create() computes and nothing else may touch.
    struct State final {
        ImageWindow sourceWindow;
        float opacity = 1.0F;
        bool translationOnly = true;
        // Device-space translation: the authored translation times the proxy factor, computed with
        // exactly the expression the pre-S4 Layer Output stage used.
        double deviceTranslationX = 0.0;
        double deviceTranslationY = 0.0;
        // Inverse linear map in device space, row major.
        double inverseA = 1.0;
        double inverseB = 0.0;
        double inverseC = 0.0;
        double inverseD = 1.0;
        // Forward linear map in device space: the inverse of the above, precomputed for
        // supportBounds().
        double forwardA = 1.0;
        double forwardB = 0.0;
        double forwardC = 0.0;
        double forwardD = 1.0;
        // The anchor, as a source-local device pixel coordinate and as an absolute output one.
        double anchorLocalX = 0.0;
        double anchorLocalY = 0.0;
        double pivotOutputX = 0.0;
        double pivotOutputY = 0.0;
    };

    explicit constexpr LayerTransform(const State state) noexcept : state_(state) {}

    State state_;
};
// One output row of an affine-resampled layer. `outputWindow` is the window `output` spans -- the
// layer's own data window, which supportBounds() sized -- and its coordinates are ABSOLUTE
// composition coordinates, the same space inverseMap() consumes; `source`'s data window must equal
// the window the transform was created for. Bilinear taps outside the source data window are exact
// transparent black, and because the process representation is premultiplied, interpolating towards
// that transparent black is already the correct edge falloff: no unpremultiply/repremultiply round
// trip is involved and no edge pixel can carry colour above its own alpha. Output storage must not
// overlap the source image.
[[nodiscard]] ImageStatus layerTransformBilinearRow(Rgba32fImageView source,
                                                    ImageWindow outputWindow, std::int64_t outputY,
                                                    const LayerTransform& transform,
                                                    std::span<Rgba32f> output) noexcept;

// Composites source over destination in lin_rec709_scene process space. Destination is the
// in-place output. Source storage must not overlap destination storage. Both spans contain
// premultiplied pixels; process RGB is never clamped.
//
// This is exactly the Normal blend mode, and blendLinearRec709SceneRow() below CALLS it for that
// mode rather than reproducing its arithmetic, which is what makes a Normal layer bit-identical to
// every version before blend modes existed.
[[nodiscard]] ImageStatus sourceOverLinearRec709SceneRow(std::span<const Rgba32f> source,
                                                         std::span<Rgba32f> destination) noexcept;

// Composites source onto destination in lin_rec709_scene process space under one blend mode, with
// the same span contract sourceOverLinearRec709SceneRow() has. ALPHA compositing is source-over for
// every mode -- only the colour combination changes -- so a blend mode never makes a layer cover
// more or less of what is beneath it than its own alpha says, and Normal is the case where the
// colour combination is source-over too.
//
// The formulas, and why each one is evaluated where it is, are in
// docs/architecture/color-management.md, "Blend modes". In brief: Add is premultiplied addition,
// which the general compositing formula reduces to exactly, so it needs no round trip; every other
// non-Normal mode is a separable function of UN-premultiplied colour, so the kernel divides both
// pixels by their own alpha, applies the mode, and re-premultiplies through the general formula.
// Process RGB is never clamped, at either end.
[[nodiscard]] ImageStatus blendLinearRec709SceneRow(core::BlendMode mode,
                                                    std::span<const Rgba32f> source,
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
