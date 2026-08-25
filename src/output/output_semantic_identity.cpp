#include "output_semantic_identity.hpp"

#include "output_analysis_numeric.hpp"
#include "output_semantic_identity_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace color = bloom::color;
namespace core = bloom::core;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

constexpr char kOutputSemanticIdentityDomain[] = "BloomOutputSemanticIdentity";
constexpr std::string_view kPngColorId = "srgb_rec709_display";
constexpr std::string_view kExrColorId = "lin_rec709_scene";
constexpr std::uint8_t kPngPayloadKind = 1;
constexpr std::uint8_t kExrPayloadKind = 2;
constexpr std::uint8_t kPackedRgba8 = 1;
constexpr std::uint8_t kStraightAlpha = 1;
constexpr std::uint8_t kPngSrgbRenderingIntent = 0;
constexpr std::uint8_t kZipCompression = 1;
constexpr std::uint8_t kIncreasingY = 1;
constexpr std::uint64_t kPngMaximumPayloadBytes = 268'435'456U;
constexpr std::uint64_t kExrMaximumPayloadBytes = 1'073'741'824U;
constexpr std::size_t kPayloadChunkPixels = 1'024;
constexpr std::size_t kExrPayloadChunkBytes = kPayloadChunkPixels * 4U * sizeof(std::uint32_t);

static_assert(sizeof(kOutputSemanticIdentityDomain) == 28);
static_assert(kPngColorId.size() == 19);
static_assert(kExrColorId.size() == 16);
static_assert(kExrPayloadChunkBytes == 16'384);
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(std::uint32_t));

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr std::array<std::byte, sizeof(Integer)>
bigEndianBytes(const Integer value) noexcept {
    std::array<std::byte, sizeof(Integer)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes[index] = static_cast<std::byte>(value >> shift);
    }
    return bytes;
}

template <std::signed_integral Integer>
[[nodiscard]] constexpr std::array<std::byte, sizeof(Integer)>
bigEndianSignedBytes(const Integer value) noexcept {
    return bigEndianBytes(std::bit_cast<std::make_unsigned_t<Integer>>(value));
}

class DigestStream final {
  public:
    [[nodiscard]] bool exact(const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - byteCount_ ||
            !hasher_.update(bytes)) {
            return false;
        }
        byteCount_ += bytes.size();
        return true;
    }

    template <std::size_t Size> [[nodiscard]] bool literal(const char (&text)[Size]) noexcept {
        return exact(std::as_bytes(std::span{text}));
    }

    template <std::integral Integer> [[nodiscard]] bool integer(const Integer value) noexcept {
        const auto encoded = [&] {
            if constexpr (std::is_signed_v<Integer>) {
                return bigEndianSignedBytes(value);
            } else {
                return bigEndianBytes(value);
            }
        }();
        return exact(encoded);
    }

    [[nodiscard]] bool text(const std::string_view value) noexcept {
        return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
               integer(static_cast<std::uint32_t>(value.size())) &&
               exact(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] bool bytes(const std::span<const std::byte> value) noexcept {
        return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
               integer(static_cast<std::uint32_t>(value.size())) && exact(value);
    }

    [[nodiscard]] core::Sha256Digest finish() const noexcept { return hasher_.finalize(); }
    [[nodiscard]] std::uint64_t byteCount() const noexcept { return byteCount_; }

  private:
    core::Sha256Hasher hasher_;
    std::uint64_t byteCount_ = 0;
};

[[nodiscard]] constexpr bool addChecked(std::uint64_t& value,
                                        const std::uint64_t increment) noexcept {
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    value += increment;
    return true;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
multiplyChecked(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] bool addTextSize(std::uint64_t& value, const std::string_view text) noexcept {
    return text.size() <= std::numeric_limits<std::uint32_t>::max() &&
           addChecked(value, sizeof(std::uint32_t)) && addChecked(value, text.size());
}

[[nodiscard]] bool addBytesSize(std::uint64_t& value, const std::size_t bytes) noexcept {
    return bytes <= std::numeric_limits<std::uint32_t>::max() &&
           addChecked(value, sizeof(std::uint32_t)) && addChecked(value, bytes);
}

void reportProgress(const output::OutputSemanticIdentityProgressCallbackV1& callback,
                    const output::OutputSemanticIdentityProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring cannot change a portable identity or its publication outcome.
        return;
    }
}

struct CommonPreflight final {
    core::Sha256Digest analysisDigest;
    output::OutputPresetIdentityV1 preset;
    std::span<const std::byte> processIdentityBytes;
    std::uint64_t pixelCount;
    std::uint64_t preimageBytes;
};

struct PngPreflight final {
    CommonPreflight common;
    std::span<const std::byte> displayIdentityBytes;
    std::uint64_t payloadBytes;
};

struct ExrPreflight final {
    CommonPreflight common;
    std::uint64_t componentCount;
};

template <typename Value> struct PreflightResult final {
    std::optional<Value> value;
    output::OutputSemanticIdentityErrorCodeV1 error =
        output::OutputSemanticIdentityErrorCodeV1::InternalInvariant;
};

template <typename Value>
[[nodiscard]] PreflightResult<Value>
preflightFailure(const output::OutputSemanticIdentityErrorCodeV1 error) noexcept {
    return {.value = std::nullopt,
            .error = error == output::OutputSemanticIdentityErrorCodeV1::None
                         ? output::OutputSemanticIdentityErrorCodeV1::InternalInvariant
                         : error};
}

struct ProcessDescriptorPreflight final {
    const render::Rgba32fImageDescriptor* descriptor;
    std::span<const std::byte> canonicalIdentityBytes;
    std::uint64_t pixelCount;
};

[[nodiscard]] PreflightResult<ProcessDescriptorPreflight> preflightProcessIdentity(
    const std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>& identity) noexcept {
    using Error = output::OutputSemanticIdentityErrorCodeV1;
    if (identity == nullptr) {
        return preflightFailure<ProcessDescriptorPreflight>(Error::MissingProcessIdentity);
    }
    const auto canonicalBytes = identity->canonicalBytes();
    if (canonicalBytes.size() != output::kCompositionProcessFrameSemanticIdentityV1Bytes &&
        canonicalBytes.size() != output::kProxyProcessFrameSemanticIdentityV1Bytes) {
        return preflightFailure<ProcessDescriptorPreflight>(Error::InvalidProcessIdentity);
    }
    const auto& frame = identity->processFrame();
    if (frame == nullptr || !frame->processImage().isValid()) {
        return preflightFailure<ProcessDescriptorPreflight>(Error::InvalidProcessIdentity);
    }
    const auto* descriptor = frame->processImage().descriptor();
    if (descriptor == nullptr) {
        return preflightFailure<ProcessDescriptorPreflight>(Error::InvalidProcessIdentity);
    }
    const auto pixelCount = descriptor->layout().pixelCount;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (pixelCount > std::numeric_limits<std::uint64_t>::max()) {
            return preflightFailure<ProcessDescriptorPreflight>(Error::ResourceLimitExceeded);
        }
    }
    const auto count = static_cast<std::uint64_t>(pixelCount);
    const auto dataExtent = descriptor->dataWindow().extent();
    const auto displayExtent = descriptor->displayWindow().extent();
    if (dataExtent.width() > output::kOutputAnalysisMaximumDimensionV1 ||
        dataExtent.height() > output::kOutputAnalysisMaximumDimensionV1 ||
        displayExtent.width() > output::kOutputAnalysisMaximumDimensionV1 ||
        displayExtent.height() > output::kOutputAnalysisMaximumDimensionV1 ||
        count > output::kOutputAnalysisMaximumPixelCountV1 ||
        descriptor->layout().pixelStorageBytes >
            output::kOutputAnalysisMaximumProcessPixelBytesV1) {
        return preflightFailure<ProcessDescriptorPreflight>(Error::ResourceLimitExceeded);
    }
    return {.value = ProcessDescriptorPreflight{descriptor, canonicalBytes, count},
            .error = Error::None};
}

[[nodiscard]] std::optional<std::uint64_t>
commonPreimageSize(const output::OutputPresetIdentityV1& preset,
                   const std::size_t processIdentityBytes,
                   const std::size_t displayIdentityBytes) noexcept {
    std::uint64_t size = sizeof(kOutputSemanticIdentityDomain);
    if (!addChecked(size, sizeof(std::uint16_t) + core::kSha256DigestBytes) ||
        !addBytesSize(size, processIdentityBytes) || !addTextSize(size, preset.serializedId) ||
        !addChecked(size, sizeof(std::uint32_t)) ||
        !addTextSize(size, preset.outputPixelSemanticsProfileId) ||
        !addBytesSize(size, displayIdentityBytes) || !addChecked(size, sizeof(std::uint8_t))) {
        return std::nullopt;
    }
    return size;
}

[[nodiscard]] PreflightResult<PngPreflight>
preflightPng(const output::PngRgba8SrgbOutputSemanticIdentityInputV1& input) noexcept {
    using Error = output::OutputSemanticIdentityErrorCodeV1;
    const auto& verifiedProduct = input.verifiedProduct;
    const auto& boundAnalysis = verifiedProduct.boundAnalysis();
    if (boundAnalysis == nullptr) {
        return preflightFailure<PngPreflight>(Error::MissingBoundAnalysis);
    }
    const auto process = preflightProcessIdentity(boundAnalysis->processIdentity());
    if (!process.value) {
        return preflightFailure<PngPreflight>(process.error);
    }
    const auto& displayIdentity = boundAnalysis->displayProcessorIdentity();
    if (displayIdentity == nullptr) {
        return preflightFailure<PngPreflight>(Error::MissingDisplayIdentity);
    }
    const auto displayView = displayIdentity->borrowedView();
    if (!displayView || displayView->canonicalBytes().empty() ||
        displayView->canonicalBytes().size() > color::kDisplayProcessorIdentityMaximumBytes) {
        return preflightFailure<PngPreflight>(Error::InvalidDisplayIdentity);
    }
    const auto reparsed = color::parseDisplayProcessorIdentityV1(displayView->canonicalBytes());
    if (!reparsed || reparsed.identity() == nullptr ||
        reparsed.identity()->expectedOcioRevision() != displayView->expectedOcioRevision()) {
        return preflightFailure<PngPreflight>(Error::InvalidDisplayIdentity);
    }

    const auto* descriptor = process.value->descriptor;
    const auto dataWindow = descriptor->dataWindow();
    const auto displayWindow = descriptor->displayWindow();
    const auto dimensions = verifiedProduct.dimensions();
    if (dimensions.width() == 0 || dimensions.height() == 0) {
        return preflightFailure<PngPreflight>(Error::InvalidDimensions);
    }
    if (dimensions.width() > output::kOutputAnalysisMaximumDimensionV1 ||
        dimensions.height() > output::kOutputAnalysisMaximumDimensionV1) {
        return preflightFailure<PngPreflight>(Error::ResourceLimitExceeded);
    }
    if (dataWindow.originX() != 0 || dataWindow.originY() != 0 ||
        dataWindow.extent() != dimensions || displayWindow.originX() != 0 ||
        displayWindow.originY() != 0 || displayWindow.extent() != dimensions ||
        descriptor->pixelAspect() != core::PixelAspectRatio::square()) {
        return preflightFailure<PngPreflight>(Error::ProcessDescriptorMismatch);
    }

    const auto expectedPixels = multiplyChecked(dimensions.width(), dimensions.height());
    if (!expectedPixels || *expectedPixels != process.value->pixelCount) {
        return preflightFailure<PngPreflight>(Error::ProcessDescriptorMismatch);
    }
    const auto expectedBytes = multiplyChecked(*expectedPixels, 4U);
    if (!expectedBytes || *expectedBytes > kPngMaximumPayloadBytes) {
        return preflightFailure<PngPreflight>(Error::ResourceLimitExceeded);
    }
    if (verifiedProduct.bytes().size() != *expectedBytes) {
        return preflightFailure<PngPreflight>(Error::PayloadSizeMismatch);
    }

    const auto preset = output::outputPresetIdentityV1(output::OutputPresetV1::PngRgba8SrgbV1);
    if (!preset) {
        return preflightFailure<PngPreflight>(Error::InternalInvariant);
    }
    auto preimageSize = commonPreimageSize(*preset, process.value->canonicalIdentityBytes.size(),
                                           displayView->canonicalBytes().size());
    if (!preimageSize || !addChecked(*preimageSize, 2U * sizeof(std::uint32_t) + 2U) ||
        !addTextSize(*preimageSize, kPngColorId) ||
        !addChecked(*preimageSize, sizeof(std::uint8_t)) ||
        !addTextSize(*preimageSize, output::kPngRgba8SrgbSemanticProfileV1) ||
        !addChecked(*preimageSize, sizeof(std::uint64_t)) ||
        !addChecked(*preimageSize, *expectedBytes)) {
        return preflightFailure<PngPreflight>(Error::HashInputTooLarge);
    }
    constexpr auto maximumHashBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (*preimageSize > maximumHashBytes) {
        return preflightFailure<PngPreflight>(Error::HashInputTooLarge);
    }
    return {
        .value =
            PngPreflight{
                .common = {boundAnalysis->digest(), *preset, process.value->canonicalIdentityBytes,
                           *expectedPixels, *preimageSize},
                .displayIdentityBytes = displayView->canonicalBytes(),
                .payloadBytes = *expectedBytes,
            },
        .error = Error::None};
}

[[nodiscard]] bool matchesWindow(const output::FlatExrInclusiveWindowV1& supplied,
                                 const render::ImageWindow expected) noexcept {
    if (!output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(expected.originX(),
                                                                   expected.extent().width()) ||
        !output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(expected.originY(),
                                                                   expected.extent().height())) {
        return false;
    }
    return supplied.xMin == expected.originX() && supplied.yMin == expected.originY() &&
           supplied.xMax == expected.originX() + expected.extent().width() - 1 &&
           supplied.yMax == expected.originY() + expected.extent().height() - 1;
}

[[nodiscard]] PreflightResult<ExrPreflight> preflightExr(
    const output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1& input) noexcept {
    using Error = output::OutputSemanticIdentityErrorCodeV1;
    const auto& verifiedProduct = input.verifiedProduct;
    const auto& boundAnalysis = verifiedProduct.boundAnalysis();
    if (boundAnalysis == nullptr) {
        return preflightFailure<ExrPreflight>(Error::MissingBoundAnalysis);
    }
    const auto process = preflightProcessIdentity(boundAnalysis->processIdentity());
    if (!process.value) {
        return preflightFailure<ExrPreflight>(process.error);
    }
    const auto& metadata = verifiedProduct.metadata();
    const auto dataPixelCount =
        output::detail::flatExrInclusiveWindowPixelCountV1(metadata.dataWindow);
    const auto displayPixelCount =
        output::detail::flatExrInclusiveWindowPixelCountV1(metadata.displayWindow);
    if (!dataPixelCount || !displayPixelCount) {
        return preflightFailure<ExrPreflight>(Error::InvalidWindow);
    }
    const auto dataWidth = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(metadata.dataWindow.xMax) - metadata.dataWindow.xMin + 1);
    const auto dataHeight = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(metadata.dataWindow.yMax) - metadata.dataWindow.yMin + 1);
    const auto displayWidth = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(metadata.displayWindow.xMax) - metadata.displayWindow.xMin + 1);
    const auto displayHeight = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(metadata.displayWindow.yMax) - metadata.displayWindow.yMin + 1);
    if (dataWidth > output::kOutputAnalysisMaximumDimensionV1 ||
        dataHeight > output::kOutputAnalysisMaximumDimensionV1 ||
        displayWidth > output::kOutputAnalysisMaximumDimensionV1 ||
        displayHeight > output::kOutputAnalysisMaximumDimensionV1 ||
        *dataPixelCount > output::kOutputAnalysisMaximumPixelCountV1) {
        return preflightFailure<ExrPreflight>(Error::ResourceLimitExceeded);
    }
    const auto* descriptor = process.value->descriptor;
    if (*dataPixelCount != process.value->pixelCount ||
        !matchesWindow(metadata.dataWindow, descriptor->dataWindow()) ||
        !matchesWindow(metadata.displayWindow, descriptor->displayWindow())) {
        return preflightFailure<ExrPreflight>(Error::ProcessDescriptorMismatch);
    }
    const auto roundedAspect = output::detail::roundOutputAnalysisPositiveRationalToBinary32V1(
        {.numerator = descriptor->pixelAspect().numerator(),
         .denominator = descriptor->pixelAspect().denominator()});
    if (!roundedAspect || roundedAspect->bits != metadata.pixelAspectRatioBits) {
        return preflightFailure<ExrPreflight>(Error::InvalidPixelAspectRatio);
    }

    const auto componentCount = multiplyChecked(*dataPixelCount, 4U);
    const auto payloadBytes =
        componentCount ? multiplyChecked(*componentCount, sizeof(std::uint32_t)) : std::nullopt;
    if (!componentCount || !payloadBytes || *payloadBytes > kExrMaximumPayloadBytes) {
        return preflightFailure<ExrPreflight>(Error::ResourceLimitExceeded);
    }
    if (verifiedProduct.componentBits().size() != *componentCount) {
        return preflightFailure<ExrPreflight>(Error::PayloadSizeMismatch);
    }

    const auto preset =
        output::outputPresetIdentityV1(output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1);
    if (!preset) {
        return preflightFailure<ExrPreflight>(Error::InternalInvariant);
    }
    auto preimageSize =
        commonPreimageSize(*preset, process.value->canonicalIdentityBytes.size(), 0);
    constexpr std::uint64_t fixedMetadataBytes = 8U * sizeof(std::int32_t) + sizeof(std::uint32_t) +
                                                 2U + 8U * sizeof(std::uint32_t) +
                                                 sizeof(std::uint64_t);
    if (!preimageSize || !addChecked(*preimageSize, fixedMetadataBytes) ||
        !addTextSize(*preimageSize, kExrColorId) ||
        !addTextSize(*preimageSize, output::kFlatExrRgba32fSemanticProfileV1) ||
        !addChecked(*preimageSize, *payloadBytes)) {
        return preflightFailure<ExrPreflight>(Error::HashInputTooLarge);
    }
    constexpr auto maximumHashBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (*preimageSize > maximumHashBytes) {
        return preflightFailure<ExrPreflight>(Error::HashInputTooLarge);
    }
    return {
        .value =
            ExrPreflight{
                .common = {boundAnalysis->digest(), *preset, process.value->canonicalIdentityBytes,
                           *dataPixelCount, *preimageSize},
                .componentCount = *componentCount,
            },
        .error = Error::None};
}

[[nodiscard]] bool streamCommon(DigestStream& stream, const CommonPreflight& preflight,
                                const std::span<const std::byte> displayIdentityBytes,
                                const std::uint8_t payloadKind) noexcept {
    return stream.literal(kOutputSemanticIdentityDomain) &&
           stream.integer(output::kOutputSemanticIdentitySerializationVersionV1) &&
           stream.exact(std::as_bytes(preflight.analysisDigest.bytes())) &&
           stream.bytes(preflight.processIdentityBytes) &&
           stream.text(preflight.preset.serializedId) && stream.integer(preflight.preset.version) &&
           stream.text(preflight.preset.outputPixelSemanticsProfileId) &&
           stream.bytes(displayIdentityBytes) && stream.integer(payloadKind);
}

struct HashResult final {
    std::optional<core::Sha256Digest> digest;
    bool cancelled = false;
    output::OutputSemanticIdentityErrorCodeV1 error =
        output::OutputSemanticIdentityErrorCodeV1::InternalInvariant;
};

[[nodiscard]] HashResult
hashFailure(const output::OutputSemanticIdentityErrorCodeV1 error) noexcept {
    return {.digest = std::nullopt,
            .cancelled = false,
            .error = error == output::OutputSemanticIdentityErrorCodeV1::None
                         ? output::OutputSemanticIdentityErrorCodeV1::InternalInvariant
                         : error};
}

[[nodiscard]] HashResult
hashPng(const output::PngRgba8SrgbOutputSemanticIdentityInputV1& input,
        const PngPreflight& preflight, const runtime::CancellationToken& cancellation,
        const output::OutputSemanticIdentityProgressCallbackV1& progress) noexcept {
    DigestStream stream;
    const auto dimensions = input.verifiedProduct.dimensions();
    if (!streamCommon(stream, preflight.common, preflight.displayIdentityBytes, kPngPayloadKind) ||
        !stream.integer(dimensions.width()) || !stream.integer(dimensions.height()) ||
        !stream.integer(kPackedRgba8) || !stream.integer(kStraightAlpha) ||
        !stream.text(kPngColorId) || !stream.integer(kPngSrgbRenderingIntent) ||
        !stream.text(output::kPngRgba8SrgbSemanticProfileV1) ||
        !stream.integer(preflight.payloadBytes)) {
        return hashFailure(output::OutputSemanticIdentityErrorCodeV1::HashInputTooLarge);
    }

    reportProgress(progress, {output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload,
                              0, preflight.common.pixelCount});
    const auto payload = input.verifiedProduct.bytes();
    const auto pixelCount = payload.size() / 4U;
    for (std::size_t pixelOffset = 0; pixelOffset < pixelCount;) {
        if (cancellation.isCancellationRequested()) {
            return {.digest = std::nullopt,
                    .cancelled = true,
                    .error = output::OutputSemanticIdentityErrorCodeV1::None};
        }
        const auto count = std::min(kPayloadChunkPixels, pixelCount - pixelOffset);
        const auto byteOffset = pixelOffset * 4U;
        const auto byteCount = count * 4U;
        if (!stream.exact(std::as_bytes(payload.subspan(byteOffset, byteCount)))) {
            return hashFailure(output::OutputSemanticIdentityErrorCodeV1::HashInputTooLarge);
        }
        pixelOffset += count;
        reportProgress(progress,
                       {output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload,
                        pixelOffset, preflight.common.pixelCount});
    }
    if (stream.byteCount() != preflight.common.preimageBytes) {
        return hashFailure(output::OutputSemanticIdentityErrorCodeV1::InternalInvariant);
    }
    return {.digest = stream.finish(),
            .cancelled = false,
            .error = output::OutputSemanticIdentityErrorCodeV1::None};
}

[[nodiscard]] HashResult
hashExr(const output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1& input,
        const ExrPreflight& preflight, const runtime::CancellationToken& cancellation,
        const output::OutputSemanticIdentityProgressCallbackV1& progress) noexcept {
    const auto& metadata = input.verifiedProduct.metadata();
    DigestStream stream;
    bool streamed =
        streamCommon(stream, preflight.common, {}, kExrPayloadKind) &&
        stream.integer(metadata.dataWindow.xMin) && stream.integer(metadata.dataWindow.yMin) &&
        stream.integer(metadata.dataWindow.xMax) && stream.integer(metadata.dataWindow.yMax) &&
        stream.integer(metadata.displayWindow.xMin) &&
        stream.integer(metadata.displayWindow.yMin) &&
        stream.integer(metadata.displayWindow.xMax) &&
        stream.integer(metadata.displayWindow.yMax) &&
        stream.integer(metadata.pixelAspectRatioBits) && stream.integer(kZipCompression) &&
        stream.integer(kIncreasingY) && stream.text(kExrColorId);
    for (const auto bits : output::kFlatExrRec709D65ChromaticitiesBitsV1) {
        streamed = streamed && stream.integer(bits);
    }
    streamed = streamed && stream.text(output::kFlatExrRgba32fSemanticProfileV1) &&
               stream.integer(preflight.common.pixelCount);
    if (!streamed) {
        return hashFailure(output::OutputSemanticIdentityErrorCodeV1::HashInputTooLarge);
    }

    reportProgress(progress, {output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload,
                              0, preflight.common.pixelCount});
    const auto componentBits = input.verifiedProduct.componentBits();
    std::array<std::byte, kExrPayloadChunkBytes> encoded{};
    const auto pixelCount = componentBits.size() / 4U;
    for (std::size_t pixelOffset = 0; pixelOffset < pixelCount;) {
        if (cancellation.isCancellationRequested()) {
            return {.digest = std::nullopt,
                    .cancelled = true,
                    .error = output::OutputSemanticIdentityErrorCodeV1::None};
        }
        const auto count = std::min(kPayloadChunkPixels, pixelCount - pixelOffset);
        std::size_t encodedOffset = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto componentOffset = (pixelOffset + index) * 4U;
            const std::span<const std::uint32_t, 4> pixel(componentBits.data() + componentOffset,
                                                          4);
            if (!output::detail::validOutputSemanticExrPixelBitsV1(pixel)) {
                return hashFailure(
                    output::OutputSemanticIdentityErrorCodeV1::InvalidSemanticPayload);
            }
            for (const auto bits : pixel) {
                const auto bytes = bigEndianBytes(bits);
                std::ranges::copy(bytes,
                                  encoded.begin() + static_cast<std::ptrdiff_t>(encodedOffset));
                encodedOffset += bytes.size();
            }
        }
        if (!stream.exact(std::span(encoded).first(encodedOffset))) {
            return hashFailure(output::OutputSemanticIdentityErrorCodeV1::HashInputTooLarge);
        }
        pixelOffset += count;
        reportProgress(progress,
                       {output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload,
                        pixelOffset, preflight.common.pixelCount});
    }
    if (stream.byteCount() != preflight.common.preimageBytes) {
        return hashFailure(output::OutputSemanticIdentityErrorCodeV1::InternalInvariant);
    }
    return {.digest = stream.finish(),
            .cancelled = false,
            .error = output::OutputSemanticIdentityErrorCodeV1::None};
}

} // namespace

namespace bloom::output {

OutputSemanticIdentityV1PreparationResult
detailPreparePngOutputSemanticIdentityV1(PngRgba8SrgbOutputSemanticIdentityInputV1 input,
                                         const runtime::CancellationToken& cancellation,
                                         const OutputSemanticIdentityProgressCallbackV1& progress,
                                         const bool injectAllocationFailure) noexcept {
    try {
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Preflight, 0, 1});
        const auto preflight = preflightPng(input);
        if (!preflight.value) {
            return OutputSemanticIdentityV1PreparationResult::failed(preflight.error);
        }
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Preflight, 1, 1});
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        const auto hashed = hashPng(input, *preflight.value, cancellation, progress);
        if (hashed.cancelled || cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        if (!hashed.digest) {
            return OutputSemanticIdentityV1PreparationResult::failed(hashed.error);
        }

        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Publishing, 0, 1});
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        if (injectAllocationFailure) {
            throw std::bad_alloc();
        }
        auto identity =
            std::shared_ptr<const OutputSemanticIdentityV1>(new OutputSemanticIdentityV1(
                *hashed.digest,
                OutputSemanticIdentityV1::RetainedProducts(std::move(input.verifiedProduct)),
                preflight.value->common.preimageBytes));
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Publishing, 1, 1});
        return OutputSemanticIdentityV1PreparationResult::prepared(std::move(identity));
    } catch (const std::bad_alloc&) {
        return OutputSemanticIdentityV1PreparationResult::failed(
            OutputSemanticIdentityErrorCodeV1::AllocationFailure);
    } catch (const std::length_error&) {
        return OutputSemanticIdentityV1PreparationResult::failed(
            OutputSemanticIdentityErrorCodeV1::AllocationFailure);
    }
}

OutputSemanticIdentityV1PreparationResult detailPrepareFlatExrOutputSemanticIdentityV1(
    FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 input,
    const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress,
    const bool injectAllocationFailure) noexcept {
    try {
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Preflight, 0, 1});
        const auto preflight = preflightExr(input);
        if (!preflight.value) {
            return OutputSemanticIdentityV1PreparationResult::failed(preflight.error);
        }
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Preflight, 1, 1});
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        const auto hashed = hashExr(input, *preflight.value, cancellation, progress);
        if (hashed.cancelled || cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        if (!hashed.digest) {
            return OutputSemanticIdentityV1PreparationResult::failed(hashed.error);
        }

        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Publishing, 0, 1});
        if (cancellation.isCancellationRequested()) {
            return OutputSemanticIdentityV1PreparationResult::cancelled();
        }
        if (injectAllocationFailure) {
            throw std::bad_alloc();
        }
        auto identity =
            std::shared_ptr<const OutputSemanticIdentityV1>(new OutputSemanticIdentityV1(
                *hashed.digest,
                OutputSemanticIdentityV1::RetainedProducts(std::move(input.verifiedProduct)),
                preflight.value->common.preimageBytes));
        reportProgress(progress, {OutputSemanticIdentityProgressStageV1::Publishing, 1, 1});
        return OutputSemanticIdentityV1PreparationResult::prepared(std::move(identity));
    } catch (const std::bad_alloc&) {
        return OutputSemanticIdentityV1PreparationResult::failed(
            OutputSemanticIdentityErrorCodeV1::AllocationFailure);
    } catch (const std::length_error&) {
        return OutputSemanticIdentityV1PreparationResult::failed(
            OutputSemanticIdentityErrorCodeV1::AllocationFailure);
    }
}

} // namespace bloom::output
