#include <bloom/output/process_frame_semantic_identity.hpp>

#include <bloom/output/output_limits.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using bloom::core::PixelAspectRatio;
using bloom::core::Sha256Digest;
using bloom::core::Sha256Hasher;
using bloom::output::ProcessFrameSemanticIdentityErrorCode;
using bloom::render::ImageExtent;
using bloom::render::Rgba32fImage;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompositionFormatResolution;
using bloom::runtime::ProcessFrame;
using bloom::runtime::ProcessFrameIdentity;
using bloom::runtime::ProxyResolution;

constexpr char kPixelStreamDomain[] = "BloomProcessPixelStream";
constexpr char kSemanticIdentityDomain[] = "BloomProcessFrameSemanticIdentity";
constexpr std::string_view kProcessColorId = "lin_rec709_scene";
constexpr std::string_view kProcessPixelSemanticsProfileId = "bloom.process.rgba32f.semantic.v2";
constexpr std::size_t kPixelStreamFixedBytes =
    sizeof(kPixelStreamDomain) + sizeof(std::uint16_t) + sizeof(std::uint64_t);
constexpr std::size_t kBytesPerPixel = 4 * sizeof(std::uint32_t);
constexpr std::size_t kPixelHashChunkPixels = 256;
constexpr std::size_t kPixelHashChunkBytes = kPixelHashChunkPixels * kBytesPerPixel;
constexpr std::size_t kCompositionIdentityBytes =
    sizeof(kSemanticIdentityDomain) + sizeof(std::uint16_t) + 3 * sizeof(std::uint64_t) +
    2 * sizeof(std::int64_t) + sizeof(std::uint64_t) + sizeof(std::uint8_t) +
    2 * (2 * sizeof(std::int64_t) + 2 * sizeof(std::uint32_t)) + 2 * sizeof(std::uint32_t) +
    sizeof(std::uint32_t) + kProcessColorId.size() + 3 * sizeof(std::uint8_t) +
    sizeof(std::uint32_t) + kProcessPixelSemanticsProfileId.size() + 4 * sizeof(std::uint32_t) +
    bloom::core::kSha256DigestBytes;

static_assert(sizeof(kSemanticIdentityDomain) == 34);
static_assert(kProcessColorId.size() == 16);
static_assert(kProcessPixelSemanticsProfileId.size() == 33);
static_assert(kPixelHashChunkBytes == 4096);
static_assert(kCompositionIdentityBytes ==
              bloom::output::kCompositionProcessFrameSemanticIdentityV1Bytes);
static_assert(kCompositionIdentityBytes + 2 * sizeof(std::uint32_t) ==
              bloom::output::kProxyProcessFrameSemanticIdentityV1Bytes);
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(std::uint32_t));

[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    return value >= 0 ? static_cast<std::uint64_t>(value)
                      : static_cast<std::uint64_t>(-(value + 1)) + 1;
}

[[nodiscard]] bool isNormalizedTime(const bloom::core::RationalTime time) noexcept {
    if (time.denominator() <= 0) {
        return false;
    }
    return std::gcd(magnitude(time.numerator()), static_cast<std::uint64_t>(time.denominator())) ==
           1;
}

[[nodiscard]] std::optional<std::uint64_t>
checkedProduct(const std::array<std::uint64_t, 3>& factors) noexcept {
    std::uint64_t product = 1;
    for (const auto factor : factors) {
        if (factor != 0 && product > std::numeric_limits<std::uint64_t>::max() / factor) {
            return std::nullopt;
        }
        product *= factor;
    }
    return product;
}

[[nodiscard]] std::optional<PixelAspectRatio>
proxyPixelAspect(const bloom::document::CompositionFormat& format,
                 const ImageExtent extent) noexcept {
    std::array<std::uint64_t, 3> numerator{format.pixelAspect().numerator(), format.width(),
                                           extent.height()};
    std::array<std::uint64_t, 3> denominator{format.pixelAspect().denominator(), format.height(),
                                             extent.width()};
    for (auto& numeratorFactor : numerator) {
        for (auto& denominatorFactor : denominator) {
            const auto divisor = std::gcd(numeratorFactor, denominatorFactor);
            numeratorFactor /= divisor;
            denominatorFactor /= divisor;
        }
    }

    const auto reducedNumerator = checkedProduct(numerator);
    const auto reducedDenominator = checkedProduct(denominator);
    if (!reducedNumerator.has_value() || !reducedDenominator.has_value()) {
        return std::nullopt;
    }
    return PixelAspectRatio::create(*reducedNumerator, *reducedDenominator);
}

[[nodiscard]] std::optional<std::uint64_t> checkedPixelCount(const Rgba32fImage& image) noexcept {
    const auto* descriptor = image.descriptor();
    if (!image.isValid() || descriptor == nullptr) {
        return std::nullopt;
    }
    const auto count = descriptor->layout().pixelCount;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (count > std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint64_t>(count);
}

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr std::array<std::byte, sizeof(Integer)>
bigEndianBytes(const Integer value) noexcept {
    std::array<std::byte, sizeof(Integer)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1) * 8);
        bytes[index] = static_cast<std::byte>(value >> shift);
    }
    return bytes;
}

template <std::signed_integral Integer>
[[nodiscard]] constexpr std::array<std::byte, sizeof(Integer)>
bigEndianSignedBytes(const Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    return bigEndianBytes(std::bit_cast<Unsigned>(value));
}

[[nodiscard]] bool update(Sha256Hasher& hasher, const std::span<const std::byte> bytes) noexcept {
    return hasher.update(bytes);
}

template <std::size_t Size>
[[nodiscard]] bool update(Sha256Hasher& hasher, const char (&text)[Size]) noexcept {
    return update(hasher, std::as_bytes(std::span{text}));
}

template <typename Integer>
[[nodiscard]] bool updateBigEndian(Sha256Hasher& hasher, const Integer value) noexcept {
    const auto bytes = [&] {
        if constexpr (std::is_signed_v<Integer>) {
            return bigEndianSignedBytes(value);
        } else {
            return bigEndianBytes(value);
        }
    }();
    return update(hasher, bytes);
}

class FixedWriter final {
  public:
    explicit constexpr FixedWriter(const std::span<std::byte> bytes) noexcept : bytes_(bytes) {}

    template <std::size_t Size> [[nodiscard]] bool literal(const char (&text)[Size]) noexcept {
        return exact(std::as_bytes(std::span{text}));
    }

    [[nodiscard]] bool text(const std::string_view value) noexcept {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        return integer(static_cast<std::uint32_t>(value.size())) &&
               exact(std::as_bytes(std::span{value}));
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

    [[nodiscard]] bool exact(const std::span<const std::byte> value) noexcept {
        if (value.size() > bytes_.size() - offset_) {
            return false;
        }
        std::ranges::copy(value, bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ += value.size();
        return true;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return offset_; }

  private:
    std::span<std::byte> bytes_;
    std::size_t offset_ = 0;
};

struct PreflightIdentity final {
    std::size_t requiredBytes;
    std::uint8_t resolutionKind;
    std::optional<ImageExtent> proxyExtent;
    std::uint64_t outputNodeId;
    std::uint64_t pixelCount;
};

struct PreflightOutcome final {
    std::optional<PreflightIdentity> value;
    ProcessFrameSemanticIdentityErrorCode error =
        ProcessFrameSemanticIdentityErrorCode::InternalInvariant;
};

[[nodiscard]] PreflightOutcome
preflightFailure(const ProcessFrameSemanticIdentityErrorCode error) noexcept {
    return {.value = std::nullopt,
            .error = error == ProcessFrameSemanticIdentityErrorCode::None
                         ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                         : error};
}

[[nodiscard]] bool exceedsOutputLimits(const Rgba32fImage& image,
                                       const std::uint64_t pixelCount) noexcept {
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr) {
        return true;
    }
    const auto dataExtent = descriptor->dataWindow().extent();
    const auto displayExtent = descriptor->displayWindow().extent();
    return dataExtent.width() > bloom::output::kOutputAnalysisMaximumDimensionV1 ||
           dataExtent.height() > bloom::output::kOutputAnalysisMaximumDimensionV1 ||
           displayExtent.width() > bloom::output::kOutputAnalysisMaximumDimensionV1 ||
           displayExtent.height() > bloom::output::kOutputAnalysisMaximumDimensionV1 ||
           pixelCount > bloom::output::kOutputAnalysisMaximumPixelCountV1 ||
           descriptor->layout().pixelStorageBytes >
               bloom::output::kOutputAnalysisMaximumProcessPixelBytesV1;
}

[[nodiscard]] PreflightOutcome preflightIdentity(const ProcessFrame& frame) noexcept {
    const auto& identity = frame.identity();
    const auto& image = frame.processImage();
    if (identity.plan == nullptr) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::MissingPlan);
    }
    const auto& plan = *identity.plan;
    if (!plan.projectId().isValid() || !plan.compositionId().isValid()) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidStableId);
    }
    if (!isNormalizedTime(identity.time)) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidTime);
    }
    if (identity.quality != bloom::runtime::EvaluationQuality::Reference) {
        return preflightFailure(
            ProcessFrameSemanticIdentityErrorCode::UnsupportedEvaluationQuality);
    }
    if (identity.colorIntent != bloom::runtime::EvaluationColorIntent::LinearRec709Scene) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::UnsupportedColorIntent);
    }
    if (plan.planSemanticsVersion() == 0 || plan.animationSamplingSemanticsVersion() == 0 ||
        identity.animationSamplingSemanticsVersion == 0 ||
        identity.evaluatorSemanticsVersion == 0 || identity.imagePrimitiveSemanticsVersion == 0 ||
        identity.animationSamplingSemanticsVersion != plan.animationSamplingSemanticsVersion()) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidSemanticsVersion);
    }
    if (plan.operations().empty() || identity.output != plan.output() ||
        identity.output.value() != plan.operations().size() - 1) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidOutput);
    }
    const auto* output =
        std::get_if<CompiledCompositionOutput>(&plan.operations()[identity.output.value()]);
    if (output == nullptr || output->input.value() >= identity.output.value()) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidOutput);
    }
    if (!output->sourceNodeId.isValid()) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidStableId);
    }

    const auto* descriptor = image.descriptor();
    const auto pixelCount = checkedPixelCount(image);
    if (descriptor == nullptr || !pixelCount.has_value()) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidImage);
    }
    if (exceedsOutputLimits(image, *pixelCount)) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::ResourceLimitExceeded);
    }
    constexpr auto maximumHashBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (*pixelCount > (maximumHashBytes - kPixelStreamFixedBytes) / kBytesPerPixel) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
    }

    const auto displayExtent = descriptor->displayWindow().extent();
    std::uint8_t resolutionKind = 0;
    std::optional<ImageExtent> proxyExtent;
    PixelAspectRatio expectedPixelAspect = plan.format().pixelAspect();
    std::size_t requiredBytes = 0;
    if (std::holds_alternative<CompositionFormatResolution>(identity.resolution)) {
        if (displayExtent.width() != plan.format().width() ||
            displayExtent.height() != plan.format().height()) {
            return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
        }
        resolutionKind = 1;
        requiredBytes = bloom::output::kCompositionProcessFrameSemanticIdentityV1Bytes;
    } else if (const auto* proxy = std::get_if<ProxyResolution>(&identity.resolution)) {
        if (displayExtent != proxy->extent) {
            return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
        }
        const auto proxyAspect = proxyPixelAspect(plan.format(), proxy->extent);
        if (!proxyAspect.has_value()) {
            return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidResolution);
        }
        expectedPixelAspect = *proxyAspect;
        proxyExtent = proxy->extent;
        resolutionKind = 2;
        requiredBytes = bloom::output::kProxyProcessFrameSemanticIdentityV1Bytes;
    } else {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InvalidResolution);
    }
    if (descriptor->pixelAspect() != expectedPixelAspect) {
        return preflightFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
    }
    return {.value = PreflightIdentity{requiredBytes, resolutionKind, proxyExtent,
                                       output->sourceNodeId.value(), *pixelCount},
            .error = ProcessFrameSemanticIdentityErrorCode::None};
}

void reportProgress(const bloom::output::ProcessFrameSemanticIdentityProgressCallback& callback,
                    const bloom::output::ProcessFrameSemanticIdentityProgress& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring is best effort and must not change identity bytes.
        return;
    }
}

struct PixelHashOutcome final {
    std::optional<Sha256Digest> digest;
    ProcessFrameSemanticIdentityErrorCode error =
        ProcessFrameSemanticIdentityErrorCode::InternalInvariant;
    bool cancelled = false;
};

[[nodiscard]] PixelHashOutcome
hashFailure(const ProcessFrameSemanticIdentityErrorCode error) noexcept {
    return {.digest = std::nullopt,
            .error = error == ProcessFrameSemanticIdentityErrorCode::None
                         ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                         : error,
            .cancelled = false};
}

[[nodiscard]] PixelHashOutcome hashProcessPixels(
    const Rgba32fImage& image, const std::uint64_t pixelCount,
    const bloom::runtime::CancellationToken& cancellation,
    const bloom::output::ProcessFrameSemanticIdentityProgressCallback& progress) noexcept {
    reportProgress(
        progress, {.stage = bloom::output::ProcessFrameSemanticIdentityProgressStage::HashingPixels,
                   .completed = 0,
                   .total = pixelCount});
    if (cancellation.isCancellationRequested()) {
        return {.digest = std::nullopt,
                .error = ProcessFrameSemanticIdentityErrorCode::None,
                .cancelled = true};
    }

    Sha256Hasher hasher;
    if (!update(hasher, kPixelStreamDomain) ||
        !updateBigEndian(hasher, bloom::output::kProcessPixelStreamSerializationVersion) ||
        !updateBigEndian(hasher, pixelCount)) {
        return hashFailure(ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
    }

    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr) {
        return hashFailure(ProcessFrameSemanticIdentityErrorCode::InternalInvariant);
    }
    const auto width = static_cast<std::size_t>(descriptor->dataWindow().extent().width());
    const auto height = descriptor->dataWindow().extent().height();
    const auto pixels = image.pixels();
    std::array<std::byte, kPixelHashChunkBytes> encoded{};
    std::uint64_t completed = 0;
    for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
        const auto rowOffset = static_cast<std::size_t>(rowIndex) * width;
        const auto row = pixels.subspan(rowOffset, width);
        for (std::size_t offset = 0; offset < row.size(); offset += kPixelHashChunkPixels) {
            if (cancellation.isCancellationRequested()) {
                return {.digest = std::nullopt,
                        .error = ProcessFrameSemanticIdentityErrorCode::None,
                        .cancelled = true};
            }
            const auto chunkPixelCount = std::min(kPixelHashChunkPixels, row.size() - offset);
            std::size_t encodedOffset = 0;
            for (const auto pixel : row.subspan(offset, chunkPixelCount)) {
                const auto alpha = pixel.alpha();
                const auto hasNoncanonicalTransparentBits =
                    alpha == 0.0F &&
                    std::ranges::any_of(pixel.components(), [](const float component) {
                        return std::bit_cast<std::uint32_t>(component) != 0U;
                    });
                if (!std::isfinite(pixel.red()) || !std::isfinite(pixel.green()) ||
                    !std::isfinite(pixel.blue()) || !std::isfinite(alpha) || alpha < 0.0F ||
                    alpha > 1.0F || hasNoncanonicalTransparentBits) {
                    return hashFailure(ProcessFrameSemanticIdentityErrorCode::InvalidPixel);
                }
                for (const auto component : pixel.components()) {
                    const auto componentBytes =
                        bigEndianBytes(std::bit_cast<std::uint32_t>(component));
                    std::ranges::copy(componentBytes,
                                      encoded.begin() + static_cast<std::ptrdiff_t>(encodedOffset));
                    encodedOffset += componentBytes.size();
                }
            }
            if (!update(hasher, std::span(encoded).first(encodedOffset))) {
                return hashFailure(ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
            }
            completed += static_cast<std::uint64_t>(chunkPixelCount);
        }
        reportProgress(
            progress,
            {.stage = bloom::output::ProcessFrameSemanticIdentityProgressStage::HashingPixels,
             .completed = completed,
             .total = pixelCount});
        if (cancellation.isCancellationRequested()) {
            return {.digest = std::nullopt,
                    .error = ProcessFrameSemanticIdentityErrorCode::None,
                    .cancelled = true};
        }
    }
    if (completed != pixelCount) {
        return hashFailure(ProcessFrameSemanticIdentityErrorCode::InternalInvariant);
    }
    return {.digest = hasher.finalize(),
            .error = ProcessFrameSemanticIdentityErrorCode::None,
            .cancelled = false};
}

[[nodiscard]] bool emitIdentity(const ProcessFrameIdentity& identity, const Rgba32fImage& image,
                                const PreflightIdentity& preflight,
                                const Sha256Digest& processPixelDigest,
                                const std::span<std::byte> destination) noexcept {
    const auto& plan = *identity.plan;
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr) {
        return false;
    }
    const auto dataWindow = descriptor->dataWindow();
    const auto displayWindow = descriptor->displayWindow();
    const auto pixelAspect = descriptor->pixelAspect();
    FixedWriter writer(destination);
    if (!writer.literal(kSemanticIdentityDomain) ||
        !writer.integer(bloom::output::kProcessFrameSemanticIdentitySerializationVersion) ||
        !writer.integer(plan.projectId().value()) ||
        !writer.integer(plan.compositionId().value()) ||
        !writer.integer(plan.sourceRevision().value()) ||
        !writer.integer(identity.time.numerator()) ||
        !writer.integer(identity.time.denominator()) || !writer.integer(preflight.outputNodeId) ||
        !writer.integer(preflight.resolutionKind)) {
        return false;
    }
    if (preflight.proxyExtent.has_value() && (!writer.integer(preflight.proxyExtent->width()) ||
                                              !writer.integer(preflight.proxyExtent->height()))) {
        return false;
    }
    return writer.integer(dataWindow.originX()) && writer.integer(dataWindow.originY()) &&
           writer.integer(dataWindow.extent().width()) &&
           writer.integer(dataWindow.extent().height()) &&
           writer.integer(displayWindow.originX()) && writer.integer(displayWindow.originY()) &&
           writer.integer(displayWindow.extent().width()) &&
           writer.integer(displayWindow.extent().height()) &&
           writer.integer(pixelAspect.numerator()) && writer.integer(pixelAspect.denominator()) &&
           writer.text(kProcessColorId) && writer.integer(std::uint8_t{1}) &&
           writer.integer(std::uint8_t{1}) && writer.integer(std::uint8_t{1}) &&
           writer.text(kProcessPixelSemanticsProfileId) &&
           writer.integer(plan.planSemanticsVersion()) &&
           writer.integer(identity.animationSamplingSemanticsVersion) &&
           writer.integer(identity.evaluatorSemanticsVersion) &&
           writer.integer(identity.imagePrimitiveSemanticsVersion) &&
           writer.exact(std::as_bytes(processPixelDigest.bytes())) &&
           writer.size() == preflight.requiredBytes;
}

} // namespace

namespace bloom::output {

ProcessFrameSemanticIdentityV1::ProcessFrameSemanticIdentityV1(
    std::shared_ptr<const runtime::ProcessFrame> processFrame,
    std::array<std::byte, kProxyProcessFrameSemanticIdentityV1Bytes> canonicalBytes,
    const std::size_t canonicalByteCount, const core::Sha256Digest processPixelDigest) noexcept
    : processFrame_(std::move(processFrame)), canonicalBytes_(canonicalBytes),
      canonicalByteCount_(canonicalByteCount), processPixelDigest_(processPixelDigest) {}

ProcessFrameSemanticIdentityV1PreparationResult
ProcessFrameSemanticIdentityV1PreparationResult::prepared(
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> identity) noexcept {
    if (identity == nullptr) {
        return failed(ProcessFrameSemanticIdentityErrorCode::InternalInvariant);
    }
    return {ProcessFrameSemanticIdentityPreparationStatus::Prepared, std::move(identity),
            ProcessFrameSemanticIdentityErrorCode::None};
}

ProcessFrameSemanticIdentityV1PreparationResult
ProcessFrameSemanticIdentityV1PreparationResult::cancelled() noexcept {
    return {ProcessFrameSemanticIdentityPreparationStatus::Cancelled,
            {},
            ProcessFrameSemanticIdentityErrorCode::None};
}

ProcessFrameSemanticIdentityV1PreparationResult
ProcessFrameSemanticIdentityV1PreparationResult::failed(
    const ProcessFrameSemanticIdentityErrorCode error) noexcept {
    return {ProcessFrameSemanticIdentityPreparationStatus::Failed,
            {},
            error == ProcessFrameSemanticIdentityErrorCode::None
                ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                : error};
}

ProcessFrameSemanticIdentityV1PreparationResult::ProcessFrameSemanticIdentityV1PreparationResult(
    const ProcessFrameSemanticIdentityPreparationStatus status,
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> identity,
    const ProcessFrameSemanticIdentityErrorCode error) noexcept
    : status_(status), identity_(std::move(identity)), error_(error) {}

ProcessFrameSemanticIdentityV1PreparationResult ProcessFrameSemanticIdentityV1Preparer::prepare(
    std::shared_ptr<const runtime::ProcessFrame> processFrame,
    const runtime::CancellationToken& cancellation,
    const ProcessFrameSemanticIdentityProgressCallback& progress) const noexcept {
    try {
        if (cancellation.isCancellationRequested()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::cancelled();
        }
        reportProgress(progress, {.stage = ProcessFrameSemanticIdentityProgressStage::Preflight,
                                  .completed = 0,
                                  .total = 1});
        if (processFrame == nullptr) {
            return ProcessFrameSemanticIdentityV1PreparationResult::failed(
                ProcessFrameSemanticIdentityErrorCode::MissingFrame);
        }
        const auto preflight = preflightIdentity(*processFrame);
        if (!preflight.value.has_value()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::failed(preflight.error);
        }
        reportProgress(progress, {.stage = ProcessFrameSemanticIdentityProgressStage::Preflight,
                                  .completed = 1,
                                  .total = 1});
        if (cancellation.isCancellationRequested()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::cancelled();
        }

        const auto hashed = hashProcessPixels(processFrame->processImage(),
                                              preflight.value->pixelCount, cancellation, progress);
        if (hashed.cancelled || cancellation.isCancellationRequested()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::cancelled();
        }
        if (!hashed.digest.has_value()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::failed(hashed.error);
        }

        reportProgress(progress, {.stage = ProcessFrameSemanticIdentityProgressStage::Encoding,
                                  .completed = 0,
                                  .total = 1});
        if (cancellation.isCancellationRequested()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::cancelled();
        }
        std::array<std::byte, kProxyProcessFrameSemanticIdentityV1Bytes> canonicalBytes{};
        if (!emitIdentity(processFrame->identity(), processFrame->processImage(), *preflight.value,
                          *hashed.digest,
                          std::span(canonicalBytes).first(preflight.value->requiredBytes))) {
            return ProcessFrameSemanticIdentityV1PreparationResult::failed(
                ProcessFrameSemanticIdentityErrorCode::InternalInvariant);
        }
        reportProgress(progress, {.stage = ProcessFrameSemanticIdentityProgressStage::Encoding,
                                  .completed = 1,
                                  .total = 1});
        if (cancellation.isCancellationRequested()) {
            return ProcessFrameSemanticIdentityV1PreparationResult::cancelled();
        }

        auto identity = std::shared_ptr<const ProcessFrameSemanticIdentityV1>(
            new ProcessFrameSemanticIdentityV1(std::move(processFrame), canonicalBytes,
                                               preflight.value->requiredBytes, *hashed.digest));
        return ProcessFrameSemanticIdentityV1PreparationResult::prepared(std::move(identity));
    } catch (const std::bad_alloc&) {
        return ProcessFrameSemanticIdentityV1PreparationResult::failed(
            ProcessFrameSemanticIdentityErrorCode::AllocationFailure);
    } catch (const std::length_error&) {
        return ProcessFrameSemanticIdentityV1PreparationResult::failed(
            ProcessFrameSemanticIdentityErrorCode::AllocationFailure);
    }
}

} // namespace bloom::output
