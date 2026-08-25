#include <bloom/output/process_frame_semantic_identity.hpp>

#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <type_traits>
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
using bloom::runtime::ProcessFrameIdentity;
using bloom::runtime::ProxyResolution;

constexpr char kPixelStreamDomain[] = "BloomProcessPixelStream";
constexpr char kSemanticIdentityDomain[] = "BloomProcessFrameSemanticIdentity";
constexpr std::string_view kProcessColorId = "lin_rec709_scene";
constexpr std::string_view kProcessPixelSemanticsProfileId = "bloom.process.rgba32f.semantic.v2";
constexpr std::size_t kPixelStreamFixedBytes =
    sizeof(kPixelStreamDomain) + sizeof(std::uint16_t) + sizeof(std::uint64_t);
constexpr std::size_t kBytesPerPixel = 4 * sizeof(std::uint32_t);
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

struct ValidatedIdentity final {
    std::size_t requiredBytes;
    std::uint8_t resolutionKind;
    std::optional<ImageExtent> proxyExtent;
    std::uint64_t outputNodeId;
    Sha256Digest processPixelDigest;
};

struct ValidationOutcome final {
    std::optional<ValidatedIdentity> value;
    ProcessFrameSemanticIdentityErrorCode failure;
};

[[nodiscard]] ValidationOutcome
validationFailure(const ProcessFrameSemanticIdentityErrorCode failure) noexcept {
    return {.value = std::nullopt, .failure = failure};
}

[[nodiscard]] ValidationOutcome validateIdentity(const ProcessFrameIdentity& identity,
                                                 const Rgba32fImage& image) noexcept {
    if (identity.plan == nullptr) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::MissingPlan);
    }
    const auto& plan = *identity.plan;
    if (!plan.projectId.isValid() || !plan.compositionId.isValid()) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidStableId);
    }
    if (!isNormalizedTime(identity.time)) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidTime);
    }
    if (identity.quality != bloom::runtime::EvaluationQuality::Reference) {
        return validationFailure(
            ProcessFrameSemanticIdentityErrorCode::UnsupportedEvaluationQuality);
    }
    if (identity.colorIntent != bloom::runtime::EvaluationColorIntent::LinearRec709Scene) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::UnsupportedColorIntent);
    }
    if (plan.planSemanticsVersion == 0 || plan.animationSamplingSemanticsVersion == 0 ||
        identity.animationSamplingSemanticsVersion == 0 ||
        identity.evaluatorSemanticsVersion == 0 || identity.imagePrimitiveSemanticsVersion == 0 ||
        identity.animationSamplingSemanticsVersion != plan.animationSamplingSemanticsVersion) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidSemanticsVersion);
    }
    if (plan.operations.empty() || identity.output != plan.output ||
        identity.output.value() != plan.operations.size() - 1) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidOutput);
    }
    const auto* output =
        std::get_if<CompiledCompositionOutput>(&plan.operations[identity.output.value()]);
    if (output == nullptr || output->input.value() >= identity.output.value()) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidOutput);
    }
    if (!output->sourceNodeId.isValid()) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidStableId);
    }

    const auto* descriptor = image.descriptor();
    const auto pixelCount = checkedPixelCount(image);
    if (descriptor == nullptr || !pixelCount.has_value()) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidImage);
    }
    const auto displayExtent = descriptor->displayWindow().extent();
    std::uint8_t resolutionKind = 0;
    std::optional<ImageExtent> proxyExtent;
    PixelAspectRatio expectedPixelAspect = plan.format.pixelAspect();
    std::size_t requiredBytes = 0;
    if (std::holds_alternative<CompositionFormatResolution>(identity.resolution)) {
        if (displayExtent.width() != plan.format.width() ||
            displayExtent.height() != plan.format.height()) {
            return validationFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
        }
        resolutionKind = 1;
        requiredBytes = bloom::output::kCompositionProcessFrameSemanticIdentityV1Bytes;
    } else if (const auto* proxy = std::get_if<ProxyResolution>(&identity.resolution)) {
        if (displayExtent != proxy->extent) {
            return validationFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
        }
        const auto proxyAspect = proxyPixelAspect(plan.format, proxy->extent);
        if (!proxyAspect.has_value()) {
            return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidResolution);
        }
        expectedPixelAspect = *proxyAspect;
        proxyExtent = proxy->extent;
        resolutionKind = 2;
        requiredBytes = bloom::output::kProxyProcessFrameSemanticIdentityV1Bytes;
    } else {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InvalidResolution);
    }
    if (descriptor->pixelAspect() != expectedPixelAspect) {
        return validationFailure(ProcessFrameSemanticIdentityErrorCode::InconsistentImage);
    }
    const auto digestResult = bloom::output::hashProcessPixelStreamV1(image);
    if (!digestResult) {
        return validationFailure(digestResult.error());
    }
    return {.value = ValidatedIdentity{requiredBytes, resolutionKind, proxyExtent,
                                       output->sourceNodeId.value(), digestResult.digest()},
            .failure = ProcessFrameSemanticIdentityErrorCode::None};
}

[[nodiscard]] bool emitIdentity(const ProcessFrameIdentity& identity, const Rgba32fImage& image,
                                const ValidatedIdentity& validated,
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
        !writer.integer(plan.projectId.value()) || !writer.integer(plan.compositionId.value()) ||
        !writer.integer(plan.sourceRevision.value()) ||
        !writer.integer(identity.time.numerator()) ||
        !writer.integer(identity.time.denominator()) || !writer.integer(validated.outputNodeId) ||
        !writer.integer(validated.resolutionKind)) {
        return false;
    }
    if (validated.proxyExtent.has_value() && (!writer.integer(validated.proxyExtent->width()) ||
                                              !writer.integer(validated.proxyExtent->height()))) {
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
           writer.integer(plan.planSemanticsVersion) &&
           writer.integer(identity.animationSamplingSemanticsVersion) &&
           writer.integer(identity.evaluatorSemanticsVersion) &&
           writer.integer(identity.imagePrimitiveSemanticsVersion) &&
           writer.exact(std::as_bytes(validated.processPixelDigest.bytes())) &&
           writer.size() == validated.requiredBytes;
}

} // namespace

namespace bloom::output {

ProcessPixelDigestV1Result hashProcessPixelStreamV1(const render::Rgba32fImage& image) noexcept {
    const auto pixelCount = checkedPixelCount(image);
    if (!pixelCount.has_value()) {
        return ProcessPixelDigestV1Result::failure(
            ProcessFrameSemanticIdentityErrorCode::InvalidImage);
    }
    constexpr auto maximumHashBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (*pixelCount > (maximumHashBytes - kPixelStreamFixedBytes) / kBytesPerPixel) {
        return ProcessPixelDigestV1Result::failure(
            ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
    }

    Sha256Hasher hasher;
    if (!update(hasher, kPixelStreamDomain) ||
        !updateBigEndian(hasher, kProcessPixelStreamSerializationVersion) ||
        !updateBigEndian(hasher, *pixelCount)) {
        return ProcessPixelDigestV1Result::failure(
            ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
    }
    for (const auto pixel : image.pixels()) {
        const auto alpha = pixel.alpha();
        if (!std::isfinite(pixel.red()) || !std::isfinite(pixel.green()) ||
            !std::isfinite(pixel.blue()) || !std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F ||
            (alpha == 0.0F &&
             (pixel.red() != 0.0F || pixel.green() != 0.0F || pixel.blue() != 0.0F))) {
            return ProcessPixelDigestV1Result::failure(
                ProcessFrameSemanticIdentityErrorCode::InvalidPixel);
        }
        for (const auto component : pixel.components()) {
            if (!updateBigEndian(hasher, std::bit_cast<std::uint32_t>(component))) {
                return ProcessPixelDigestV1Result::failure(
                    ProcessFrameSemanticIdentityErrorCode::HashInputTooLarge);
            }
        }
    }
    return ProcessPixelDigestV1Result::success(hasher.finalize());
}

ProcessFrameSemanticIdentityV1Validation
validateProcessFrameSemanticIdentityV1(const runtime::ProcessFrameIdentity& identity,
                                       const render::Rgba32fImage& image) noexcept {
    const auto validated = validateIdentity(identity, image);
    if (!validated.value.has_value()) {
        return ProcessFrameSemanticIdentityV1Validation::failure(validated.failure);
    }
    return ProcessFrameSemanticIdentityV1Validation::success(validated.value->requiredBytes,
                                                             validated.value->processPixelDigest);
}

ProcessFrameSemanticIdentityV1Validation
validateProcessFrameSemanticIdentityV1(const runtime::ProcessFrame& frame) noexcept {
    return validateProcessFrameSemanticIdentityV1(frame.identity(), frame.processImage());
}

ProcessFrameSemanticIdentityV1WriteResult
writeProcessFrameSemanticIdentityV1(const runtime::ProcessFrameIdentity& identity,
                                    const render::Rgba32fImage& image,
                                    const std::span<std::byte> destination) noexcept {
    const auto validated = validateIdentity(identity, image);
    if (!validated.value.has_value()) {
        return ProcessFrameSemanticIdentityV1WriteResult::failure(validated.failure);
    }
    const auto requiredBytes = validated.value->requiredBytes;
    if (destination.size() < requiredBytes) {
        return ProcessFrameSemanticIdentityV1WriteResult::failure(
            ProcessFrameSemanticIdentityErrorCode::InsufficientCapacity, requiredBytes,
            destination.size(), validated.value->processPixelDigest);
    }

    std::array<std::byte, kProxyProcessFrameSemanticIdentityV1Bytes> encoded{};
    if (!emitIdentity(identity, image, *validated.value, std::span(encoded).first(requiredBytes))) {
        return ProcessFrameSemanticIdentityV1WriteResult::failure(
            ProcessFrameSemanticIdentityErrorCode::InternalInvariant, requiredBytes,
            destination.size(), validated.value->processPixelDigest);
    }
    std::ranges::copy(std::span(encoded).first(requiredBytes), destination.begin());
    return ProcessFrameSemanticIdentityV1WriteResult::success(requiredBytes,
                                                              validated.value->processPixelDigest);
}

ProcessFrameSemanticIdentityV1WriteResult
writeProcessFrameSemanticIdentityV1(const runtime::ProcessFrame& frame,
                                    const std::span<std::byte> destination) noexcept {
    return writeProcessFrameSemanticIdentityV1(frame.identity(), frame.processImage(), destination);
}

} // namespace bloom::output
