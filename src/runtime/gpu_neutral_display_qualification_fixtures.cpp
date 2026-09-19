// Private deterministic fixtures for the Bloom Neutral v1 display GPU qualification. This
// translation unit has no GPU or OCIO dependency: it only builds validated premultiplied RGBA32F
// source pixels and hashes their canonical serialization with bloom::core's SHA-256.

#include <bloom/runtime/gpu_neutral_display_qualification.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {
namespace {

using render::Rgba32f;

// Returns nullopt if the authored value is not a valid premultiplied pixel, so a malformed fixture
// fails qualification instead of being silently replaced with transparent black.
[[nodiscard]] std::optional<Rgba32f> pixel(const float red, const float green, const float blue,
                                           const float alpha) {
    const auto result = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!result) {
        return std::nullopt;
    }
    return *result.value();
}

// A deterministic low-discrepancy-ish generator so the large fixtures are reproducible across runs
// and machines without a library RNG.
class Rng final {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
    [[nodiscard]] std::uint32_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return static_cast<std::uint32_t>(state_);
    }
    [[nodiscard]] float unit() noexcept {
        return static_cast<float>(next() % 1000000U) / 1000000.0F;
    }

  private:
    std::uint64_t state_;
};

[[nodiscard]] std::optional<std::vector<Rgba32f>> randomPixels(const std::size_t count,
                                                               const std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Rgba32f> pixels;
    pixels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const float alpha = rng.unit();
        const float red = (rng.unit() * 2.0F - 0.5F) * alpha;
        const float green = (rng.unit() * 2.0F - 0.5F) * alpha;
        const float blue = (rng.unit() * 2.0F - 0.5F) * alpha;
        const auto value = pixel(red, green, blue, alpha);
        if (!value.has_value()) {
            return std::nullopt;
        }
        pixels.push_back(*value);
    }
    return pixels;
}

// Alpha endpoints and every one of the 255 quantization transitions, each sampled at the boundary
// and its two adjacent binary32 values.
[[nodiscard]] std::optional<std::vector<Rgba32f>> alphaBoundaryPixels() {
    std::vector<Rgba32f> pixels;
    const auto append = [&pixels](const float red, const float green, const float blue,
                                  const float alpha) -> bool {
        const auto value = pixel(red, green, blue, alpha);
        if (!value.has_value()) {
            return false;
        }
        pixels.push_back(*value);
        return true;
    };
    if (!append(0.0F, 0.0F, 0.0F, 0.0F) || !append(1.0F, 1.0F, 1.0F, 1.0F)) {
        return std::nullopt;
    }
    for (int byteValue = 1; byteValue <= 255; ++byteValue) {
        const float boundary = (static_cast<float>(byteValue) - 0.5F) / 255.0F;
        const float below = std::nextafter(boundary, 0.0F);
        const float above = std::nextafter(boundary, 1.0F);
        for (const float alpha : {below, boundary, above}) {
            if (alpha < 0.0F || alpha > 1.0F) {
                continue;
            }
            if (!append(0.25F * alpha, 0.5F * alpha, 0.75F * alpha, alpha)) {
                return std::nullopt;
            }
        }
    }
    return pixels;
}

// Signed/HDR premultiplied RGB, canonical zero alpha, exact half alpha, and tiny normal values.
[[nodiscard]] std::optional<std::vector<Rgba32f>> specialPixels() {
    const float tinyNormal = std::numeric_limits<float>::min();
    const std::array<std::array<float, 4>, 9> authored{{
        {{0.5F, 0.25F, 0.1F, 1.0F}},                        // opaque
        {{0.15F, 0.05F, 0.025F, 0.5F}},                     // translucent
        {{0.7F, 0.7F, 0.7F, 0.0F}},                         // zero alpha canonicalizes RGB to +0
        {{0.0F, 0.0F, 0.0F, -0.0F}},                        // negative zero alpha
        {{-0.1F, -0.05F, -0.02F, 1.0F}},                    // negative
        {{4.0F, 2.0F, 1.0F, 1.0F}},                         // HDR
        {{-2.5F, 8.0F, 0.25F, 0.5F}},                       // signed + HDR
        {{tinyNormal, tinyNormal, tinyNormal, tinyNormal}}, // tiny normal
        {{0.5F, 0.5F, 0.5F, 0.5F}},                         // exact half alpha (rounds up)
    }};
    std::vector<Rgba32f> pixels;
    pixels.reserve(authored.size());
    for (const auto& value : authored) {
        const auto built = pixel(value[0], value[1], value[2], value[3]);
        if (!built.has_value()) {
            return std::nullopt;
        }
        pixels.push_back(*built);
    }
    return pixels;
}

} // namespace

std::optional<std::vector<GpuNeutralDisplayFixture>> makeGpuNeutralDisplayParityFixtures() {
    std::vector<GpuNeutralDisplayFixture> fixtures;
    const auto single = pixel(0.25F, 0.5F, 0.75F, 1.0F);
    if (!single.has_value()) {
        return std::nullopt;
    }
    fixtures.push_back({"single-pixel", 1, 1, {*single}});

    const auto odd = randomPixels(257, 11);
    if (!odd.has_value()) {
        return std::nullopt;
    }
    fixtures.push_back({"odd-257", 257, 1, *odd});

    const auto special = specialPixels();
    if (!special.has_value()) {
        return std::nullopt;
    }
    fixtures.push_back({"special-values", 9, 1, *special});

    const auto boundary = alphaBoundaryPixels();
    if (!boundary.has_value()) {
        return std::nullopt;
    }
    const auto width = static_cast<std::uint32_t>(boundary->size());
    fixtures.push_back({"alpha-boundary", width, 1, *boundary});

    const auto hd = randomPixels(static_cast<std::size_t>(1280U) * 720U, 2222);
    if (!hd.has_value()) {
        return std::nullopt;
    }
    fixtures.push_back({"1280x720", 1280, 720, *hd});

    const auto fhd = randomPixels(static_cast<std::size_t>(1920U) * 1080U, 3333);
    if (!fhd.has_value()) {
        return std::nullopt;
    }
    fixtures.push_back({"1920x1080", 1920, 1080, *fhd});
    return fixtures;
}

std::optional<std::vector<Rgba32f>>
makeGpuNeutralDisplayMeasuredPixels(const std::uint32_t width, const std::uint32_t height) {
    const std::size_t count = static_cast<std::size_t>(width) * height;
    // Seeded per size so each measured size gets a distinct but deterministic source.
    return randomPixels(count, (static_cast<std::uint64_t>(width) << 32U) ^ height);
}

core::Sha256Digest
gpuNeutralDisplayFixtureDigest(const std::vector<GpuNeutralDisplayFixture>& fixtures) noexcept {
    core::Sha256Hasher hasher;
    const auto updateU32 = [&hasher](const std::uint32_t value) {
        const std::byte bytes[4] = {static_cast<std::byte>(value & 0xFFU),
                                    static_cast<std::byte>((value >> 8U) & 0xFFU),
                                    static_cast<std::byte>((value >> 16U) & 0xFFU),
                                    static_cast<std::byte>((value >> 24U) & 0xFFU)};
        (void)hasher.update(bytes);
    };
    for (const GpuNeutralDisplayFixture& fixture : fixtures) {
        updateU32(static_cast<std::uint32_t>(fixture.name.size()));
        for (const char character : fixture.name) {
            const std::byte byte = static_cast<std::byte>(static_cast<unsigned char>(character));
            (void)hasher.update(std::span<const std::byte>(&byte, 1));
        }
        updateU32(fixture.width);
        updateU32(fixture.height);
        updateU32(static_cast<std::uint32_t>(fixture.pixels.size()));
        for (const Rgba32f& value : fixture.pixels) {
            for (const float component : value.components()) {
                updateU32(std::bit_cast<std::uint32_t>(component));
            }
        }
    }
    return hasher.finalize();
}

} // namespace bloom::runtime::detail
