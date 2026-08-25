#include <bloom/output/output_analysis_digest.hpp>

#include <bloom/output/output_limits.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace {

namespace color = bloom::color;
namespace core = bloom::core;
namespace output = bloom::output;
namespace render = bloom::render;

constexpr char kOutputAnalysisDigestDomain[] = "BloomOutputAnalysisDigest";
static_assert(sizeof(kOutputAnalysisDigestDomain) == 26);

[[nodiscard]] bool checkedAdd(std::size_t& total, const std::size_t value) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] std::optional<std::string_view> fieldValue(const std::string_view descriptor,
                                                         const std::string_view key) noexcept {
    std::size_t offset = 0;
    while (offset < descriptor.size()) {
        const auto separator = descriptor.find(';', offset);
        const auto fieldEnd = separator == std::string_view::npos ? descriptor.size() : separator;
        const auto field = descriptor.substr(offset, fieldEnd - offset);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos) {
            return std::nullopt;
        }
        if (field.substr(0, equals) == key) {
            return field.substr(equals + 1U);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return std::nullopt;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parseTaggedInteger(const std::string_view value,
                                                        const std::string_view tag) noexcept {
    if (!value.starts_with(tag)) {
        return std::nullopt;
    }
    const auto digits = value.substr(tag.size());
    Integer result{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()
               ? std::optional<Integer>{result}
               : std::nullopt;
}

[[nodiscard]] bool descriptorUnsignedEquals(const std::string_view descriptor,
                                            const std::string_view key,
                                            const std::uint32_t expected) noexcept {
    const auto value = fieldValue(descriptor, key);
    return value && parseTaggedInteger<std::uint32_t>(*value, "u:") == expected;
}

[[nodiscard]] bool descriptorSignedEquals(const std::string_view descriptor,
                                          const std::string_view key,
                                          const std::int64_t expected) noexcept {
    const auto value = fieldValue(descriptor, key);
    return value && parseTaggedInteger<std::int64_t>(*value, "i:") == expected;
}

[[nodiscard]] bool windowDescriptorMatches(const std::string_view descriptor,
                                           const render::ImageWindow window) noexcept {
    return descriptorUnsignedEquals(descriptor, "height", window.extent().height()) &&
           descriptorSignedEquals(descriptor, "origin-x", window.originX()) &&
           descriptorSignedEquals(descriptor, "origin-y", window.originY()) &&
           descriptorUnsignedEquals(descriptor, "width", window.extent().width());
}

[[nodiscard]] bool
reportSourceMatchesProcessFrame(const output::OutputAnalysisReportV1View report,
                                const bloom::runtime::ProcessFrame& processFrame) noexcept {
    const auto* const descriptor = processFrame.processImage().descriptor();
    if (descriptor == nullptr || report.facets.size() != output::kOutputAnalysisFacetCountV1) {
        return false;
    }
    const auto dataWindow = descriptor->dataWindow();
    const auto displayWindow = descriptor->displayWindow();
    const auto pixelAspect = descriptor->pixelAspect();
    const auto& pixels = report.facets[0].sourceDescriptor;
    const auto& data = report.facets[5].sourceDescriptor;
    const auto& display = report.facets[6].sourceDescriptor;
    const auto& aspect = report.facets[7].sourceDescriptor;
    return descriptorUnsignedEquals(pixels, "height", dataWindow.extent().height()) &&
           descriptorUnsignedEquals(pixels, "width", dataWindow.extent().width()) &&
           windowDescriptorMatches(data, dataWindow) &&
           windowDescriptorMatches(display, displayWindow) &&
           descriptorUnsignedEquals(aspect, "numerator", pixelAspect.numerator()) &&
           descriptorUnsignedEquals(aspect, "denominator", pixelAspect.denominator());
}

[[nodiscard]] bool
processFrameFitsOutputV1Limits(const bloom::runtime::ProcessFrame& processFrame) noexcept {
    const auto* const descriptor = processFrame.processImage().descriptor();
    if (descriptor == nullptr) {
        return false;
    }
    const auto dataExtent = descriptor->dataWindow().extent();
    const auto displayExtent = descriptor->displayWindow().extent();
    const auto& layout = descriptor->layout();
    return dataExtent.width() <= output::kOutputAnalysisMaximumDimensionV1 &&
           dataExtent.height() <= output::kOutputAnalysisMaximumDimensionV1 &&
           displayExtent.width() <= output::kOutputAnalysisMaximumDimensionV1 &&
           displayExtent.height() <= output::kOutputAnalysisMaximumDimensionV1 &&
           static_cast<std::uint64_t>(layout.pixelCount) <=
               output::kOutputAnalysisMaximumPixelCountV1 &&
           layout.pixelStorageBytes <= output::kOutputAnalysisMaximumProcessPixelBytesV1;
}

[[nodiscard]] bool targetDependencyMatches(const std::string_view descriptor,
                                           const core::Sha256Digest& expectedRevision) noexcept {
    constexpr std::string_view prefix = "kind=id:ocio;revision=id:";
    if (!descriptor.starts_with(prefix)) {
        return false;
    }
    const auto expectedHex = expectedRevision.toLowercaseHex();
    return descriptor.substr(prefix.size()) ==
           std::string_view(expectedHex.data(), expectedHex.size());
}

class DigestStream final {
  public:
    [[nodiscard]] bool append(const std::span<const std::byte> bytes) noexcept {
        if (!hasher_.update(bytes)) {
            return false;
        }
        byteCount_ += bytes.size();
        return true;
    }

    [[nodiscard]] bool appendU8(const std::uint8_t value) noexcept {
        const std::array bytes{static_cast<std::byte>(value)};
        return append(bytes);
    }

    [[nodiscard]] bool appendU16(const std::uint16_t value) noexcept {
        const std::array bytes{static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)};
        return append(bytes);
    }

    [[nodiscard]] bool appendU32(const std::uint32_t value) noexcept {
        const std::array bytes{static_cast<std::byte>(value >> 24U),
                               static_cast<std::byte>(value >> 16U),
                               static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)};
        return append(bytes);
    }

    [[nodiscard]] bool appendText(const std::string_view text) noexcept {
        if (text.size() > std::numeric_limits<std::uint32_t>::max() ||
            !appendU32(static_cast<std::uint32_t>(text.size()))) {
            return false;
        }
        return append(std::as_bytes(std::span(text.data(), text.size())));
    }

    [[nodiscard]] bool appendBytes(const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
            !appendU32(static_cast<std::uint32_t>(bytes.size()))) {
            return false;
        }
        return append(bytes);
    }

    [[nodiscard]] core::Sha256Digest finish() const noexcept { return hasher_.finalize(); }
    [[nodiscard]] std::size_t byteCount() const noexcept { return byteCount_; }

  private:
    core::Sha256Hasher hasher_;
    std::size_t byteCount_ = 0;
};

[[nodiscard]] std::optional<std::size_t> encodedTextSize(const std::string_view text) noexcept {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::size_t size = sizeof(std::uint32_t);
    return checkedAdd(size, text.size()) ? std::optional<std::size_t>{size} : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> encodedBytesSize(const std::size_t byteCount) noexcept {
    if (byteCount > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::size_t size = sizeof(std::uint32_t);
    return checkedAdd(size, byteCount) ? std::optional<std::size_t>{size} : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t>
preimageSize(const output::OutputAnalysisReportV1View report,
             const output::OutputPresetIdentityV1& presetIdentity,
             const std::array<std::string_view, output::kOutputAnalysisFacetCountV1>& stableCodes,
             const std::size_t processIdentityBytes, const std::size_t revisionBytes,
             const std::size_t displayIdentityBytes) noexcept {
    std::size_t size = sizeof(kOutputAnalysisDigestDomain);
    if (!checkedAdd(size, sizeof(std::uint16_t))) {
        return std::nullopt;
    }
    const auto processSize = encodedBytesSize(processIdentityBytes);
    const auto presetIdSize = encodedTextSize(presetIdentity.serializedId);
    const auto profileSize = encodedTextSize(presetIdentity.outputPixelSemanticsProfileId);
    const auto revisionSize = encodedBytesSize(revisionBytes);
    const auto displaySize = encodedBytesSize(displayIdentityBytes);
    if (!processSize || !presetIdSize || !profileSize || !revisionSize || !displaySize ||
        !checkedAdd(size, *processSize) || !checkedAdd(size, *presetIdSize) ||
        !checkedAdd(size, sizeof(std::uint32_t)) || !checkedAdd(size, *profileSize) ||
        !checkedAdd(size, *revisionSize) || !checkedAdd(size, *displaySize) ||
        !checkedAdd(size, sizeof(std::uint16_t))) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < report.facets.size(); ++index) {
        const auto& facet = report.facets[index];
        const auto codeSize = encodedTextSize(stableCodes[index]);
        const auto sourceSize = encodedTextSize(facet.sourceDescriptor);
        const auto targetSize = encodedTextSize(facet.targetDescriptor);
        if (!codeSize || !sourceSize || !targetSize || !checkedAdd(size, 3U) ||
            !checkedAdd(size, *codeSize) || !checkedAdd(size, *sourceSize) ||
            !checkedAdd(size, *targetSize)) {
            return std::nullopt;
        }
    }
    return size;
}

} // namespace

namespace bloom::output {

OutputAnalysisDigestV1Result
computeOutputAnalysisDigestV1(const ProcessFrameSemanticIdentityV1& processIdentity,
                              const OutputAnalysisReportV1View report,
                              const OutputAnalysisDigestDependenciesV1& dependencies) noexcept {
    const auto reportValidation = validateOutputAnalysisReportV1(report);
    if (!reportValidation) {
        return OutputAnalysisDigestV1Result::failure(OutputAnalysisDigestErrorCodeV1::InvalidReport,
                                                     0, reportValidation.issue());
    }
    const auto permissionMask = reportValidation.permissionMask();
    if (!permissionMask) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant);
    }
    if (!reportValidation.approvable()) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::ReportNotApprovable);
    }

    const auto& processFrameOwner = processIdentity.processFrame();
    if (processFrameOwner == nullptr) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant);
    }
    const auto& processFrame = *processFrameOwner;
    if (!reportSourceMatchesProcessFrame(report, processFrame)) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::ProcessFrameDescriptorMismatch);
    }
    if (!processFrameFitsOutputV1Limits(processFrame)) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::ResourceLimitExceeded);
    }

    const auto presetIdentity = outputPresetIdentityV1(report.preset);
    if (!presetIdentity) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant);
    }

    std::array<std::string_view, kOutputAnalysisFacetCountV1> stableCodes{};
    for (std::size_t index = 0; index < report.facets.size(); ++index) {
        const auto stableCode = outputFacetStableCodeTextV1(report.facets[index].stableCode);
        if (!stableCode) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::StableCodeInvariantViolation);
        }
        stableCodes[index] = *stableCode;
    }

    std::span<const std::byte> displayIdentityBytes;
    std::span<const std::byte> revisionBytes;
    if (report.preset == OutputPresetV1::PngRgba8SrgbV1) {
        if (!dependencies.expectedOcioRevision) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::MissingPngExpectedOcioRevision);
        }
        if (dependencies.displayProcessorIdentity == nullptr) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::MissingPngDisplayIdentity);
        }
        const auto displayView = dependencies.displayProcessorIdentity->borrowedView();
        if (!displayView) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::InvalidDisplayIdentity);
        }
        if (displayView->expectedOcioRevision() != *dependencies.expectedOcioRevision) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::OcioRevisionMismatch);
        }
        if (!targetDependencyMatches(report.facets[10].targetDescriptor,
                                     *dependencies.expectedOcioRevision)) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::TargetDependencyRevisionMismatch);
        }
        revisionBytes = std::as_bytes(dependencies.expectedOcioRevision->bytes());
        displayIdentityBytes = displayView->canonicalBytes();
    } else if (report.preset == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
        if (dependencies.expectedOcioRevision) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::UnexpectedExrExpectedOcioRevision);
        }
        if (dependencies.displayProcessorIdentity != nullptr) {
            return OutputAnalysisDigestV1Result::failure(
                OutputAnalysisDigestErrorCodeV1::UnexpectedExrDisplayIdentity);
        }
    } else {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant);
    }

    const auto processIdentityBytes = processIdentity.canonicalBytes();
    const auto calculatedPreimageSize =
        preimageSize(report, *presetIdentity, stableCodes, processIdentityBytes.size(),
                     revisionBytes.size(), displayIdentityBytes.size());
    if (!calculatedPreimageSize) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::PreimageSizeOverflow);
    }
    if (*calculatedPreimageSize > kOutputAnalysisDigestMaximumPreimageBytesV1) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::PreimageTooLarge, *calculatedPreimageSize);
    }

    DigestStream stream;
    const auto domainBytes =
        std::as_bytes(std::span(kOutputAnalysisDigestDomain, sizeof(kOutputAnalysisDigestDomain)));
    bool streamed = stream.append(domainBytes) &&
                    stream.appendU16(kOutputAnalysisDigestSerializationVersionV1) &&
                    stream.appendBytes(processIdentityBytes) &&
                    stream.appendText(presetIdentity->serializedId) &&
                    stream.appendU32(presetIdentity->version) &&
                    stream.appendText(presetIdentity->outputPixelSemanticsProfileId) &&
                    stream.appendBytes(revisionBytes) && stream.appendBytes(displayIdentityBytes) &&
                    stream.appendU16(static_cast<std::uint16_t>(report.facets.size()));
    for (std::size_t index = 0; index < report.facets.size(); ++index) {
        const auto& facet = report.facets[index];
        streamed = streamed && stream.appendU8(static_cast<std::uint8_t>(facet.facet)) &&
                   stream.appendU8(static_cast<std::uint8_t>(facet.state)) &&
                   stream.appendU8(permissionMask->permits(facet.facet) ? 1U : 0U) &&
                   stream.appendText(stableCodes[index]) &&
                   stream.appendText(facet.sourceDescriptor) &&
                   stream.appendText(facet.targetDescriptor);
    }
    if (!streamed) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant, stream.byteCount());
    }
    if (stream.byteCount() != *calculatedPreimageSize) {
        return OutputAnalysisDigestV1Result::failure(
            OutputAnalysisDigestErrorCodeV1::InternalInvariant, stream.byteCount());
    }
    return OutputAnalysisDigestV1Result::success(stream.finish(), stream.byteCount());
}

} // namespace bloom::output
