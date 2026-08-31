#include <bloom/color/ocio_cpu_display_frame.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::ImageExtent;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// The five-pixel deterministic fixture required by the task package's test list: negative, HDR,
// zero-alpha, translucent, and opaque premultiplied RGBA32F samples, laid out left to right in a
// single 5x1 row.
struct FixturePixel final {
    float red;
    float green;
    float blue;
    float alpha;
};

constexpr std::array<FixturePixel, 5> kFixture{{
    {0.5F, 0.25F, 0.1F, 1.0F},     // opaque
    {0.15F, 0.05F, 0.025F, 0.5F},  // translucent (straight = 0.3, 0.1, 0.05)
    {0.7F, 0.7F, 0.7F, 0.0F},      // zero-alpha (canonicalizes to transparent black)
    {-0.1F, -0.05F, -0.02F, 1.0F}, // negative
    {4.0F, 2.0F, 1.0F, 1.0F},      // HDR
}};

[[nodiscard]] std::optional<Rgba32fImage> buildFixtureImage() {
    const auto extentResult = ImageExtent::create(kFixture.size(), 1);
    if (!extentResult) {
        return std::nullopt;
    }
    const auto windowResult = ImageWindow::create(0, 0, kFixture.size(), 1);
    if (!windowResult) {
        return std::nullopt;
    }
    const auto window = *windowResult.value();
    const auto descriptorResult =
        Rgba32fImageDescriptor::create(window, window, bloom::core::PixelAspectRatio::square());
    if (!descriptorResult) {
        return std::nullopt;
    }
    auto builderResult = Rgba32fImageBuilder::create(*descriptorResult.value(), 1U << 20U);
    if (!builderResult) {
        return std::nullopt;
    }
    auto builder = std::move(*builderResult.value());
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        const auto& fixture = kFixture[i];
        const auto pixelResult =
            Rgba32f::fromPremultiplied(fixture.red, fixture.green, fixture.blue, fixture.alpha);
        if (!pixelResult) {
            return std::nullopt;
        }
        const auto writeStatus =
            builder.write(static_cast<std::int64_t>(i), 0, *pixelResult.value());
        if (writeStatus.has_value()) {
            return std::nullopt;
        }
    }
    auto frozen = std::move(builder).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

[[nodiscard]] std::optional<bloom::color::PreparedCpuDisplayProcessorHandle> buildNeutralHandle() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (resolution.outcome() != bloom::color::OcioBuiltInRegistryOutcome::Ready) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto buildResult = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!buildResult) {
        return std::nullopt;
    }
    return std::move(buildResult).takeHandle();
}

[[nodiscard]] std::string hexByte(const std::uint8_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return stream.str();
}

void printPixelsForDerivation(const std::span<const Rgba8> pixels) {
    // Printed only to stderr as derivation evidence; not itself a test assertion. See
    // testPixelGolden's comment for how the pinned golden below was captured.
    std::cerr << "[derivation] observed RGBA8 bytes: ";
    for (const auto& pixel : pixels) {
        std::cerr << hexByte(pixel.red) << hexByte(pixel.green) << hexByte(pixel.blue)
                  << hexByte(pixel.alpha) << ' ';
    }
    std::cerr << '\n';
}

// Pinned golden RGBA8 bytes for kFixture above, applying the qualified Bloom Neutral v1 CPU
// display processor. Derivation: assets/ocio/neutral-v1/config.ocio's srgb_rec709_display space
// declares from_scene_reference as OCIO's ExponentWithLinearTransform{gamma: 2.4, offset: 0.055,
// direction: inverse}, which assets/ocio/neutral-v1/provenance.md independently verified against
// the closed-form sRGB OETF (v = 12.92*l at/below the 0.0031308 breakpoint, v = 1.055*l^(1/2.4) -
// 0.055 above it, mirrored through the origin for negative l) to within 6.7e-6 float32
// discrepancy -- far below one 8-bit quantization step (~0.0039). This test's golden bytes were
// captured by running this exact adapter (applyBloomNeutralDisplayChunk via
// produceBloomNeutralDisplayFrame) once against the fixture above and pinning the observed
// output; testPixelGoldenClosedFormCrossCheck below independently re-derives the same bytes from
// the closed-form formula as corroborating evidence that the pinned bytes are not an accidental
// capture of a regression. Re-derive by rebuilding bloom_color_ocio_cpu_display_frame_test and
// reading its "[derivation]" stderr line if this fixture ever changes.
constexpr std::array<Rgba8, kFixture.size()> kExpectedPixels{{
    Rgba8{188, 137, 89, 255},
    Rgba8{149, 89, 63, 128},
    Rgba8{0, 0, 0, 0},
    Rgba8{0, 0, 0, 255},
    Rgba8{255, 255, 255, 255},
}};

void testDeterministicPixelGolden(Expectations& expectations) {
    auto image = buildFixtureImage();
    expectations.expect(image.has_value(), "the fixture image builds successfully");
    auto handle = buildNeutralHandle();
    expectations.expect(handle.has_value(), "the Bloom Neutral processor handle builds");
    if (!image.has_value() || !handle.has_value()) {
        return;
    }
    const auto view = image->view();
    expectations.expect(static_cast<bool>(view), "the fixture image exposes a valid view");
    if (!view) {
        return;
    }

    auto result = bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(),
                                                                kFixture.size(), 1U << 20U);
    expectations.expect(static_cast<bool>(result), "the full-frame flow succeeds for the fixture");
    if (!result) {
        return;
    }
    const auto pixels = result.value()->pixels();
    printPixelsForDerivation(pixels);
    expectations.expect(pixels.size() == kFixture.size(), "the output pixel count matches");
    if (pixels.size() != kFixture.size()) {
        return;
    }
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        expectations.expect(pixels[i] == kExpectedPixels[i],
                            "pixel matches the pinned RGBA8 golden for its fixture index");
    }

    // Zero-alpha canonicalization is pinned explicitly: fixture index 2 has premultiplied alpha
    // exactly 0, so its straight RGB is canonicalized to positive zero before the OCIO transform,
    // and its packed alpha byte is exactly 0.
    expectations.expect(pixels[2].alpha == 0, "the zero-alpha fixture pixel packs to alpha byte 0");
}

// Independent corroboration for the pinned golden above, computed from the closed-form sRGB OETF
// rather than by re-running the adapter under test. Uses a generous tolerance (2 quantization
// steps) because provenance.md's own review only bounds the config's transform against the
// closed form to 6.7e-6 in the float32 domain, and this cross-check additionally goes through an
// independent double-precision reimplementation of that same formula plus the documented
// clamp/quantize packing rule -- it corroborates the pinned bytes rather than replacing them as
// the primary regression oracle.
[[nodiscard]] double srgbOetf(const double linear) {
    const double magnitude = linear < 0.0 ? -linear : linear;
    const double encoded =
        magnitude <= 0.0031308 ? 12.92 * magnitude : 1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055;
    return linear < 0.0 ? -encoded : encoded;
}

[[nodiscard]] std::uint8_t clampQuantize(const double value) {
    const double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<std::uint8_t>(std::floor(clamped * 255.0 + 0.5));
}

void testPixelGoldenClosedFormCrossCheck(Expectations& expectations) {
    struct Straight final {
        double red;
        double green;
        double blue;
        double alpha;
    };
    constexpr std::array<Straight, kFixture.size()> straight{{
        {0.5, 0.25, 0.1, 1.0},
        {0.3, 0.1, 0.05, 0.5},
        {0.0, 0.0, 0.0, 0.0},
        {-0.1, -0.05, -0.02, 1.0},
        {4.0, 2.0, 1.0, 1.0},
    }};
    for (std::size_t i = 0; i < straight.size(); ++i) {
        const auto expectedRed = clampQuantize(srgbOetf(straight[i].red));
        const auto expectedGreen = clampQuantize(srgbOetf(straight[i].green));
        const auto expectedBlue = clampQuantize(srgbOetf(straight[i].blue));
        const auto expectedAlpha = clampQuantize(straight[i].alpha);
        const auto redOk =
            static_cast<int>(expectedRed) - static_cast<int>(kExpectedPixels[i].red) <= 2 &&
            static_cast<int>(kExpectedPixels[i].red) - static_cast<int>(expectedRed) <= 2;
        expectations.expect(redOk, "closed-form red cross-check is within 2 quantization steps");
        const auto greenOk =
            static_cast<int>(expectedGreen) - static_cast<int>(kExpectedPixels[i].green) <= 2 &&
            static_cast<int>(kExpectedPixels[i].green) - static_cast<int>(expectedGreen) <= 2;
        expectations.expect(greenOk,
                            "closed-form green cross-check is within 2 quantization steps");
        const auto blueOk =
            static_cast<int>(expectedBlue) - static_cast<int>(kExpectedPixels[i].blue) <= 2 &&
            static_cast<int>(kExpectedPixels[i].blue) - static_cast<int>(expectedBlue) <= 2;
        expectations.expect(blueOk, "closed-form blue cross-check is within 2 quantization steps");
        expectations.expect(expectedAlpha == kExpectedPixels[i].alpha,
                            "alpha packing matches the straight-alpha clamp/quantize rule exactly");
    }
}

void testChunkSizeIndependence(Expectations& expectations) {
    auto image = buildFixtureImage();
    auto handle = buildNeutralHandle();
    expectations.expect(image.has_value() && handle.has_value(),
                        "fixture and handle build for the chunk-independence test");
    if (!image.has_value() || !handle.has_value()) {
        return;
    }
    const auto view = image->view();
    expectations.expect(static_cast<bool>(view), "fixture view is valid");
    if (!view) {
        return;
    }

    std::optional<std::vector<Rgba8>> reference;
    for (const std::size_t chunkSize : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
        auto result = bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(),
                                                                    chunkSize, 1U << 20U);
        expectations.expect(static_cast<bool>(result), "each chunk size succeeds");
        if (!result) {
            continue;
        }
        const auto pixels = result.value()->pixels();
        std::vector<Rgba8> observed(pixels.begin(), pixels.end());
        if (!reference.has_value()) {
            reference = observed;
        } else {
            expectations.expect(observed == *reference,
                                "different chunk sizes produce bit-identical output");
        }
    }
}

void testDeterminismAcrossRuns(Expectations& expectations) {
    auto image = buildFixtureImage();
    auto handle = buildNeutralHandle();
    expectations.expect(image.has_value() && handle.has_value(),
                        "fixture and handle build for the determinism test");
    if (!image.has_value() || !handle.has_value()) {
        return;
    }
    const auto view = image->view();
    expectations.expect(static_cast<bool>(view), "fixture view is valid");
    if (!view) {
        return;
    }
    auto first =
        bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(), 2, 1U << 20U);
    auto second =
        bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(), 2, 1U << 20U);
    expectations.expect(static_cast<bool>(first) && static_cast<bool>(second),
                        "two independent runs both succeed");
    if (first && second) {
        const auto firstPixels = first.value()->pixels();
        const auto secondPixels = second.value()->pixels();
        expectations.expect(std::vector<Rgba8>(firstPixels.begin(), firstPixels.end()) ==
                                std::vector<Rgba8>(secondPixels.begin(), secondPixels.end()),
                            "two independent runs are bit-equal");
    }
}

void testCancellationPublishesNothing(Expectations& expectations) {
    auto image = buildFixtureImage();
    auto handle = buildNeutralHandle();
    expectations.expect(image.has_value() && handle.has_value(),
                        "fixture and handle build for the cancellation test");
    if (!image.has_value() || !handle.has_value()) {
        return;
    }
    const auto view = image->view();
    if (!view) {
        return;
    }
    bool cancelNow = true;
    auto alwaysCancelled = [&cancelNow]() { return cancelNow; };
    auto result = bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(), 1,
                                                                1U << 20U, alwaysCancelled);
    expectations.expect(!static_cast<bool>(result),
                        "cancellation at the first chunk boundary publishes no frame");
}

void testBudgetExhaustionTyped(Expectations& expectations) {
    auto image = buildFixtureImage();
    auto handle = buildNeutralHandle();
    expectations.expect(image.has_value() && handle.has_value(),
                        "fixture and handle build for the budget test");
    if (!image.has_value() || !handle.has_value()) {
        return;
    }
    const auto view = image->view();
    if (!view) {
        return;
    }
    // The fixture needs 5 * sizeof(Rgba8) = 20 bytes; a 1-byte limit cannot admit it.
    auto result =
        bloom::color::produceBloomNeutralDisplayFrame(*handle, *view.value(), kFixture.size(), 1);
    expectations.expect(!static_cast<bool>(result), "an insufficient budget fails typed");
    if (!result) {
        const auto* error = result.error();
        expectations.expect(error != nullptr &&
                                error->code ==
                                    bloom::render::ImageErrorCode::PixelStorageBudgetExceeded,
                            "the typed failure is exactly PixelStorageBudgetExceeded");
        expectations.expect(error != nullptr && error->requestedPixelStorageBytes.has_value() &&
                                *error->requestedPixelStorageBytes == kFixture.size() * 4,
                            "the error reports the exact requested byte count, not zero/garbage");
        expectations.expect(error != nullptr && error->pixelStorageByteLimit.has_value() &&
                                *error->pixelStorageByteLimit == 1,
                            "the error echoes back the exact caller-supplied byte limit");
    }
}

void testNonFiniteDivisionRejectedNoPartialFrame(Expectations& expectations) {
    // bloom::render::Rgba32f::fromPremultiplied already rejects a non-finite raw component at
    // construction (image_types.cpp), so a genuinely non-finite *input* Rgba32f cannot exist --
    // the reachable "non-finite" case in the documented seven-step flow is step 2's division
    // result: a large but individually finite premultiplied component divided by a small but
    // finite positive alpha can overflow float32 to infinity. This fixture (red just below
    // float32's max finite value, alpha 1e-6) produces exactly that: red / alpha overflows.
    auto handle = buildNeutralHandle();
    expectations.expect(handle.has_value(), "handle builds for the non-finite-division test");
    if (!handle.has_value()) {
        return;
    }
    const auto pixelResult = Rgba32f::fromPremultiplied(3.0e38F, 0.0F, 0.0F, 1.0e-6F);
    expectations.expect(static_cast<bool>(pixelResult), "the overflow fixture pixel constructs");
    if (!pixelResult) {
        return;
    }
    const std::array<Rgba32f, 1> source{*pixelResult.value()};
    std::array<float, 4> scratch{};
    std::array<Rgba8, 1> destination{Rgba8{9, 9, 9, 9}};
    const auto status =
        bloom::color::applyBloomNeutralDisplayChunk(*handle, source, scratch, destination);
    expectations.expect(status.has_value(), "an overflowing unpremultiply division is rejected");
    expectations.expect(status.has_value() &&
                            status->code == bloom::render::ImageErrorCode::NonFiniteResult,
                        "the rejection is typed NonFiniteResult");
    expectations.expect(destination[0] == Rgba8{9, 9, 9, 9},
                        "the caller's destination buffer is left untouched (no partial write "
                        "the caller could mistake for a real pixel)");
}

// The Bloom Neutral v1 transform (a bounded, sub-linear power function per fixture derivation
// above) compresses rather than expands, so no finite float32 RGB input was found that drives its
// OCIO CPU processor output to a non-finite value within the time available for this task; step
// 5's non-finite-processor-output rejection exists and is exercised structurally (the same
// isfinite check as step 1/2, applied to scratchRgba after OCIO::CPUProcessor::apply, is plain
// C++ code with no OCIO-specific branch), but this suite does not claim a crafted fixture reaches
// it, per the task package's explicit "if achievable, else document why not" allowance.

} // namespace

int main() {
    Expectations expectations;
    testDeterministicPixelGolden(expectations);
    testPixelGoldenClosedFormCrossCheck(expectations);
    testChunkSizeIndependence(expectations);
    testDeterminismAcrossRuns(expectations);
    testCancellationPublishesNothing(expectations);
    testBudgetExhaustionTyped(expectations);
    testNonFiniteDivisionRejectedNoPartialFrame(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
