#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::media {
// Default decode request budget. This is a benign default for callers that do not pass their own
// budget. Decoded storage and the decoder's peak working set are admitted against the explicit
// caller-supplied pixelBudget; there is no separate absolute byte, pixel, or dimension ceiling.
// Geometry is bounded only by the codec's own representable window and by overflow/index checks.
inline constexpr std::size_t kMaxImageStorageBytes = 268435456;
// Bounds retained member metadata, not unrelated files in the directory or frame-number span.
inline constexpr std::size_t kMaxSequenceEntries = 100000;
using CancelImageWork = std::function<bool()>;
using ImageProgress = std::function<void(std::uint64_t, std::uint64_t)>;
template <typename T> struct ImageResult;
enum class ImageColorSpace : std::uint8_t { Auto, Srgb, Linear, Raw };
enum class ImageAlphaAssociation : std::uint8_t { Auto, Straight, Premultiplied };
struct ImageInterpretation {
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    std::string inputColorSpaceId;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Auto;
    friend bool operator==(const ImageInterpretation&, const ImageInterpretation&) = default;
};

enum class ImageFormat : std::uint8_t { Png, Jpeg, Exr, Tiff };

enum class ImageDiagnosticCode : std::uint8_t {
    None,
    FileUnavailable,
    FileTooLarge,
    FileReadFailed,
    InvalidMagic,
    InvalidHeader,
    UnsupportedVersion,
    UnsupportedDeepImage,
    UnsupportedMultipart,
    UnsupportedTiledLevels,
    UnsupportedChannels,
    UnsupportedChannelType,
    InvalidDataWindow,
    DimensionsExceeded,
    PixelBudgetExceeded,
    Truncated,
    DecodeFailed,
    AllocationFailure,
    DigestMismatch,
    ProviderMissing,
    ProviderFailed,
    ColorSpaceUnavailable,
    Cancelled,
};

struct ImageProbe {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    core::Sha256Digest contentDigest;
    ImageFormat format = ImageFormat::Png;
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Straight;
    // The tag is intentionally a Bloom vocabulary value rather than an OpenEXR type. For a
    // chromaticities-bearing EXR it records the recognized interpretation; when metadata is absent,
    // interpretationAssumption explains the convention used by the bounded reader.
    std::string colorSpaceTag;
    std::string interpretationAssumption;
};

// A session-owned, immutable source revision. Its digest, header and every decoder read refer to
// these exact captured bytes, never to a reopened pathname or a timestamp. Capture is worker-only;
// callers bound both transient capture and retention through their current host allowances.
struct ImageSourceCaptureStatistics final {
    std::uint64_t ioNanoseconds = 0;
    std::uint64_t hashNanoseconds = 0;
    std::uint64_t probeNanoseconds = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesHashed = 0;
};
struct ImageDecodeStatistics final {
    // Codec, image construction and any requested input colour processing. Source capture I/O,
    // SHA256, metadata probe, proxy resampling and cache lookup are excluded.
    std::uint64_t decodeNanoseconds = 0;
};
class ImageSourceRevision final {
  public:
    [[nodiscard]] const ImageProbe& probe() const noexcept { return probe_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const ImageSourceCaptureStatistics& captureStatistics() const noexcept {
        return captureStatistics_;
    }
    [[nodiscard]] std::size_t retainedBytes() const noexcept {
        return sizeof(ImageSourceRevision) + bytes_.capacity() + probe_.colorSpaceTag.capacity() +
               probe_.interpretationAssumption.capacity();
    }

  private:
    ImageSourceRevision(std::vector<std::byte> bytes, ImageProbe probe,
                        ImageSourceCaptureStatistics statistics)
        : bytes_(std::move(bytes)), probe_(std::move(probe)), captureStatistics_(statistics) {}
    friend ImageResult<std::shared_ptr<const ImageSourceRevision>>
    captureImageSourceRevision(const std::filesystem::path&, std::size_t, const CancelImageWork&,
                               std::optional<core::Sha256Digest>);
    std::vector<std::byte> bytes_;
    ImageProbe probe_;
    ImageSourceCaptureStatistics captureStatistics_;
};

[[nodiscard]] ImageResult<std::shared_ptr<const ImageSourceRevision>>
captureImageSourceRevision(const std::filesystem::path& path, std::size_t hostByteBudget,
                           const CancelImageWork& cancel = {},
                           std::optional<core::Sha256Digest> expectedDigest = {});

// The revision remains live throughout decode. Its encoded storage is subtracted from the total
// host allowance before admitting codec staging and output. No path is reopened and no hash
// repeats.
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const ImageSourceRevision& revision, const ImageInterpretation& interpretation = {},
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor = {},
            const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
            std::size_t hostByteBudget = kMaxImageStorageBytes,
            ImageDecodeStatistics* statistics = nullptr);

enum class ImageProviderRole : std::uint8_t { Decode, Encode };

struct ImageProviderDecodeRequest final {
    std::filesystem::path path;
    ImageInterpretation interpretation;
    std::size_t pixelBudget = kMaxImageStorageBytes;
    std::optional<core::Sha256Digest> expectedDigest;
};

struct ImageProviderDecodeResponse final {
    ImageProbe probe;
    std::shared_ptr<const render::Rgba32fImage> image;
};

struct ImageProviderEncodeRequest final {
    std::filesystem::path destination;
    std::shared_ptr<const render::Rgba32fImage> image;
};

struct ImageProviderEncodeResponse final {
    bool written = false;
};

using ImageProviderDecode =
    std::function<ImageResult<ImageProviderDecodeResponse>(const ImageProviderDecodeRequest&)>;
using ImageProviderEncode =
    std::function<ImageResult<ImageProviderEncodeResponse>(const ImageProviderEncodeRequest&)>;

// A provider is supplied explicitly by the worker composition root. The in-process image library
// never discovers a provider globally: an absent callback is the typed ProviderMissing outcome
// required for TIFF until MEDIA-3 wires the worker.
struct ImageProvider final {
    ImageProviderDecode decode;
    ImageProviderEncode encode;
};

template <typename T> struct ImageResult {
    std::optional<T> value;
    std::string diagnostic;
    bool cancelled = false;
    ImageDiagnosticCode code = ImageDiagnosticCode::None;
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
    // Missing intervals are implicit between adjacent sorted members. `gaps` remains for
    // compatibility with older manifests that enumerated every missing frame.
    [[nodiscard]] bool hasGaps() const noexcept {
        if (!gaps.empty())
            return true;
        for (std::size_t index = 1; index < members.size(); ++index)
            if (members[index].frame > members[index - 1].frame &&
                static_cast<std::uint64_t>(members[index].frame) -
                        static_cast<std::uint64_t>(members[index - 1].frame) >
                    1)
                return true;
        return false;
    }
};
// Worker-only locator resolution: prefer the project-relative file; use the absolute relink
// hint when that file is unavailable (including an unsaved project's first Save As).
[[nodiscard]] std::filesystem::path resolveImagePath(std::string_view relativePath,
                                                     std::string_view relinkHint,
                                                     const std::filesystem::path& projectDirectory);
[[nodiscard]] ImageResult<ImageProbe> probeImage(const std::filesystem::path& path,
                                                 const CancelImageWork& cancel = {},
                                                 const ImageProvider* provider = nullptr);
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, const ImageInterpretation& interpretation = {},
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor = {},
            const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
            std::size_t pixelBudget = kMaxImageStorageBytes,
            std::optional<core::Sha256Digest> expectedDigest = {},
            const ImageProvider* provider = nullptr);
[[nodiscard]] ImageResult<ImageProviderEncodeResponse>
encodeImage(const ImageProviderEncodeRequest& request, const ImageProvider* provider = nullptr);
[[nodiscard]] ImageResult<SequenceManifest> scanSequence(const std::filesystem::path& path,
                                                         const CancelImageWork& cancel = {},
                                                         const ImageProgress& progress = {});
} // namespace bloom::media
