#pragma once

#include <bloom/color/ocio_cpu_input_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::media {
inline constexpr std::uint32_t kMaxImageDimension = 16384;
inline constexpr std::uint64_t kMaxImagePixels = 16777216;
inline constexpr std::size_t kMaxImageFileBytes = 67108864;
inline constexpr std::size_t kMaxImageStorageBytes = 268435456;
inline constexpr std::size_t kMaxSequenceEntries = 100000;
using CancelImageWork = std::function<bool()>;
using ImageProgress = std::function<void(std::uint64_t, std::uint64_t)>;
enum class ImageColorSpace : std::uint8_t { Auto, Srgb, Linear, Raw };
enum class ImageAlphaAssociation : std::uint8_t { Straight, Premultiplied };
struct ImageInterpretation {
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Straight;
    friend bool operator==(const ImageInterpretation&, const ImageInterpretation&) = default;
};
struct ImageProbe {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    core::Sha256Digest contentDigest;
};
template <typename T> struct ImageResult {
    std::optional<T> value;
    std::string diagnostic;
    bool cancelled = false;
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }
};
struct SequenceMember {
    std::int64_t frame = 0;
    std::filesystem::path path;
    core::Sha256Digest contentDigest;
};
struct SequenceManifest {
    std::string pattern;
    std::uint32_t padding = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<SequenceMember> members;
    std::vector<std::int64_t> gaps;
    std::vector<std::string> diagnostics;
};
// Worker-only locator resolution: prefer the project-relative file; use the absolute relink
// hint when that file is unavailable (including an unsaved project's first Save As).
[[nodiscard]] std::filesystem::path resolveImagePath(std::string_view relativePath,
                                                     std::string_view relinkHint,
                                                     const std::filesystem::path& projectDirectory);
[[nodiscard]] ImageResult<ImageProbe> probeImage(const std::filesystem::path& path,
                                                 const CancelImageWork& cancel = {});
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, ImageInterpretation interpretation = {},
            std::shared_ptr<const color::CpuInputProcessor> processor = {},
            const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
            std::size_t pixelBudget = kMaxImageStorageBytes,
            std::optional<core::Sha256Digest> expectedDigest = {});
[[nodiscard]] ImageResult<SequenceManifest> scanSequence(const std::filesystem::path& path,
                                                         const CancelImageWork& cancel = {},
                                                         const ImageProgress& progress = {});
} // namespace bloom::media
