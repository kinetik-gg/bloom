#include <bloom/output/output_analysis.hpp>

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using bloom::output::OutputAnalysisReportErrorCodeV1;
using bloom::output::OutputAnalysisReportIssueV1;
using bloom::output::OutputFacetDescriptorSideV1;
using bloom::output::OutputFacetIdV1;
using bloom::output::OutputFacetStableCodeRuleV1;
using bloom::output::OutputFacetStableCodeV1;
using bloom::output::OutputPreservationStateV1;
using bloom::output::OutputPresetV1;

[[nodiscard]] constexpr std::uint16_t facetBit(const OutputFacetIdV1 facet) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t{1} << (static_cast<std::uint8_t>(facet) - 1U));
}

[[nodiscard]] constexpr bool isKnownState(const OutputPreservationStateV1 state) noexcept {
    switch (state) {
    case OutputPreservationStateV1::Exact:
    case OutputPreservationStateV1::Equivalent:
    case OutputPreservationStateV1::Approximated:
    case OutputPreservationStateV1::Omitted:
    case OutputPreservationStateV1::ExternalReference:
    case OutputPreservationStateV1::Missing:
    case OutputPreservationStateV1::Unsupported:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool isKnownFacet(const OutputFacetIdV1 facet) noexcept {
    switch (facet) {
    case OutputFacetIdV1::Pixels:
    case OutputFacetIdV1::Precision:
    case OutputFacetIdV1::Color:
    case OutputFacetIdV1::AlphaAssociation:
    case OutputFacetIdV1::Channels:
    case OutputFacetIdV1::DataWindow:
    case OutputFacetIdV1::DisplayWindow:
    case OutputFacetIdV1::PixelAspect:
    case OutputFacetIdV1::Compression:
    case OutputFacetIdV1::Metadata:
    case OutputFacetIdV1::ExternalDependencies:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool isExactNominallyPossible(const OutputPresetV1 preset,
                                                      const OutputFacetIdV1 facet) noexcept {
    if (preset == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
        return true;
    }
    if (preset != OutputPresetV1::PngRgba8SrgbV1) {
        return false;
    }
    switch (facet) {
    case OutputFacetIdV1::Channels:
    case OutputFacetIdV1::DataWindow:
    case OutputFacetIdV1::DisplayWindow:
    case OutputFacetIdV1::PixelAspect:
    case OutputFacetIdV1::Compression:
    case OutputFacetIdV1::Metadata:
        return true;
    case OutputFacetIdV1::Pixels:
    case OutputFacetIdV1::Precision:
    case OutputFacetIdV1::Color:
    case OutputFacetIdV1::AlphaAssociation:
    case OutputFacetIdV1::ExternalDependencies:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool ruleAcceptsPreset(const OutputFacetStableCodeRuleV1& rule,
                                               const OutputPresetV1 preset) noexcept {
    switch (preset) {
    case OutputPresetV1::PngRgba8SrgbV1:
        return rule.validForPng;
    case OutputPresetV1::FlatExrRgba32fLinRec709SceneV1:
        return rule.validForFlatExr;
    }
    return false;
}

[[nodiscard]] constexpr OutputAnalysisReportIssueV1
issue(const OutputAnalysisReportErrorCodeV1 code, const std::size_t facetIndex = 0) noexcept {
    return {.code = code, .facetIndex = facetIndex};
}

[[nodiscard]] constexpr OutputAnalysisReportIssueV1
descriptorIssue(const OutputAnalysisReportErrorCodeV1 code, const std::size_t facetIndex,
                const OutputFacetDescriptorSideV1 side,
                const bloom::output::OutputFacetDescriptorValidation validation) noexcept {
    return {.code = code,
            .facetIndex = facetIndex,
            .descriptorSide = side,
            .descriptorError = validation.error(),
            .descriptorErrorOffset = validation.errorOffset()};
}

struct PixelDescriptorParts final {
    std::string_view height;
    std::string_view width;
};

[[nodiscard]] std::optional<PixelDescriptorParts>
parsePixels(const std::string_view descriptor, const std::string_view sampleType) noexcept {
    constexpr std::string_view prefix = "height=u:";
    constexpr std::string_view packing = ";packing=id:rgba;sample-type=id:";
    constexpr std::string_view widthPrefix = ";width=u:";
    if (!descriptor.starts_with(prefix)) {
        return std::nullopt;
    }
    const auto packingOffset = descriptor.find(packing, prefix.size());
    if (packingOffset == std::string_view::npos) {
        return std::nullopt;
    }
    const auto sampleOffset = packingOffset + packing.size();
    if (!descriptor.substr(sampleOffset).starts_with(sampleType)) {
        return std::nullopt;
    }
    const auto widthOffset = sampleOffset + sampleType.size();
    if (!descriptor.substr(widthOffset).starts_with(widthPrefix)) {
        return std::nullopt;
    }
    const auto width = descriptor.substr(widthOffset + widthPrefix.size());
    const auto height = descriptor.substr(prefix.size(), packingOffset - prefix.size());
    if (height.empty() || width.empty()) {
        return std::nullopt;
    }
    return PixelDescriptorParts{height, width};
}

struct WindowDescriptorParts final {
    std::string_view height;
    std::string_view originX;
    std::string_view originY;
    std::string_view width;
};

[[nodiscard]] std::optional<WindowDescriptorParts>
parseWindow(const std::string_view descriptor) noexcept {
    constexpr std::string_view heightPrefix = "height=u:";
    constexpr std::string_view originXPrefix = ";origin-x=i:";
    constexpr std::string_view originYPrefix = ";origin-y=i:";
    constexpr std::string_view widthPrefix = ";width=u:";
    if (!descriptor.starts_with(heightPrefix)) {
        return std::nullopt;
    }
    const auto originXOffset = descriptor.find(originXPrefix, heightPrefix.size());
    if (originXOffset == std::string_view::npos) {
        return std::nullopt;
    }
    const auto originXValueOffset = originXOffset + originXPrefix.size();
    const auto originYOffset = descriptor.find(originYPrefix, originXValueOffset);
    if (originYOffset == std::string_view::npos) {
        return std::nullopt;
    }
    const auto originYValueOffset = originYOffset + originYPrefix.size();
    const auto widthOffset = descriptor.find(widthPrefix, originYValueOffset);
    if (widthOffset == std::string_view::npos) {
        return std::nullopt;
    }
    const auto widthValueOffset = widthOffset + widthPrefix.size();
    const auto height = descriptor.substr(heightPrefix.size(), originXOffset - heightPrefix.size());
    const auto originX = descriptor.substr(originXValueOffset, originYOffset - originXValueOffset);
    const auto originY = descriptor.substr(originYValueOffset, widthOffset - originYValueOffset);
    const auto width = descriptor.substr(widthValueOffset);
    if (height.empty() || originX.empty() || originY.empty() || width.empty()) {
        return std::nullopt;
    }
    return WindowDescriptorParts{height, originX, originY, width};
}

struct PixelAspectParts final {
    std::uint32_t numerator;
    std::uint32_t denominator;
};

[[nodiscard]] std::optional<std::uint32_t> parseUnsigned32(const std::string_view value) noexcept {
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
               ? std::optional<std::uint32_t>{parsed}
               : std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> parseSigned64(const std::string_view value) noexcept {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
               ? std::optional<std::int64_t>{parsed}
               : std::nullopt;
}

struct NumericWindow final {
    std::uint32_t height;
    std::int64_t originX;
    std::int64_t originY;
    std::uint32_t width;
};

[[nodiscard]] std::optional<NumericWindow>
numericWindow(const WindowDescriptorParts& parts) noexcept {
    const auto height = parseUnsigned32(parts.height);
    const auto originX = parseSigned64(parts.originX);
    const auto originY = parseSigned64(parts.originY);
    const auto width = parseUnsigned32(parts.width);
    if (!height || !originX || !originY || !width || *height == 0 || *width == 0) {
        return std::nullopt;
    }
    return NumericWindow{*height, *originX, *originY, *width};
}

[[nodiscard]] constexpr bool inclusiveAxisFitsSigned32(const std::int64_t origin,
                                                       const std::uint32_t extent) noexcept {
    constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (extent == 0 || origin < minimum || origin > maximum) {
        return false;
    }
    return static_cast<std::uint64_t>(extent - 1U) <= static_cast<std::uint64_t>(maximum - origin);
}

[[nodiscard]] constexpr bool inclusiveWindowFitsSigned32(const NumericWindow& window) noexcept {
    return inclusiveAxisFitsSigned32(window.originX, window.width) &&
           inclusiveAxisFitsSigned32(window.originY, window.height);
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
checkedMultiply(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] std::optional<bool>
exceedsHardResourceLimits(const bloom::output::OutputAnalysisReportV1View report) noexcept {
    const auto& pixelFacet = report.facets[0];
    const auto sourcePixels = parsePixels(pixelFacet.sourceDescriptor, "binary32");
    const std::string_view targetSample =
        report.preset == OutputPresetV1::PngRgba8SrgbV1 ? "uint8" : "binary32";
    const auto targetPixels = parsePixels(pixelFacet.targetDescriptor, targetSample);
    const auto sourceDataWindow = parseWindow(report.facets[5].sourceDescriptor);
    const auto targetDataWindow = parseWindow(report.facets[5].targetDescriptor);
    const auto sourceDisplayWindow = parseWindow(report.facets[6].sourceDescriptor);
    const auto targetDisplayWindow = parseWindow(report.facets[6].targetDescriptor);
    if (!sourcePixels || !targetPixels || !sourceDataWindow || !targetDataWindow ||
        !sourceDisplayWindow || !targetDisplayWindow) {
        return std::nullopt;
    }

    const auto sourceHeight = parseUnsigned32(sourcePixels->height);
    const auto sourceWidth = parseUnsigned32(sourcePixels->width);
    const auto targetHeight = parseUnsigned32(targetPixels->height);
    const auto targetWidth = parseUnsigned32(targetPixels->width);
    const auto sourceData = numericWindow(*sourceDataWindow);
    const auto targetData = numericWindow(*targetDataWindow);
    const auto sourceDisplay = numericWindow(*sourceDisplayWindow);
    const auto targetDisplay = numericWindow(*targetDisplayWindow);
    if (!sourceHeight || !sourceWidth || !targetHeight || !targetWidth || !sourceData ||
        !targetData || !sourceDisplay || !targetDisplay) {
        return std::nullopt;
    }

    const auto pixelCount = checkedMultiply(*sourceWidth, *sourceHeight);
    if (!pixelCount) {
        return true;
    }
    constexpr auto maximumDimension = bloom::output::kOutputAnalysisMaximumDimensionV1;
    const auto windowExceeds = [](const NumericWindow& window) noexcept {
        return window.width > bloom::output::kOutputAnalysisMaximumDimensionV1 ||
               window.height > bloom::output::kOutputAnalysisMaximumDimensionV1;
    };
    return *sourceWidth > maximumDimension || *sourceHeight > maximumDimension ||
           *targetWidth > maximumDimension || *targetHeight > maximumDimension ||
           *pixelCount > bloom::output::kOutputAnalysisMaximumPixelCountV1 ||
           windowExceeds(*sourceData) || windowExceeds(*targetData) ||
           windowExceeds(*sourceDisplay) || windowExceeds(*targetDisplay);
}

[[nodiscard]] std::optional<PixelAspectParts>
parsePixelAspect(const std::string_view descriptor) noexcept {
    constexpr std::string_view denominatorPrefix = "denominator=u:";
    constexpr std::string_view numeratorPrefix = ";numerator=u:";
    if (!descriptor.starts_with(denominatorPrefix)) {
        return std::nullopt;
    }
    const auto numeratorOffset = descriptor.find(numeratorPrefix, denominatorPrefix.size());
    if (numeratorOffset == std::string_view::npos) {
        return std::nullopt;
    }
    const auto denominator = parseUnsigned32(
        descriptor.substr(denominatorPrefix.size(), numeratorOffset - denominatorPrefix.size()));
    const auto numerator =
        parseUnsigned32(descriptor.substr(numeratorOffset + numeratorPrefix.size()));
    if (!numerator || !denominator) {
        return std::nullopt;
    }
    return PixelAspectParts{*numerator, *denominator};
}

[[nodiscard]] constexpr std::uint8_t lowerHexValue(const char value) noexcept {
    return value >= '0' && value <= '9' ? static_cast<std::uint8_t>(value - '0')
                                        : static_cast<std::uint8_t>(value - 'a' + 10);
}

[[nodiscard]] std::optional<std::uint32_t>
parseBinary32Bits(const std::string_view descriptor) noexcept {
    constexpr std::string_view prefix = "value=f32:";
    if (!descriptor.starts_with(prefix) || descriptor.size() != prefix.size() + 8U) {
        return std::nullopt;
    }
    std::uint32_t bits = 0;
    for (const char digit : descriptor.substr(prefix.size())) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return std::nullopt;
        }
        bits = (bits << 4U) | lowerHexValue(digit);
    }
    return bits;
}

struct RoundedBinary32 final {
    std::uint32_t bits;
    bool exact;
};

// Pixel-aspect terms are positive uint32 values, so their ratio is always a normal binary32. This
// integer long-division implementation is independent of host floating-point mode.
[[nodiscard]] std::optional<RoundedBinary32>
roundPositiveRationalToBinary32(const PixelAspectParts value) noexcept {
    if (value.numerator == 0 || value.denominator == 0) {
        return std::nullopt;
    }

    int exponent = 0;
    if (value.numerator >= value.denominator) {
        const auto integral = value.numerator / value.denominator;
        exponent = std::bit_width(integral) - 1;
    } else {
        std::uint64_t scaledNumerator = value.numerator;
        while (scaledNumerator < value.denominator) {
            scaledNumerator <<= 1U;
            --exponent;
        }
    }

    const int scale = 23 - exponent;
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
    std::uint64_t divisor = value.denominator;
    if (scale >= 0) {
        quotient = value.numerator / divisor;
        remainder = value.numerator % divisor;
        for (int bit = 0; bit < scale; ++bit) {
            quotient <<= 1U;
            remainder <<= 1U;
            if (remainder >= divisor) {
                ++quotient;
                remainder -= divisor;
            }
        }
    } else {
        const auto rightShift = static_cast<unsigned int>(-scale);
        if (rightShift >= 32U ||
            divisor > (std::numeric_limits<std::uint64_t>::max() >> rightShift)) {
            return std::nullopt;
        }
        divisor <<= rightShift;
        quotient = value.numerator / divisor;
        remainder = value.numerator % divisor;
    }

    const bool exact = remainder == 0;
    const auto twiceRemainder = remainder * 2U;
    if (twiceRemainder > divisor || (twiceRemainder == divisor && (quotient & 1U) != 0)) {
        ++quotient;
    }
    if (quotient == (std::uint64_t{1} << 24U)) {
        quotient >>= 1U;
        ++exponent;
    }
    if (exponent < -126 || exponent > 127 || quotient < (std::uint64_t{1} << 23U) ||
        quotient >= (std::uint64_t{1} << 24U)) {
        return std::nullopt;
    }
    const auto biasedExponent = static_cast<std::uint32_t>(exponent + 127);
    const auto fraction = static_cast<std::uint32_t>(quotient - (std::uint64_t{1} << 23U));
    return RoundedBinary32{(biasedExponent << 23U) | fraction, exact};
}

[[nodiscard]] bool isLowerHexRevision(const std::string_view descriptor) noexcept {
    constexpr std::string_view prefix = "kind=id:ocio;revision=id:";
    if (!descriptor.starts_with(prefix) || descriptor.size() != prefix.size() + 64U) {
        return false;
    }
    for (const char value : descriptor.substr(prefix.size())) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

enum class VocabularyValidation : std::uint8_t {
    Valid,
    VocabularyMismatch,
    RelationshipMismatch,
};

[[nodiscard]] VocabularyValidation
validateVocabulary(const bloom::output::OutputAnalysisReportV1View report,
                   const std::size_t facetIndex) noexcept {
    constexpr std::string_view channels =
        "count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
        "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha";
    constexpr std::string_view sourceAlpha =
        "association=id:premultiplied;zero-alpha=id:canonical-zero";
    constexpr std::string_view pngAlpha = "association=id:straight;zero-alpha=id:canonical-zero";
    constexpr std::string_view noDependencies = "kind=id:none;revision=id:none";

    const auto& facet = report.facets[facetIndex];
    const auto& pixelFacet = report.facets[0];
    const auto sourcePixels = parsePixels(pixelFacet.sourceDescriptor, "binary32");
    const std::string_view targetSample =
        report.preset == OutputPresetV1::PngRgba8SrgbV1 ? "uint8" : "binary32";
    const auto targetPixels = parsePixels(pixelFacet.targetDescriptor, targetSample);

    switch (facet.facet) {
    case OutputFacetIdV1::Pixels:
        if (!sourcePixels || !targetPixels) {
            return VocabularyValidation::VocabularyMismatch;
        }
        if (!parseUnsigned32(sourcePixels->height) || !parseUnsigned32(sourcePixels->width) ||
            sourcePixels->height == "0" || sourcePixels->width == "0") {
            return VocabularyValidation::VocabularyMismatch;
        }
        return sourcePixels->height == targetPixels->height &&
                       sourcePixels->width == targetPixels->width
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::RelationshipMismatch;
    case OutputFacetIdV1::Precision:
        if (facet.sourceDescriptor != "component-type=id:binary32") {
            return VocabularyValidation::VocabularyMismatch;
        }
        return facet.targetDescriptor == (report.preset == OutputPresetV1::PngRgba8SrgbV1
                                              ? "component-type=id:uint8"
                                              : "component-type=id:binary32")
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::Color:
        if (facet.sourceDescriptor != "color-id=id:lin_rec709_scene") {
            return VocabularyValidation::VocabularyMismatch;
        }
        return facet.targetDescriptor == (report.preset == OutputPresetV1::PngRgba8SrgbV1
                                              ? "color-id=id:srgb_rec709_display"
                                              : "color-id=id:lin_rec709_scene")
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::AlphaAssociation:
        if (facet.sourceDescriptor != sourceAlpha) {
            return VocabularyValidation::VocabularyMismatch;
        }
        return facet.targetDescriptor ==
                       (report.preset == OutputPresetV1::PngRgba8SrgbV1 ? pngAlpha : sourceAlpha)
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::Channels:
        return facet.sourceDescriptor == channels && facet.targetDescriptor == channels
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::DataWindow:
    case OutputFacetIdV1::DisplayWindow: {
        if (!sourcePixels || !targetPixels) {
            return VocabularyValidation::RelationshipMismatch;
        }
        const auto sourceWindow = parseWindow(facet.sourceDescriptor);
        const auto targetWindow = parseWindow(facet.targetDescriptor);
        if (!sourceWindow || !targetWindow) {
            return VocabularyValidation::VocabularyMismatch;
        }
        const auto sourceNumericWindow = numericWindow(*sourceWindow);
        if (!sourceNumericWindow) {
            return VocabularyValidation::VocabularyMismatch;
        }
        if (facet.facet == OutputFacetIdV1::DataWindow &&
            (sourceWindow->height != sourcePixels->height ||
             sourceWindow->width != sourcePixels->width)) {
            return VocabularyValidation::RelationshipMismatch;
        }
        if (report.preset == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
            if (facet.sourceDescriptor != facet.targetDescriptor) {
                return VocabularyValidation::RelationshipMismatch;
            }
            const auto requiredCode = inclusiveWindowFitsSigned32(*sourceNumericWindow)
                                          ? OutputFacetStableCodeV1::None
                                          : OutputFacetStableCodeV1::WindowOutOfRange;
            return facet.stableCode == requiredCode ? VocabularyValidation::Valid
                                                    : VocabularyValidation::RelationshipMismatch;
        }
        const auto pixelHeight = parseUnsigned32(sourcePixels->height);
        const auto pixelWidth = parseUnsigned32(sourcePixels->width);
        if (!pixelHeight || !pixelWidth || targetWindow->height != targetPixels->height ||
            targetWindow->width != targetPixels->width || targetWindow->originX != "0" ||
            targetWindow->originY != "0") {
            return VocabularyValidation::RelationshipMismatch;
        }
        constexpr std::uint32_t pngMaximumDimension = 2'147'483'647U;
        OutputFacetStableCodeV1 requiredCode = OutputFacetStableCodeV1::None;
        if (*pixelHeight > pngMaximumDimension || *pixelWidth > pngMaximumDimension) {
            requiredCode = OutputFacetStableCodeV1::WindowOutOfRange;
        } else if (facet.sourceDescriptor != facet.targetDescriptor) {
            requiredCode = facet.facet == OutputFacetIdV1::DataWindow
                               ? OutputFacetStableCodeV1::PngOriginWindowRequired
                               : OutputFacetStableCodeV1::PngEqualWindowRequired;
        }
        return facet.stableCode == requiredCode ? VocabularyValidation::Valid
                                                : VocabularyValidation::RelationshipMismatch;
    }
    case OutputFacetIdV1::PixelAspect: {
        const auto source = parsePixelAspect(facet.sourceDescriptor);
        if (!source) {
            return VocabularyValidation::VocabularyMismatch;
        }
        if (report.preset == OutputPresetV1::PngRgba8SrgbV1) {
            if (facet.targetDescriptor != "denominator=u:1;numerator=u:1") {
                return VocabularyValidation::VocabularyMismatch;
            }
            const bool sourceIsSquare = source->numerator == 1 && source->denominator == 1;
            return (facet.stableCode == OutputFacetStableCodeV1::None) == sourceIsSquare
                       ? VocabularyValidation::Valid
                       : VocabularyValidation::RelationshipMismatch;
        }
        const auto targetBits = parseBinary32Bits(facet.targetDescriptor);
        const auto rounded = roundPositiveRationalToBinary32(*source);
        if (!targetBits || !rounded || *targetBits != rounded->bits) {
            return VocabularyValidation::RelationshipMismatch;
        }
        if (facet.stableCode == OutputFacetStableCodeV1::None && !rounded->exact) {
            return VocabularyValidation::RelationshipMismatch;
        }
        if (facet.stableCode == OutputFacetStableCodeV1::ExrParRoundedBinary32 && rounded->exact) {
            return VocabularyValidation::RelationshipMismatch;
        }
        return VocabularyValidation::Valid;
    }
    case OutputFacetIdV1::Compression:
        if (!facet.sourceDescriptor.empty()) {
            return VocabularyValidation::VocabularyMismatch;
        }
        return facet.targetDescriptor == (report.preset == OutputPresetV1::PngRgba8SrgbV1
                                              ? "method=id:deflate-level-6-filter-none"
                                              : "method=id:zip")
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::Metadata:
        return facet.sourceDescriptor == "profile=id:none" &&
                       facet.targetDescriptor == "profile=id:none"
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    case OutputFacetIdV1::ExternalDependencies:
        if (facet.sourceDescriptor != noDependencies) {
            return VocabularyValidation::VocabularyMismatch;
        }
        if (report.preset == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
            return facet.targetDescriptor == noDependencies
                       ? VocabularyValidation::Valid
                       : VocabularyValidation::VocabularyMismatch;
        }
        return isLowerHexRevision(facet.targetDescriptor)
                   ? VocabularyValidation::Valid
                   : VocabularyValidation::VocabularyMismatch;
    }
    return VocabularyValidation::VocabularyMismatch;
}

} // namespace

namespace bloom::output {

OutputAnalysisReportValidationV1
validateOutputAnalysisReportV1(const OutputAnalysisReportV1View report) noexcept {
    if (!outputPresetIdentityV1(report.preset)) {
        return OutputAnalysisReportValidationV1::failure(
            issue(OutputAnalysisReportErrorCodeV1::InvalidPreset));
    }
    if (report.facets.size() != kOutputAnalysisFacetCountV1) {
        return OutputAnalysisReportValidationV1::failure(
            issue(OutputAnalysisReportErrorCodeV1::IncorrectFacetCount, report.facets.size()));
    }

    std::uint16_t permissionBits = 0;
    for (std::size_t index = 0; index < report.facets.size(); ++index) {
        const auto& facet = report.facets[index];
        if (!isKnownFacet(facet.facet)) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::InvalidFacet, index));
        }
        const auto expectedFacet = static_cast<OutputFacetIdV1>(index + 1U);
        if (facet.facet != expectedFacet) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::FacetOutOfOrder, index));
        }
        if (!isKnownState(facet.state)) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::InvalidState, index));
        }

        const auto rule = outputFacetStableCodeRuleV1(facet.stableCode);
        if (!rule) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::InvalidStableCode, index));
        }
        if (!rule->appliesToFacet(facet.facet)) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::StableCodeFacetMismatch, index));
        }
        if (!ruleAcceptsPreset(*rule, report.preset)) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::StableCodePresetMismatch, index));
        }
        if (facet.state != rule->requiredState) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::StateMismatch, index));
        }
        if (facet.stableCode == OutputFacetStableCodeV1::None &&
            !isExactNominallyPossible(report.preset, facet.facet)) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::NominalTupleMismatch, index));
        }

        const auto schemas = outputFacetDescriptorSchemasV1(report.preset, facet.facet);
        if (!schemas) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::InternalInvariant, index));
        }
        const auto sourceValidation =
            validateOutputFacetDescriptorV1(schemas->source, facet.sourceDescriptor);
        if (!sourceValidation) {
            return OutputAnalysisReportValidationV1::failure(
                descriptorIssue(OutputAnalysisReportErrorCodeV1::SourceDescriptorInvalid, index,
                                OutputFacetDescriptorSideV1::Source, sourceValidation));
        }
        const auto targetValidation =
            validateOutputFacetDescriptorV1(schemas->target, facet.targetDescriptor);
        if (!targetValidation) {
            return OutputAnalysisReportValidationV1::failure(
                descriptorIssue(OutputAnalysisReportErrorCodeV1::TargetDescriptorInvalid, index,
                                OutputFacetDescriptorSideV1::Target, targetValidation));
        }
        if (facet.state == OutputPreservationStateV1::Exact && schemas->source == schemas->target &&
            facet.sourceDescriptor != facet.targetDescriptor) {
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::DescriptorRelationshipMismatch, index));
        }

        switch (validateVocabulary(report, index)) {
        case VocabularyValidation::Valid:
            break;
        case VocabularyValidation::VocabularyMismatch:
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::DescriptorVocabularyMismatch, index));
        case VocabularyValidation::RelationshipMismatch:
            return OutputAnalysisReportValidationV1::failure(
                issue(OutputAnalysisReportErrorCodeV1::DescriptorRelationshipMismatch, index));
        }
        if (rule->presetPermits) {
            permissionBits |= facetBit(facet.facet);
        }
    }

    const auto resourceLimitExceeded = exceedsHardResourceLimits(report);
    if (!resourceLimitExceeded) {
        return OutputAnalysisReportValidationV1::failure(
            issue(OutputAnalysisReportErrorCodeV1::InternalInvariant,
                  static_cast<std::size_t>(OutputFacetIdV1::ExternalDependencies) - 1U));
    }
    const auto resourceCodePresent =
        report.facets[static_cast<std::size_t>(OutputFacetIdV1::ExternalDependencies) - 1U]
            .stableCode == OutputFacetStableCodeV1::ResourceLimitExceeded;
    if (resourceCodePresent != *resourceLimitExceeded) {
        return OutputAnalysisReportValidationV1::failure(
            issue(OutputAnalysisReportErrorCodeV1::DescriptorRelationshipMismatch,
                  static_cast<std::size_t>(OutputFacetIdV1::ExternalDependencies) - 1U));
    }

    return OutputAnalysisReportValidationV1::success(permissionBits);
}

} // namespace bloom::output
