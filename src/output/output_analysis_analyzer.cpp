#include <bloom/output/output_analysis_analyzer.hpp>

#include "output_analysis_analyzer_test_access.hpp"
#include "output_analysis_numeric.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace core = bloom::core;
namespace detail = bloom::output::detail;
namespace output = bloom::output;
namespace render = bloom::render;

using AnalyzerError = output::OutputAnalysisAnalyzerErrorCodeV1;
using Code = output::OutputFacetStableCodeV1;
using Facet = output::OutputFacetIdV1;
using Fault = detail::OutputAnalysisAnalyzerFaultV1;
using Preset = output::OutputPresetV1;

constexpr std::string_view kSourcePrecision = "component-type=id:binary32";
constexpr std::string_view kSourceColor = "color-id=id:lin_rec709_scene";
constexpr std::string_view kSourceAlpha =
    "association=id:premultiplied;zero-alpha=id:canonical-zero";
constexpr std::string_view kChannels =
    "count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
    "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha";
constexpr std::string_view kMetadata = "profile=id:none";
constexpr std::string_view kNoDependencies = "kind=id:none;revision=id:none";

template <typename Integer> void appendDecimal(std::string& destination, const Integer value) {
    std::array<char, std::numeric_limits<Integer>::digits10 + 4> storage{};
    const auto converted = std::to_chars(storage.data(), storage.data() + storage.size(), value);
    if (converted.ec != std::errc{}) {
        throw std::length_error("output analysis decimal conversion failed");
    }
    destination.append(storage.data(), converted.ptr);
}

[[nodiscard]] std::string pixelsDescriptor(const render::ImageExtent extent,
                                           const std::string_view sampleType) {
    std::string descriptor = "height=u:";
    appendDecimal(descriptor, extent.height());
    descriptor.append(";packing=id:rgba;sample-type=id:");
    descriptor.append(sampleType);
    descriptor.append(";width=u:");
    appendDecimal(descriptor, extent.width());
    return descriptor;
}

[[nodiscard]] std::string windowDescriptor(const render::ImageWindow window) {
    std::string descriptor = "height=u:";
    appendDecimal(descriptor, window.extent().height());
    descriptor.append(";origin-x=i:");
    appendDecimal(descriptor, window.originX());
    descriptor.append(";origin-y=i:");
    appendDecimal(descriptor, window.originY());
    descriptor.append(";width=u:");
    appendDecimal(descriptor, window.extent().width());
    return descriptor;
}

[[nodiscard]] std::string pixelAspectDescriptor(const core::PixelAspectRatio value) {
    std::string descriptor = "denominator=u:";
    appendDecimal(descriptor, value.denominator());
    descriptor.append(";numerator=u:");
    appendDecimal(descriptor, value.numerator());
    return descriptor;
}

[[nodiscard]] constexpr char lowerHexDigit(const std::uint8_t value) noexcept {
    return value < 10U ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10U));
}

[[nodiscard]] std::string binary32Descriptor(const std::uint32_t bits) {
    std::string descriptor = "value=f32:";
    for (unsigned shift = 28U;; shift -= 4U) {
        descriptor.push_back(lowerHexDigit(static_cast<std::uint8_t>((bits >> shift) & 0xFU)));
        if (shift == 0U) {
            break;
        }
    }
    return descriptor;
}

[[nodiscard]] std::string ocioDependencyDescriptor(const core::Sha256Digest& revision) {
    const auto hex = revision.toLowercaseHex();
    std::string descriptor = "kind=id:ocio;revision=id:";
    descriptor.append(hex.data(), hex.size());
    return descriptor;
}

[[nodiscard]] constexpr bool known(const output::OutputAnalysisAdapterStateV1 state) noexcept {
    switch (state) {
    case output::OutputAnalysisAdapterStateV1::Qualified:
    case output::OutputAnalysisAdapterStateV1::Unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known(const output::OutputAnalysisCompressionStateV1 state) noexcept {
    switch (state) {
    case output::OutputAnalysisCompressionStateV1::Available:
    case output::OutputAnalysisCompressionStateV1::Unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool
known(const output::OutputAnalysisOtherDependencyStateV1 state) noexcept {
    switch (state) {
    case output::OutputAnalysisOtherDependencyStateV1::Available:
    case output::OutputAnalysisOtherDependencyStateV1::Missing:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool
known(const output::PngRgba8SrgbColorResolutionStateV1 state) noexcept {
    switch (state) {
    case output::PngRgba8SrgbColorResolutionStateV1::Ready:
    case output::PngRgba8SrgbColorResolutionStateV1::Missing:
    case output::PngRgba8SrgbColorResolutionStateV1::Changed:
    case output::PngRgba8SrgbColorResolutionStateV1::Invalid:
    case output::PngRgba8SrgbColorResolutionStateV1::MissingResource:
    case output::PngRgba8SrgbColorResolutionStateV1::UnsupportedVersion:
        return true;
    }
    return false;
}

struct SourceDescriptorResult final {
    std::optional<render::Rgba32fImageDescriptor> descriptor;
    bool processReady = false;
    AnalyzerError error = AnalyzerError::InternalInvariant;
};

[[nodiscard]] SourceDescriptorResult
resolveSourceDescriptor(const output::OutputAnalysisProcessSourceV1& process) noexcept {
    switch (process.state) {
    case output::OutputAnalysisProcessSourceStateV1::Ready: {
        if (process.readyIdentity == nullptr || process.missingDescriptor.has_value()) {
            return {.descriptor = std::nullopt,
                    .processReady = false,
                    .error = AnalyzerError::InvalidProcessSource};
        }
        const auto& frameOwner = process.readyIdentity->processFrame();
        if (frameOwner == nullptr || !frameOwner->processImage().isValid()) {
            return {.descriptor = std::nullopt,
                    .processReady = false,
                    .error = AnalyzerError::InvalidSourceDescriptor};
        }
        const auto* descriptor = frameOwner->processImage().descriptor();
        if (descriptor == nullptr) {
            return {.descriptor = std::nullopt,
                    .processReady = false,
                    .error = AnalyzerError::InvalidSourceDescriptor};
        }
        return {.descriptor = *descriptor, .processReady = true, .error = AnalyzerError::None};
    }
    case output::OutputAnalysisProcessSourceStateV1::Missing:
        if (process.readyIdentity != nullptr || !process.missingDescriptor.has_value()) {
            return {.descriptor = std::nullopt,
                    .processReady = false,
                    .error = AnalyzerError::InvalidProcessSource};
        }
        return {.descriptor = process.missingDescriptor,
                .processReady = false,
                .error = AnalyzerError::None};
    }
    return {.descriptor = std::nullopt,
            .processReady = false,
            .error = AnalyzerError::InvalidProcessSourceState};
}

[[nodiscard]] constexpr Code
pngColorCode(const output::PngRgba8SrgbColorResolutionStateV1 state) noexcept {
    switch (state) {
    case output::PngRgba8SrgbColorResolutionStateV1::Ready:
        return Code::PngLinRec709SceneToSrgb;
    case output::PngRgba8SrgbColorResolutionStateV1::Missing:
        return Code::OcioMissing;
    case output::PngRgba8SrgbColorResolutionStateV1::Changed:
        return Code::OcioChanged;
    case output::PngRgba8SrgbColorResolutionStateV1::Invalid:
        return Code::OcioInvalid;
    case output::PngRgba8SrgbColorResolutionStateV1::MissingResource:
        return Code::OcioResourceMissing;
    case output::PngRgba8SrgbColorResolutionStateV1::UnsupportedVersion:
        return Code::OcioVersionUnsupported;
    }
    return Code::ColorUnsupported;
}

[[nodiscard]] constexpr bool
exceedsResourceLimits(const render::Rgba32fImageDescriptor& descriptor) noexcept {
    const auto data = descriptor.dataWindow().extent();
    const auto display = descriptor.displayWindow().extent();
    const auto pixelCount = static_cast<std::uint64_t>(data.width()) * data.height();
    return data.width() > output::kOutputAnalysisMaximumDimensionV1 ||
           data.height() > output::kOutputAnalysisMaximumDimensionV1 ||
           display.width() > output::kOutputAnalysisMaximumDimensionV1 ||
           display.height() > output::kOutputAnalysisMaximumDimensionV1 ||
           pixelCount > output::kOutputAnalysisMaximumPixelCountV1;
}

[[nodiscard]] constexpr bool fitsSigned32(const render::ImageWindow window) noexcept {
    return detail::outputAnalysisInclusiveWindowFitsSigned32V1({.height = window.extent().height(),
                                                                .originX = window.originX(),
                                                                .originY = window.originY(),
                                                                .width = window.extent().width()});
}

template <typename Assessment, typename Source, typename Target>
[[nodiscard]] bool setAssessment(Assessment& assessment, const Preset preset, const Facet facet,
                                 const Code code, Source&& source, Target&& target) {
    const auto rule = output::outputFacetStableCodeRuleV1(code);
    if (!rule || !rule->appliesToFacet(facet) ||
        (preset == Preset::PngRgba8SrgbV1 ? !rule->validForPng : !rule->validForFlatExr)) {
        return false;
    }
    assessment = {.facet = facet,
                  .state = rule->requiredState,
                  .stableCode = code,
                  .sourceDescriptor = std::forward<Source>(source),
                  .targetDescriptor = std::forward<Target>(target)};
    return true;
}

template <typename Assessments>
[[nodiscard]] bool descriptorsAreAsciiAndBounded(const Assessments& assessments, std::size_t& total,
                                                 AnalyzerError& error) noexcept {
    total = 0;
    for (const auto& assessment : assessments) {
        for (const auto* descriptor :
             std::array{&assessment.sourceDescriptor, &assessment.targetDescriptor}) {
            if (descriptor->size() > output::kOutputFacetDescriptorV1MaximumBytes) {
                error = AnalyzerError::DescriptorTooLong;
                return false;
            }
            if (descriptor->size() >
                output::kOutputAnalysisReportDescriptorStorageMaximumBytesV1 - total) {
                error = AnalyzerError::DescriptorStorageTooLarge;
                return false;
            }
            if (std::ranges::any_of(*descriptor, [](const char byte) {
                    return byte <= 0 || static_cast<unsigned char>(byte) > 0x7FU;
                })) {
                error = AnalyzerError::InternalInvariant;
                return false;
            }
            total += descriptor->size();
        }
    }
    error = AnalyzerError::None;
    return true;
}

struct CommonInput final {
    output::OutputAnalysisProcessSourceV1 process;
    output::OutputAnalysisAdapterStateV1 adapter;
    output::OutputAnalysisCompressionStateV1 compression;
    output::OutputAnalysisOtherDependencyStateV1 otherDependency;
    std::optional<core::Sha256Digest> expectedOcioRevision;
    std::optional<output::PngRgba8SrgbColorResolutionStateV1> colorResolution;
};

} // namespace

namespace bloom::output {

OutputAnalysisReportV1::OutputAnalysisReportV1(
    const OutputPresetV1 preset,
    std::array<OwnedAssessment, kOutputAnalysisFacetCountV1>&& assessments,
    const OutputAnalysisPermissionMaskV1 permissionMask,
    const std::size_t descriptorByteCount) noexcept
    : preset_(preset), assessments_(std::move(assessments)), permissionMask_(permissionMask),
      descriptorByteCount_(descriptorByteCount) {
    bindAssessmentViews();
}

OutputAnalysisReportV1::OutputAnalysisReportV1(OutputAnalysisReportV1&& other) noexcept
    : preset_(other.preset_), assessments_(std::move(other.assessments_)),
      permissionMask_(other.permissionMask_), descriptorByteCount_(other.descriptorByteCount_) {
    bindAssessmentViews();
    other.bindAssessmentViews();
}

void OutputAnalysisReportV1::bindAssessmentViews() noexcept {
    for (std::size_t index = 0; index < assessments_.size(); ++index) {
        const auto& assessment = assessments_[index];
        assessmentViews_[index] = {.facet = assessment.facet,
                                   .state = assessment.state,
                                   .stableCode = assessment.stableCode,
                                   .sourceDescriptor = assessment.sourceDescriptor,
                                   .targetDescriptor = assessment.targetDescriptor};
    }
}

OutputAnalysisAnalyzerResultV1 OutputAnalysisAnalyzerResultV1::success(
    std::shared_ptr<const OutputAnalysisReportV1> report) noexcept {
    if (report == nullptr) {
        return failure(OutputAnalysisAnalyzerErrorCodeV1::InternalInvariant);
    }
    return {std::move(report), OutputAnalysisAnalyzerErrorCodeV1::None, {}};
}

OutputAnalysisAnalyzerResultV1 OutputAnalysisAnalyzerResultV1::failure(
    const OutputAnalysisAnalyzerErrorCodeV1 error,
    const OutputAnalysisReportIssueV1 generatedReportIssue) noexcept {
    return {{},
            error == OutputAnalysisAnalyzerErrorCodeV1::None
                ? OutputAnalysisAnalyzerErrorCodeV1::InternalInvariant
                : error,
            generatedReportIssue};
}

OutputAnalysisAnalyzerResultV1::OutputAnalysisAnalyzerResultV1(
    std::shared_ptr<const OutputAnalysisReportV1> report,
    const OutputAnalysisAnalyzerErrorCodeV1 error,
    const OutputAnalysisReportIssueV1 generatedReportIssue) noexcept
    : report_(std::move(report)), error_(error), generatedReportIssue_(generatedReportIssue) {}

namespace detail {

class OutputAnalysisAnalyzerV1 final {
  public:
    [[nodiscard]] static OutputAnalysisAnalyzerResultV1
    analyze(const OutputPresetV1 preset, CommonInput input, const Fault fault) noexcept {
        if (!known(input.adapter)) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InvalidAdapterState);
        }
        if (!known(input.compression)) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InvalidCompressionState);
        }
        if (!known(input.otherDependency)) {
            return OutputAnalysisAnalyzerResultV1::failure(
                AnalyzerError::InvalidOtherDependencyState);
        }
        const bool png = preset == OutputPresetV1::PngRgba8SrgbV1;
        if (png) {
            if (!input.expectedOcioRevision.has_value() || !input.colorResolution.has_value() ||
                !known(*input.colorResolution)) {
                return OutputAnalysisAnalyzerResultV1::failure(
                    AnalyzerError::InvalidColorResolutionState);
            }
        } else if (preset != OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 ||
                   input.expectedOcioRevision.has_value() || input.colorResolution.has_value()) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InternalInvariant);
        }

        const auto source = resolveSourceDescriptor(input.process);
        if (!source.descriptor.has_value()) {
            return OutputAnalysisAnalyzerResultV1::failure(source.error);
        }
        if (fault == Fault::AllocationFailure) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::AllocationFailure);
        }

        try {
            const auto& descriptor = *source.descriptor;
            const auto dataWindow = descriptor.dataWindow();
            const auto displayWindow = descriptor.displayWindow();
            const auto dataExtent = dataWindow.extent();
            const auto pixelAspect = descriptor.pixelAspect();
            const auto sourcePixels = pixelsDescriptor(dataExtent, "binary32");
            const auto sourceDataWindow = windowDescriptor(dataWindow);
            const auto sourceDisplayWindow = windowDescriptor(displayWindow);
            const auto sourcePixelAspect = pixelAspectDescriptor(pixelAspect);

            std::array<OutputAnalysisReportV1::OwnedAssessment, kOutputAnalysisFacetCountV1>
                assessments;
            const auto pixelCode =
                !source.processReady ? Code::ProcessFrameMissing
                                     : (png ? Code::PngDisplayTransformClampQuantize : Code::None);
            const auto colorCode = png ? pngColorCode(*input.colorResolution) : Code::None;
            const auto compressionCode =
                input.compression == OutputAnalysisCompressionStateV1::Available
                    ? Code::None
                    : Code::CompressionUnavailable;

            const auto pngTargetWindow =
                render::ImageWindow::create(0, 0, dataExtent.width(), dataExtent.height());
            if (!pngTargetWindow) {
                return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InternalInvariant);
            }
            const auto targetDataWindow =
                png ? windowDescriptor(*pngTargetWindow.value()) : sourceDataWindow;
            const auto targetDisplayWindow =
                png ? windowDescriptor(*pngTargetWindow.value()) : sourceDisplayWindow;

            Code dataWindowCode = Code::None;
            Code displayWindowCode = Code::None;
            if (png) {
                if (dataExtent.width() > detail::kOutputAnalysisPngMaximumDimensionV1 ||
                    dataExtent.height() > detail::kOutputAnalysisPngMaximumDimensionV1) {
                    dataWindowCode = Code::WindowOutOfRange;
                    displayWindowCode = Code::WindowOutOfRange;
                } else {
                    if (sourceDataWindow != targetDataWindow) {
                        dataWindowCode = Code::PngOriginWindowRequired;
                    }
                    if (sourceDisplayWindow != targetDisplayWindow) {
                        displayWindowCode = Code::PngEqualWindowRequired;
                    }
                }
            } else {
                dataWindowCode = fitsSigned32(dataWindow) ? Code::None : Code::WindowOutOfRange;
                displayWindowCode =
                    fitsSigned32(displayWindow) ? Code::None : Code::WindowOutOfRange;
            }

            const auto roundedAspect = detail::roundOutputAnalysisPositiveRationalToBinary32V1(
                {.numerator = pixelAspect.numerator(), .denominator = pixelAspect.denominator()});
            if (!roundedAspect) {
                return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InternalInvariant);
            }
            const auto pixelAspectCode =
                png ? (pixelAspect == core::PixelAspectRatio::square()
                           ? Code::None
                           : Code::PngSquarePixelRequired)
                    : (roundedAspect->exact ? Code::None : Code::ExrParRoundedBinary32);

            Code dependencyCode = png ? Code::PngOcioExternalReference : Code::None;
            if (exceedsResourceLimits(descriptor)) {
                dependencyCode = Code::ResourceLimitExceeded;
            } else if (input.adapter == OutputAnalysisAdapterStateV1::Unavailable) {
                dependencyCode = Code::AdapterUnavailable;
            } else if (input.otherDependency == OutputAnalysisOtherDependencyStateV1::Missing ||
                       (png &&
                        *input.colorResolution != PngRgba8SrgbColorResolutionStateV1::Ready)) {
                dependencyCode = Code::DependencyMissing;
            }

            bool valid = true;
            valid = valid &&
                    setAssessment(assessments[0], preset, Facet::Pixels, pixelCode, sourcePixels,
                                  pixelsDescriptor(dataExtent, png ? "uint8" : "binary32"));
            valid = valid && setAssessment(assessments[1], preset, Facet::Precision,
                                           png ? Code::PngFloat32ToUint8 : Code::None,
                                           std::string(kSourcePrecision),
                                           png ? std::string("component-type=id:uint8")
                                               : std::string(kSourcePrecision));
            valid = valid && setAssessment(assessments[2], preset, Facet::Color, colorCode,
                                           std::string(kSourceColor),
                                           png ? std::string("color-id=id:srgb_rec709_display")
                                               : std::string(kSourceColor));
            valid =
                valid &&
                setAssessment(
                    assessments[3], preset, Facet::AlphaAssociation,
                    png ? Code::PngPremultipliedToStraight : Code::None, std::string(kSourceAlpha),
                    png ? std::string("association=id:straight;zero-alpha=id:canonical-zero")
                        : std::string(kSourceAlpha));
            valid = valid && setAssessment(assessments[4], preset, Facet::Channels, Code::None,
                                           std::string(kChannels), std::string(kChannels));
            valid = valid && setAssessment(assessments[5], preset, Facet::DataWindow,
                                           dataWindowCode, sourceDataWindow, targetDataWindow);
            valid =
                valid && setAssessment(assessments[6], preset, Facet::DisplayWindow,
                                       displayWindowCode, sourceDisplayWindow, targetDisplayWindow);
            valid = valid && setAssessment(assessments[7], preset, Facet::PixelAspect,
                                           pixelAspectCode, sourcePixelAspect,
                                           png ? std::string("denominator=u:1;numerator=u:1")
                                               : binary32Descriptor(roundedAspect->bits));
            valid = valid &&
                    setAssessment(assessments[8], preset, Facet::Compression, compressionCode, "",
                                  png ? std::string("method=id:deflate-level-6-filter-none")
                                      : std::string("method=id:zip"));
            valid = valid && setAssessment(assessments[9], preset, Facet::Metadata, Code::None,
                                           std::string(kMetadata), std::string(kMetadata));
            valid =
                valid && setAssessment(assessments[10], preset, Facet::ExternalDependencies,
                                       dependencyCode, std::string(kNoDependencies),
                                       png ? ocioDependencyDescriptor(*input.expectedOcioRevision)
                                           : std::string(kNoDependencies));
            if (!valid) {
                return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InternalInvariant);
            }

            if (fault == Fault::DescriptorTooLong) {
                assessments[9].sourceDescriptor.assign(kOutputFacetDescriptorV1MaximumBytes + 1U,
                                                       'a');
            }
            std::size_t descriptorByteCount = 0;
            AnalyzerError descriptorError = AnalyzerError::InternalInvariant;
            if (!descriptorsAreAsciiAndBounded(assessments, descriptorByteCount, descriptorError)) {
                return OutputAnalysisAnalyzerResultV1::failure(descriptorError);
            }
            if (fault == Fault::DescriptorStorageTooLarge) {
                return OutputAnalysisAnalyzerResultV1::failure(
                    AnalyzerError::DescriptorStorageTooLarge);
            }
            if (fault == Fault::GeneratedReportInvariantViolation) {
                assessments[0].facet = Facet::Precision;
            }

            std::array<OutputFacetAssessmentV1View, kOutputAnalysisFacetCountV1> views;
            for (std::size_t index = 0; index < assessments.size(); ++index) {
                const auto& assessment = assessments[index];
                views[index] = {.facet = assessment.facet,
                                .state = assessment.state,
                                .stableCode = assessment.stableCode,
                                .sourceDescriptor = assessment.sourceDescriptor,
                                .targetDescriptor = assessment.targetDescriptor};
            }
            const auto generatedValidation =
                validateOutputAnalysisReportV1({.preset = preset, .facets = views});
            const auto permissionMask = generatedValidation.permissionMask();
            if (!generatedValidation || !permissionMask.has_value()) {
                return OutputAnalysisAnalyzerResultV1::failure(
                    AnalyzerError::GeneratedReportInvariantViolation, generatedValidation.issue());
            }

            auto report = std::shared_ptr<const OutputAnalysisReportV1>(new OutputAnalysisReportV1(
                preset, std::move(assessments), *permissionMask, descriptorByteCount));
            const auto retainedValidation = validateOutputAnalysisReportV1(report->view());
            const auto retainedMask = retainedValidation.permissionMask();
            if (!retainedValidation || !retainedMask.has_value() ||
                retainedMask->bits() != report->permissionMask().bits()) {
                return OutputAnalysisAnalyzerResultV1::failure(
                    AnalyzerError::GeneratedReportInvariantViolation, retainedValidation.issue());
            }
            return OutputAnalysisAnalyzerResultV1::success(std::move(report));
        } catch (const std::bad_alloc&) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::AllocationFailure);
        } catch (const std::length_error&) {
            return OutputAnalysisAnalyzerResultV1::failure(AnalyzerError::InternalInvariant);
        }
    }
};

OutputAnalysisAnalyzerResultV1
analyzePngRgba8SrgbV1WithFaultForTest(PngRgba8SrgbAnalysisInputV1 input,
                                      const OutputAnalysisAnalyzerFaultV1 fault) noexcept {
    return OutputAnalysisAnalyzerV1::analyze(OutputPresetV1::PngRgba8SrgbV1,
                                             {.process = std::move(input.process),
                                              .adapter = input.adapter,
                                              .compression = input.compression,
                                              .otherDependency = input.otherDependency,
                                              .expectedOcioRevision = input.expectedOcioRevision,
                                              .colorResolution = input.colorResolution},
                                             fault);
}

OutputAnalysisAnalyzerResultV1 analyzeFlatExrRgba32fLinRec709SceneV1WithFaultForTest(
    FlatExrRgba32fLinRec709SceneAnalysisInputV1 input,
    const OutputAnalysisAnalyzerFaultV1 fault) noexcept {
    return OutputAnalysisAnalyzerV1::analyze(OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                                             {.process = std::move(input.process),
                                              .adapter = input.adapter,
                                              .compression = input.compression,
                                              .otherDependency = input.otherDependency,
                                              .expectedOcioRevision = std::nullopt,
                                              .colorResolution = std::nullopt},
                                             fault);
}

} // namespace detail

OutputAnalysisAnalyzerResultV1 analyzePngRgba8SrgbV1(PngRgba8SrgbAnalysisInputV1 input) noexcept {
    return detail::analyzePngRgba8SrgbV1WithFaultForTest(
        std::move(input), detail::OutputAnalysisAnalyzerFaultV1::None);
}

OutputAnalysisAnalyzerResultV1
analyzeFlatExrRgba32fLinRec709SceneV1(FlatExrRgba32fLinRec709SceneAnalysisInputV1 input) noexcept {
    return detail::analyzeFlatExrRgba32fLinRec709SceneV1WithFaultForTest(
        std::move(input), detail::OutputAnalysisAnalyzerFaultV1::None);
}

} // namespace bloom::output
