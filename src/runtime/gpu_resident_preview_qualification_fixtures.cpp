// Deterministic fixtures for the resident-preview qualification. No GPU, Qt, or filesystem access:
// every pixel is generated from the coordinates so CPU oracle and GPU input are byte-identical and
// the qualification is reproducible. Nonzero subnormal values are deliberately excluded from the
// parity fixtures because the resident display shader rejects a frame containing one whole-frame.

#include <bloom/runtime/gpu_resident_preview_qualification.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bloom::runtime {
namespace detail {
namespace {

using render::Rgba32f;

[[nodiscard]] std::optional<Rgba32f> makePixel(const float red, const float green, const float blue,
                                               const float alpha) {
    const auto value = Rgba32f::fromPremultiplied(red * alpha, green * alpha, blue * alpha, alpha);
    return value ? std::optional(*value.value()) : std::nullopt;
}

} // namespace

core::Sha256Digest residentPreviewHashBytes(const std::span<const std::byte> bytes) noexcept {
    const auto digest = core::Sha256Hasher::hash(bytes);
    return digest ? *digest : core::Sha256Digest{};
}

std::optional<std::vector<Rgba32f>> makeResidentPreviewNonUniformPixels(const std::uint32_t width,
                                                                        const std::uint32_t height,
                                                                        const bool pattern_b) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx =
                width > 1U ? static_cast<float>(x) / static_cast<float>(width - 1U) : 0.0F;
            const float fy =
                height > 1U ? static_cast<float>(y) / static_cast<float>(height - 1U) : 0.0F;
            float alpha = 1.0F;
            float red = 0.0F;
            float green = 0.0F;
            float blue = 0.0F;
            if (!pattern_b) {
                alpha = 0.25F + 0.75F * (static_cast<float>((x * 3U + y * 5U) % 8U) / 7.0F);
                red = 2.0F * fx - 0.5F;
                green = 1.5F * fy;
                blue = 3.0F * fx * fy;
            } else {
                alpha = ((x / 7U + y / 5U) % 3U == 0U) ? 0.5F : 1.0F;
                const bool even = ((x + y) & 1U) == 0U;
                red = even ? 1.2F : 0.05F;
                green = even ? 0.3F : 0.8F;
                blue = even ? -0.1F : 1.5F;
            }
            if ((x + y) % 97U == 0U) {
                alpha = 0.0F;
                red = 0.0F;
                green = 0.0F;
                blue = 0.0F;
            }
            const auto pixel = makePixel(red, green, blue, alpha);
            if (!pixel) {
                return std::nullopt;
            }
            pixels.push_back(*pixel);
        }
    }
    return pixels;
}

std::optional<std::vector<Rgba32f>> makeResidentPreviewMeasuredPixels(const std::uint32_t width,
                                                                      const std::uint32_t height) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x % 37U) / 36.0F;
            const float fy = static_cast<float>((y * 7U + x * 3U) % 53U) / 52.0F;
            const float alpha = 0.2F + 0.8F * fy;
            const float red = 1.5F * fx - 0.25F;
            const float green = 0.75F * fx + 0.5F * fy;
            const float blue = fx * fy + 0.1F;
            const auto pixel = makePixel(red, green, blue, alpha);
            if (!pixel) {
                return std::nullopt;
            }
            pixels.push_back(*pixel);
        }
    }
    return pixels;
}

std::optional<std::vector<Rgba32f>> makeResidentPreviewCheckerPixels(const std::uint32_t width,
                                                                     const std::uint32_t height) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool even = ((x + y) & 1U) == 0U;
            const float alpha = ((x / 3U + y / 2U) % 4U == 0U) ? 0.5F : 1.0F;
            const float red = even ? 1.1F : -0.2F;
            const float green = even ? 0.15F : 0.9F;
            const float blue = even ? -0.05F : 1.4F;
            const auto pixel = makePixel(red, green, blue, alpha);
            if (!pixel) {
                return std::nullopt;
            }
            pixels.push_back(*pixel);
        }
    }
    return pixels;
}

std::optional<std::vector<Rgba32f>> makeResidentPreviewSemanticPixels(const std::uint32_t width,
                                                                      const std::uint32_t height) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float alpha = static_cast<float>((x + 2U * y) % 6U) / 5.0F;
            const float red = alpha + 0.1F;
            const float green = 1.0F - alpha;
            const float blue = 0.5F * alpha;
            const auto pixel = makePixel(red, green, blue, alpha);
            if (!pixel) {
                return std::nullopt;
            }
            pixels.push_back(*pixel);
        }
    }
    return pixels;
}

} // namespace detail
} // namespace bloom::runtime
