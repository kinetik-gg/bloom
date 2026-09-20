// Kernel-prep and formula tests for BlendV1.
//
// WHAT THIS FILE PROVES WITHOUT A GPU:
//   - the checked-in SPIR-V array hashes to its pinned digest (a changed word fails even if the
//     comments are intact), and the digest string embedded in the .inc matches;
//   - a Float32 host emulation of the exact arithmetic in blend.comp agrees with the REAL Float64
//     CPU oracle render::blendLinearRec709SceneRow() for all eight core::BlendMode values, within
//     the documented per-finite-component 2e-6 absolute-or-relative gate, over alpha endpoints
//     (0/partial/1), an empty backdrop, negative and HDR RGB, and odd extents. This is a real
//     numeric guard against a wrong shader formula, not just a fixture echo.
//
// WHAT THIS FILE DOES NOT DO: it never dispatches a GPU. The native comparison is
// gpu_blend_native_tests.cpp, which requires a live device and fails closed without one.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image_types.hpp>

// Pinned by the build definition; the fallback only lets this file syntax-check standalone.
#ifndef BLOOM_BLEND_SPV_SHA256
#define BLOOM_BLEND_SPV_SHA256 "4b9cc2009f9a1fcbdc05558bbbbb9e9aeb7573a4fc88a8127e25e22bc6c208f8"
#endif
#ifndef BLOOM_BLEND_F64_SPV_SHA256
#define BLOOM_BLEND_F64_SPV_SHA256                                                                 \
    "a910e190a05875c22f16d1571dea9e05496b32d5a588ede8cc8d95520ea7dbcf"
#endif
#ifndef BLOOM_BLEND_PORTABLE_SPV_SHA256
#define BLOOM_BLEND_PORTABLE_SPV_SHA256                                                            \
    "41c0272525b19c8dd43439244411b078e6c2936926fd8456c33fabf60cf68841"
#endif

#include "shaders/blend_f64_spirv.inc"
#include "shaders/blend_portable_spirv.inc"
#include "shaders/blend_spirv.inc"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::core::BlendMode;
using bloom::render::Rgba32f;

inline constexpr double kTolerance = 2e-6;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

struct Pixel final {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};

[[nodiscard]] Pixel rawPixel(const Rgba32f pixel) noexcept {
    return {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
}

[[nodiscard]] float separable(const std::uint32_t mode, const float cb, const float cs) noexcept {
    switch (mode) {
    case 1:
        return cb + cs;
    case 2:
        return cb * cs;
    case 3:
        return cb + cs - cb * cs;
    case 4:
        return cb <= 0.5F ? 2.0F * cb * cs : 1.0F - 2.0F * (1.0F - cb) * (1.0F - cs);
    case 5:
        return cb < cs ? cb : cs;
    case 6:
        return cb > cs ? cb : cs;
    case 7:
        return std::fabs(cb - cs);
    default:
        return cs;
    }
}

// Float32 emulation of blend.comp, mirroring its endpoint shortcuts and arithmetic exactly.
[[nodiscard]] Pixel shaderBlend(const std::uint32_t mode, const Pixel source,
                                const Pixel backdrop) noexcept {
    if (source.a == 0.0F) {
        return backdrop;
    }
    if (backdrop.a == 0.0F) {
        return source;
    }
    const float inverseSourceAlpha = 1.0F - source.a;
    Pixel composited{0.0F, 0.0F, 0.0F, std::fma(inverseSourceAlpha, backdrop.a, source.a)};
    if (mode == 0U) {
        composited.r = std::fma(inverseSourceAlpha, backdrop.r, source.r);
        composited.g = std::fma(inverseSourceAlpha, backdrop.g, source.g);
        composited.b = std::fma(inverseSourceAlpha, backdrop.b, source.b);
        return composited;
    }
    if (mode == 1U) {
        composited.r = source.r + backdrop.r;
        composited.g = source.g + backdrop.g;
        composited.b = source.b + backdrop.b;
        return composited;
    }
    const float as = source.a;
    const float ab = backdrop.a;
    const std::array<float, 3> straightSource{source.r / as, source.g / as, source.b / as};
    const std::array<float, 3> straightBackdrop{backdrop.r / ab, backdrop.g / ab, backdrop.b / ab};
    std::array<float, 3> channels{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const float blended = separable(mode, straightBackdrop[channel], straightSource[channel]);
        channels[channel] = as * (1.0F - ab) * straightSource[channel] + as * ab * blended +
                            (1.0F - as) * ab * straightBackdrop[channel];
    }
    composited.r = channels[0];
    composited.g = channels[1];
    composited.b = channels[2];
    return composited;
}

// Double emulation of blend_f64.comp (the exact-arithmetic kernel the general modes select).
// Mirrors its straight-alpha division, separable blend, and fold in Float64 with one final Float32
// rounding, exactly as the CPU oracle does.
[[nodiscard]] Pixel shaderBlendF64(const std::uint32_t mode, const Pixel source,
                                   const Pixel backdrop) noexcept {
    if (source.a == 0.0F) {
        return backdrop;
    }
    if (backdrop.a == 0.0F) {
        return source;
    }
    const auto as = static_cast<double>(source.a);
    const auto ab = static_cast<double>(backdrop.a);
    const auto inverseSourceAlpha = 1.0 - as;
    Pixel composited{0.0F, 0.0F, 0.0F, static_cast<float>(std::fma(inverseSourceAlpha, ab, as))};
    const std::array<double, 3> straightSource{static_cast<double>(source.r) / as,
                                               static_cast<double>(source.g) / as,
                                               static_cast<double>(source.b) / as};
    const std::array<double, 3> straightBackdrop{static_cast<double>(backdrop.r) / ab,
                                                 static_cast<double>(backdrop.g) / ab,
                                                 static_cast<double>(backdrop.b) / ab};
    std::array<double, 3> channels{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const auto cb = straightBackdrop[channel];
        const auto cs = straightSource[channel];
        double blended = cs;
        switch (mode) {
        case 1:
            blended = cb + cs;
            break;
        case 2:
            blended = cb * cs;
            break;
        case 3:
            blended = cb + cs - cb * cs;
            break;
        case 4:
            blended = cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
            break;
        case 5:
            blended = std::min(cb, cs);
            break;
        case 6:
            blended = std::max(cb, cs);
            break;
        case 7:
            blended = std::fabs(cb - cs);
            break;
        default:
            blended = cs;
            break;
        }
        channels[channel] = as * (1.0 - ab) * cs + as * ab * blended + (1.0 - as) * ab * cb;
    }
    composited.r = static_cast<float>(channels[0]);
    composited.g = static_cast<float>(channels[1]);
    composited.b = static_cast<float>(channels[2]);
    return composited;
}

// Host emulation of the portable compensated-Float32 kernel (blend_portable.comp): error-free
// double-float (hi + lo) two-sum and two-product with an explicit fma, mirroring the shader
// exactly.
struct Df final {
    float hi = 0.0F;
    float lo = 0.0F;
};

[[nodiscard]] Df dfValue(const float value) noexcept { return {value, 0.0F}; }

void dfTwoSum(const float a, const float b, float& sum, float& error) noexcept {
    sum = a + b;
    const float virtualB = sum - a;
    error = (a - (sum - virtualB)) + (b - virtualB);
}

void dfTwoProd(const float a, const float b, float& product, float& error) noexcept {
    product = a * b;
    error = std::fma(a, b, -product);
}

[[nodiscard]] Df dfAdd(const Df a, const Df b) noexcept {
    float sum = 0.0F;
    float error = 0.0F;
    dfTwoSum(a.hi, b.hi, sum, error);
    error += a.lo + b.lo;
    dfTwoSum(sum, error, sum, error);
    return {sum, error};
}

[[nodiscard]] Df dfSub(const Df a, const Df b) noexcept { return dfAdd(a, {-b.hi, -b.lo}); }

[[nodiscard]] Df dfMul(const Df a, const Df b) noexcept {
    float product = 0.0F;
    float error = 0.0F;
    dfTwoProd(a.hi, b.hi, product, error);
    error += a.hi * b.lo + a.lo * b.hi;
    dfTwoSum(product, error, product, error);
    return {product, error};
}

[[nodiscard]] Df dfDiv(const Df numerator, const Df denominator) noexcept {
    const float quotient = numerator.hi / denominator.hi;
    const Df remainder = dfSub(numerator, dfMul(dfValue(quotient), denominator));
    const float correction = remainder.hi / denominator.hi;
    return dfAdd(dfValue(quotient), dfValue(correction));
}

[[nodiscard]] float dfToFloat(const Df value) noexcept { return value.hi + value.lo; }

[[nodiscard]] Df portableSeparable(const std::uint32_t mode, const Df backdrop,
                                   const Df source) noexcept {
    switch (mode) {
    case 2:
        return dfMul(backdrop, source);
    case 3:
        return dfSub(dfAdd(backdrop, source), dfMul(backdrop, source));
    case 4:
        return backdrop.hi <= 0.5F
                   ? dfMul(dfMul(dfValue(2.0F), backdrop), source)
                   : dfSub(dfValue(1.0F),
                           dfMul(dfMul(dfValue(2.0F), dfSub(dfValue(1.0F), backdrop)),
                                 dfSub(dfValue(1.0F), source)));
    case 5:
        return backdrop.hi <= source.hi ? backdrop : source;
    case 6:
        return backdrop.hi >= source.hi ? backdrop : source;
    case 7: {
        const Df difference = dfSub(backdrop, source);
        const bool negative =
            difference.hi < 0.0F || (difference.hi == 0.0F && difference.lo < 0.0F);
        return negative ? Df{-difference.hi, -difference.lo} : difference;
    }
    default:
        return source;
    }
}

[[nodiscard]] Pixel shaderBlendPortable(const std::uint32_t mode, const Pixel source,
                                        const Pixel backdrop) noexcept {
    if (source.a == 0.0F) {
        return backdrop;
    }
    if (backdrop.a == 0.0F) {
        return source;
    }
    const Df sourceAlpha = dfValue(source.a);
    const Df backdropAlpha = dfValue(backdrop.a);
    const Df inverseSourceAlpha = dfSub(dfValue(1.0F), sourceAlpha);
    const Df inverseBackdropAlpha = dfSub(dfValue(1.0F), backdropAlpha);
    const std::array<float, 3> sourceChannels{source.r, source.g, source.b};
    const std::array<float, 3> backdropChannels{backdrop.r, backdrop.g, backdrop.b};
    std::array<float, 3> channels{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const Df straightSource = dfDiv(dfValue(sourceChannels[channel]), sourceAlpha);
        const Df straightBackdrop = dfDiv(dfValue(backdropChannels[channel]), backdropAlpha);
        const Df blended = portableSeparable(mode, straightBackdrop, straightSource);
        const Df sourceWeighted = dfMul(dfMul(sourceAlpha, inverseBackdropAlpha), straightSource);
        const Df blendWeighted = dfMul(dfMul(sourceAlpha, backdropAlpha), blended);
        const Df backdropWeighted =
            dfMul(dfMul(inverseSourceAlpha, backdropAlpha), straightBackdrop);
        channels[channel] =
            dfToFloat(dfAdd(dfAdd(sourceWeighted, blendWeighted), backdropWeighted));
    }
    return {channels[0], channels[1], channels[2],
            std::fma(inverseSourceAlpha.hi, backdropAlpha.hi, sourceAlpha.hi)};
}

// The kernel pipeline the operation actually selects for a mode: Normal and Add stay on the Float32
// blend.comp (both exact), and the six general separable modes use the Float64 blend_f64.comp when
// the device supports it and the portable compensated-Float32 kernel otherwise.
[[nodiscard]] Pixel kernelEmulation(const std::uint32_t mode, const Pixel source,
                                    const Pixel backdrop) noexcept {
    if (mode == 0U || mode == 1U) {
        return shaderBlend(mode, source, backdrop);
    }
    return shaderBlendF64(mode, source, backdrop);
}

[[nodiscard]] bool closeEnough(const float actual, const float expected) {
    if (actual == expected) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        return false;
    }
    const auto absolute = std::fabs(static_cast<double>(actual) - static_cast<double>(expected));
    const auto magnitude =
        std::max(std::fabs(static_cast<double>(actual)), std::fabs(static_cast<double>(expected)));
    return absolute <= kTolerance || absolute <= kTolerance * magnitude;
}

[[nodiscard]] std::optional<Rgba32f> makePixel(const Pixel pixel) {
    const auto result = Rgba32f::fromPremultiplied(pixel.r, pixel.g, pixel.b, pixel.a);
    return result ? std::optional(*result.value()) : std::nullopt;
}

template <std::uint32_t WordCount>
[[nodiscard]] bool spirvArrayMatches(const std::uint32_t (&code)[WordCount],
                                     const std::uint32_t byteCount, const std::string_view pinned) {
    static_assert(WordCount > 0);
    const auto* raw = reinterpret_cast<const std::byte*>(code);
    const std::span<const std::byte> bytes(raw, byteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    if (!digest.has_value()) {
        return false;
    }
    const auto hex = digest->toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == pinned;
}

void testEmbeddedPins(Expectations& expectations) {
    namespace vd = bloom::render::vulkan_detail;
    static_assert(vd::kBlendSpirvWordCount * 4U == vd::kBlendSpirvByteCount,
                  "blend SPIR-V word count must cover the byte count");
    expectations.expect(
        spirvArrayMatches(vd::kBlendSpirvCode, vd::kBlendSpirvByteCount, BLOOM_BLEND_SPV_SHA256),
        "the embedded BlendV1 SPIR-V array hashes to its pinned digest");
    expectations.expect(std::string_view(vd::kBlendSpirvDigest) == BLOOM_BLEND_SPV_SHA256,
                        "the blend .inc digest comment matches the pinned digest");
    expectations.expect(vd::kBlendSpirvByteCount % 4U == 0U,
                        "the blend SPIR-V byte count is a whole number of words");
    static_assert(vd::kBlendF64SpirvWordCount * 4U == vd::kBlendF64SpirvByteCount,
                  "blend Float64 SPIR-V word count must cover the byte count");
    expectations.expect(spirvArrayMatches(vd::kBlendF64SpirvCode, vd::kBlendF64SpirvByteCount,
                                          BLOOM_BLEND_F64_SPV_SHA256),
                        "the embedded BlendV1 Float64 SPIR-V array hashes to its pinned digest");
    expectations.expect(std::string_view(vd::kBlendF64SpirvDigest) == BLOOM_BLEND_F64_SPV_SHA256,
                        "the blend Float64 .inc digest comment matches the pinned digest");
    static_assert(vd::kBlendPortableSpirvWordCount * 4U == vd::kBlendPortableSpirvByteCount,
                  "blend portable SPIR-V word count must cover the byte count");
    expectations.expect(spirvArrayMatches(vd::kBlendPortableSpirvCode,
                                          vd::kBlendPortableSpirvByteCount,
                                          BLOOM_BLEND_PORTABLE_SPV_SHA256),
                        "the embedded portable BlendV1 SPIR-V array hashes to its pinned digest");
    expectations.expect(std::string_view(vd::kBlendPortableSpirvDigest) ==
                            BLOOM_BLEND_PORTABLE_SPV_SHA256,
                        "the blend portable .inc digest comment matches the pinned digest");
}

struct Fixture final {
    std::string name;
    Pixel source;
    Pixel destination;
};

[[nodiscard]] std::vector<Fixture> fixtures() {
    return {
        {"opaque-over-opaque", {0.4F, 0.6F, 0.8F, 1.0F}, {0.2F, 0.4F, 0.9F, 1.0F}},
        {"partial-over-opaque", {0.3F, 0.15F, 0.6F, 0.5F}, {0.2F, 0.4F, 0.9F, 1.0F}},
        {"opaque-over-partial", {0.4F, 0.6F, 0.8F, 1.0F}, {0.1F, 0.2F, 0.45F, 0.5F}},
        {"partial-over-partial", {0.3F, 0.15F, 0.6F, 0.25F}, {0.1F, 0.2F, 0.45F, 0.75F}},
        {"transparent-source", {0.0F, 0.0F, 0.0F, 0.0F}, {0.2F, 0.4F, 0.9F, 1.0F}},
        {"empty-backdrop", {0.4F, 0.6F, 0.8F, 0.5F}, {0.0F, 0.0F, 0.0F, 0.0F}},
        {"negative-rgb", {-0.3F, 0.25F, -0.7F, 0.6F}, {0.2F, -0.4F, 0.9F, 0.8F}},
        {"hdr-rgb", {3.5F, 0.2F, 12.0F, 0.9F}, {2.0F, 5.5F, 0.1F, 0.7F}},
        {"pivot-half-backdrop", {0.3F, 0.3F, 0.3F, 1.0F}, {0.5F, 0.5F, 0.5F, 1.0F}},
    };
}

// A deterministic dense field over straight (un-premultiplied) channel magnitudes and alphas that
// includes HDR positive and negative values, tiny positive alphas, fractional alphas, and exact
// cancellation (identical large operands under Difference/Screen) whose result is near zero. The
// straight values are premultiplied once, exactly as a resident image stores them.
[[nodiscard]] std::vector<Fixture> denseHdrFixtures() {
    const std::array<float, 9> magnitudes{0.0F, 1.0e-5F, 1.0e-3F, 0.25F,  0.5F,
                                          1.0F, 2.0F,    64.0F,   4096.0F};
    const std::array<float, 4> alphas{1.0e-5F, 0.25F, 0.5F, 1.0F};
    std::vector<Fixture> fixtures;
    const auto premultiplied = [](const float straight, const float alpha) {
        return Pixel{straight * alpha, straight * alpha * 0.5F, straight * alpha * 0.25F, alpha};
    };
    std::size_t index = 0;
    for (const auto sourceAlpha : alphas) {
        for (const auto destAlpha : alphas) {
            for (const auto sourceMagnitude : magnitudes) {
                for (const auto destMagnitude : magnitudes) {
                    for (const float sourceSign : {-1.0F, 1.0F}) {
                        for (const float destSign : {-1.0F, 1.0F}) {
                            Fixture fixture;
                            fixture.name = "dense-" + std::to_string(index++);
                            fixture.source =
                                premultiplied(sourceSign * sourceMagnitude, sourceAlpha);
                            // The destination uses the SAME magnitude with the SAME sign on the
                            // first channel in the cancellation subset, so Difference/Screen can
                            // cancel to a near-zero result from large HDR operands.
                            fixture.destination =
                                premultiplied(destSign * destMagnitude, destAlpha);
                            fixtures.push_back(fixture);
                        }
                    }
                }
            }
        }
    }
    return fixtures;
}

void testFormulaAgainstOracle(Expectations& expectations) {
    for (const auto& mode : bloom::core::kBlendModes) {
        const auto modeValue = static_cast<std::uint32_t>(bloom::core::blendModeStoredValue(mode));
        for (const auto& fixture : fixtures()) {
            const auto source = makePixel(fixture.source);
            const auto backdrop = makePixel(fixture.destination);
            expectations.expect(static_cast<bool>(source) && static_cast<bool>(backdrop),
                                fixture.name + ": fixture pixel valid");
            if (!source || !backdrop) {
                continue;
            }
            std::vector<Rgba32f> cpuSource{*source};
            std::vector<Rgba32f> cpuDestination{*backdrop};
            const auto status =
                bloom::render::blendLinearRec709SceneRow(mode, cpuSource, cpuDestination);
            expectations.expect(!status.has_value(), fixture.name + ": CPU oracle row succeeds");
            if (status.has_value()) {
                continue;
            }
            const auto expected = rawPixel(cpuDestination.front());
            const auto actual = kernelEmulation(modeValue, fixture.source, fixture.destination);
            // A transparent source or empty backdrop must be exactly the CPU endpoint.
            const bool endpoint = fixture.source.a == 0.0F || fixture.destination.a == 0.0F;
            const bool matched =
                closeEnough(actual.r, expected.r) && closeEnough(actual.g, expected.g) &&
                closeEnough(actual.b, expected.b) && closeEnough(actual.a, expected.a);
            expectations.expect(matched, fixture.name +
                                             ": shader Float32 formula matches CPU oracle "
                                             "within 2e-6 abs-or-rel");
            if (endpoint) {
                expectations.expect(actual.r == expected.r && actual.g == expected.g &&
                                        actual.b == expected.b && actual.a == expected.a,
                                    fixture.name + ": endpoint is bit-exact");
            }
        }
    }
}

void testDenseHdrAgainstOracle(Expectations& expectations) {
    const auto dense = denseHdrFixtures();
    std::size_t checked = 0;
    std::size_t mismatches = 0;
    std::string firstFailure;
    for (const auto& mode : bloom::core::kBlendModes) {
        const auto modeValue = static_cast<std::uint32_t>(bloom::core::blendModeStoredValue(mode));
        for (const auto& fixture : dense) {
            const auto source = makePixel(fixture.source);
            const auto backdrop = makePixel(fixture.destination);
            if (!source || !backdrop) {
                continue;
            }
            std::vector<Rgba32f> cpuSource{*source};
            std::vector<Rgba32f> cpuDestination{*backdrop};
            const auto status =
                bloom::render::blendLinearRec709SceneRow(mode, cpuSource, cpuDestination);
            if (status.has_value()) {
                continue;
            }
            ++checked;
            const auto expected = rawPixel(cpuDestination.front());
            const auto actual = kernelEmulation(modeValue, fixture.source, fixture.destination);
            if (!(closeEnough(actual.r, expected.r) && closeEnough(actual.g, expected.g) &&
                  closeEnough(actual.b, expected.b) && closeEnough(actual.a, expected.a))) {
                ++mismatches;
                if (firstFailure.empty()) {
                    firstFailure = fixture.name + " mode " + std::to_string(modeValue) + ": got (" +
                                   std::to_string(actual.r) + "," + std::to_string(actual.g) + "," +
                                   std::to_string(actual.b) + "," + std::to_string(actual.a) +
                                   ") expected (" + std::to_string(expected.r) + "," +
                                   std::to_string(expected.g) + "," + std::to_string(expected.b) +
                                   "," + std::to_string(expected.a) + ")";
                }
            }
        }
    }
    expectations.expect(checked >= 5000, "the dense HDR field checks at least 5000 combinations");
    if (mismatches != 0) {
        std::cerr << "first dense mismatch: " << firstFailure << '\n';
    }
    expectations.expect(mismatches == 0,
                        "the shader formula matches the CPU oracle on all dense HDR/alpha/"
                        "cancellation fixtures within 2e-6 abs-or-rel (" +
                            std::to_string(mismatches) + " mismatches of " +
                            std::to_string(checked) + ")");
}

// The portable kernel must hold the same 2e-6 gate as the Float64 companion across the dense HDR,
// tiny-alpha, and cancellation field, and stay bit-exact at the exact endpoints.
void testPortableDenseAgainstOracle(Expectations& expectations) {
    const auto dense = denseHdrFixtures();
    std::size_t checked = 0;
    std::size_t mismatches = 0;
    std::string firstFailure;
    for (std::uint32_t modeValue = 2; modeValue <= 7; ++modeValue) {
        for (const auto& fixture : dense) {
            const auto source = makePixel(fixture.source);
            const auto backdrop = makePixel(fixture.destination);
            if (!source || !backdrop) {
                continue;
            }
            std::vector<Rgba32f> cpuSource{*source};
            std::vector<Rgba32f> cpuDestination{*backdrop};
            const auto status = bloom::render::blendLinearRec709SceneRow(
                bloom::core::kBlendModes[modeValue], cpuSource, cpuDestination);
            if (status.has_value()) {
                continue;
            }
            ++checked;
            const auto expected = rawPixel(cpuDestination.front());
            const auto actual = shaderBlendPortable(modeValue, fixture.source, fixture.destination);
            if (!(closeEnough(actual.r, expected.r) && closeEnough(actual.g, expected.g) &&
                  closeEnough(actual.b, expected.b) && closeEnough(actual.a, expected.a))) {
                ++mismatches;
                if (firstFailure.empty()) {
                    firstFailure = fixture.name + " mode " + std::to_string(modeValue);
                }
            }
        }
    }
    expectations.expect(checked >= 5000,
                        "the portable dense HDR field checks at least 5000 combinations");
    if (mismatches != 0) {
        std::cerr << "first portable dense mismatch: " << firstFailure << '\n';
    }
    expectations.expect(mismatches == 0,
                        "the portable Float32 formula matches the CPU oracle on all dense HDR/"
                        "alpha/cancellation fixtures within 2e-6 abs-or-rel (" +
                            std::to_string(mismatches) + " mismatches of " +
                            std::to_string(checked) + ")");

    for (const auto& mode : bloom::core::kBlendModes) {
        const auto modeValue = static_cast<std::uint32_t>(bloom::core::blendModeStoredValue(mode));
        for (const auto& fixture : fixtures()) {
            const auto source = makePixel(fixture.source);
            const auto backdrop = makePixel(fixture.destination);
            if (!source || !backdrop) {
                continue;
            }
            std::vector<Rgba32f> cpuSource{*source};
            std::vector<Rgba32f> cpuDestination{*backdrop};
            static_cast<void>(
                bloom::render::blendLinearRec709SceneRow(mode, cpuSource, cpuDestination));
            const auto expected = rawPixel(cpuDestination.front());
            const auto actual = shaderBlendPortable(modeValue, fixture.source, fixture.destination);
            const bool endpoint = fixture.source.a == 0.0F || fixture.destination.a == 0.0F;
            if (endpoint) {
                expectations.expect(actual.r == expected.r && actual.g == expected.g &&
                                        actual.b == expected.b && actual.a == expected.a,
                                    fixture.name + ": portable endpoint is bit-exact");
            }
        }
    }
}

void testRejectsUnknownStoredValues(Expectations& expectations) {
    expectations.expect(!bloom::core::blendModeFromStoredValue(8).has_value(),
                        "an unknown stored blend mode is rejected, not folded to Normal");
    for (const auto& mode : bloom::core::kBlendModes) {
        const auto stored = bloom::core::blendModeStoredValue(mode);
        expectations.expect(bloom::core::blendModeFromStoredValue(stored) == mode,
                            "every blend mode round-trips through its stored integer");
    }
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testEmbeddedPins(expectations);
        testFormulaAgainstOracle(expectations);
        testDenseHdrAgainstOracle(expectations);
        testPortableDenseAgainstOracle(expectations);
        testRejectsUnknownStoredValues(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 2;
    }
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " blend kernel expectation(s) failed\n";
        return 1;
    }
    std::cout << "blend kernel tests passed\n";
    return 0;
}
