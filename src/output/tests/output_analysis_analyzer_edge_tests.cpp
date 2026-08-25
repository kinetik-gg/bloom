#include "output_analysis_analyzer_test_support.hpp"

#include "output_analysis_analyzer_test_access.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace bloom::output::test {

namespace {

using AnalyzerError = OutputAnalysisAnalyzerErrorCodeV1;
using Code = OutputFacetStableCodeV1;
using Facet = OutputFacetIdV1;
using Fault = detail::OutputAnalysisAnalyzerFaultV1;
using State = OutputPreservationStateV1;

void expectCode(Expectations& expectations, const OutputAnalysisAnalyzerResultV1& result,
                const Facet facetId, const State state, const Code code,
                const std::string_view message) {
    expectations.expect(result && result.report() != nullptr &&
                            facet(*result.report(), facetId).state == state &&
                            facet(*result.report(), facetId).stableCode == code,
                        message);
}

void testClosedCapabilityStatesAndPrecedence(Expectations& expectations) {
    auto compressionInput = exrInput(descriptor());
    compressionInput.compression = OutputAnalysisCompressionStateV1::Unavailable;
    const auto compression = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(compressionInput));
    expectCode(expectations, compression, Facet::Compression, State::Missing,
               Code::CompressionUnavailable,
               "compression Unavailable changes the Compression facet");
    expectCode(expectations, compression, Facet::ExternalDependencies, State::Exact, Code::None,
               "compression availability is isolated from external dependencies");

    auto dependencyInput = exrInput(descriptor());
    dependencyInput.otherDependency = OutputAnalysisOtherDependencyStateV1::Missing;
    const auto dependency = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(dependencyInput));
    expectCode(expectations, dependency, Facet::ExternalDependencies, State::Missing,
               Code::DependencyMissing, "a missing output dependency maps to dependency.missing");

    auto adapterInput = exrInput(descriptor());
    adapterInput.adapter = OutputAnalysisAdapterStateV1::Unavailable;
    adapterInput.otherDependency = OutputAnalysisOtherDependencyStateV1::Missing;
    const auto adapter = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(adapterInput));
    expectCode(expectations, adapter, Facet::ExternalDependencies, State::Missing,
               Code::AdapterUnavailable,
               "adapter.unavailable takes precedence over dependency.missing");

    auto resourceInput = exrInput(descriptor(32'769, 1));
    resourceInput.adapter = OutputAnalysisAdapterStateV1::Unavailable;
    resourceInput.otherDependency = OutputAnalysisOtherDependencyStateV1::Missing;
    const auto resource = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(resourceInput));
    expectCode(expectations, resource, Facet::ExternalDependencies, State::Missing,
               Code::ResourceLimitExceeded,
               "resource.limit-exceeded takes precedence over adapter and dependencies");
}

void testPngColorResolutionStates(Expectations& expectations) {
    struct Fixture final {
        PngRgba8SrgbColorResolutionStateV1 state;
        Code code;
    };
    constexpr std::array fixtures{
        Fixture{PngRgba8SrgbColorResolutionStateV1::Ready, Code::PngLinRec709SceneToSrgb},
        Fixture{PngRgba8SrgbColorResolutionStateV1::Missing, Code::OcioMissing},
        Fixture{PngRgba8SrgbColorResolutionStateV1::Changed, Code::OcioChanged},
        Fixture{PngRgba8SrgbColorResolutionStateV1::Invalid, Code::OcioInvalid},
        Fixture{PngRgba8SrgbColorResolutionStateV1::MissingResource, Code::OcioResourceMissing},
        Fixture{PngRgba8SrgbColorResolutionStateV1::UnsupportedVersion,
                Code::OcioVersionUnsupported},
    };
    for (const auto& fixture : fixtures) {
        auto input = pngInput(descriptor());
        input.colorResolution = fixture.state;
        const auto result = analyzePngRgba8SrgbV1(std::move(input));
        const auto expectedState = fixture.state == PngRgba8SrgbColorResolutionStateV1::Ready
                                       ? State::Approximated
                                       : State::Missing;
        expectCode(expectations, result, Facet::Color, expectedState, fixture.code,
                   "every closed PNG color-resolution state maps one-to-one");
        expectCode(expectations, result, Facet::ExternalDependencies,
                   fixture.state == PngRgba8SrgbColorResolutionStateV1::Ready
                       ? State::ExternalReference
                       : State::Missing,
                   fixture.state == PngRgba8SrgbColorResolutionStateV1::Ready
                       ? Code::PngOcioExternalReference
                       : Code::DependencyMissing,
                   "non-ready PNG color resolution contributes dependency.missing");
    }

    auto adapterInput = pngInput(descriptor());
    adapterInput.colorResolution = PngRgba8SrgbColorResolutionStateV1::Changed;
    adapterInput.adapter = OutputAnalysisAdapterStateV1::Unavailable;
    const auto adapter = analyzePngRgba8SrgbV1(std::move(adapterInput));
    expectCode(expectations, adapter, Facet::Color, State::Missing, Code::OcioChanged,
               "adapter failure does not erase the exact OCIO resolution failure");
    expectCode(expectations, adapter, Facet::ExternalDependencies, State::Missing,
               Code::AdapterUnavailable,
               "adapter failure wins only external-dependency aggregation");
}

void testWindowAndPixelAspectDerivation(Expectations& expectations) {
    const auto translated = window(1, -2, 1, 1);
    const auto translatedDescriptor = descriptor(translated, translated);
    const auto pngTranslated = analyzePngRgba8SrgbV1(pngInput(translatedDescriptor));
    expectCode(expectations, pngTranslated, Facet::DataWindow, State::Unsupported,
               Code::PngOriginWindowRequired,
               "PNG rejects a translated source data window with its exact code");
    expectCode(expectations, pngTranslated, Facet::DisplayWindow, State::Unsupported,
               Code::PngEqualWindowRequired,
               "PNG compares display window with the implicit zero-origin target");

    const auto splitDescriptor = descriptor(window(0, 0, 1, 1), window(0, 0, 2, 1));
    const auto split = analyzePngRgba8SrgbV1(pngInput(splitDescriptor));
    expectCode(expectations, split, Facet::DataWindow, State::Exact, Code::None,
               "PNG data-window equality is independent of display-window equality");
    expectCode(expectations, split, Facet::DisplayWindow, State::Unsupported,
               Code::PngEqualWindowRequired,
               "a distinct PNG display extent remains independently unsupported");

    constexpr std::uint32_t pngSignedDimensionMaximum = 2'147'483'647U;
    const auto oversizedPng =
        analyzePngRgba8SrgbV1(pngInput(descriptor(pngSignedDimensionMaximum + 1U, 1)));
    expectCode(expectations, oversizedPng, Facet::DataWindow, State::Unsupported,
               Code::WindowOutOfRange,
               "a PNG data extent above signed 32-bit range uses window.out-of-range");
    expectCode(expectations, oversizedPng, Facet::DisplayWindow, State::Unsupported,
               Code::WindowOutOfRange,
               "the PNG implicit display target shares the signed extent limit");

    const auto int32Maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    const auto oneAtMaximum = window(int32Maximum, 0, 1, 1);
    const auto exactExr =
        analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(oneAtMaximum, oneAtMaximum)));
    expectCode(expectations, exactExr, Facet::DataWindow, State::Exact, Code::None,
               "an inclusive EXR bound exactly at INT32_MAX is representable");
    const auto twoAtMaximum = window(int32Maximum, 0, 2, 1);
    const auto outOfRange =
        analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(twoAtMaximum, twoAtMaximum)));
    expectCode(expectations, outOfRange, Facet::DataWindow, State::Unsupported,
               Code::WindowOutOfRange, "an EXR inclusive bound one past INT32_MAX is unsupported");

    const auto oneThird = core::PixelAspectRatio::create(1, 3);
    const auto oneHalf = core::PixelAspectRatio::create(1, 2);
    if (!oneThird || !oneHalf) {
        std::abort();
    }
    const auto squareWindow = window(0, 0, 1, 1);
    const auto rounded = analyzeFlatExrRgba32fLinRec709SceneV1(
        exrInput(descriptor(squareWindow, squareWindow, *oneThird)));
    expectCode(expectations, rounded, Facet::PixelAspect, State::Approximated,
               Code::ExrParRoundedBinary32,
               "EXR reports a non-binary rational as permitted approximation");
    expectations.expect(rounded && rounded.report() != nullptr &&
                            facet(*rounded.report(), Facet::PixelAspect).targetDescriptor ==
                                "value=f32:3eaaaaab",
                        "EXR uses deterministic round-to-nearest-even binary32 bits");
    const auto exactHalf = analyzeFlatExrRgba32fLinRec709SceneV1(
        exrInput(descriptor(squareWindow, squareWindow, *oneHalf)));
    expectCode(expectations, exactHalf, Facet::PixelAspect, State::Exact, Code::None,
               "an exactly representable EXR rational remains Exact");
    const auto pngAspect =
        analyzePngRgba8SrgbV1(pngInput(descriptor(squareWindow, squareWindow, *oneThird)));
    expectCode(expectations, pngAspect, Facet::PixelAspect, State::Unsupported,
               Code::PngSquarePixelRequired, "PNG rejects a non-square source pixel aspect");
}

void testResourceBoundariesAndCoexistence(Expectations& expectations) {
    const auto dimensionAtLimit = analyzeFlatExrRgba32fLinRec709SceneV1(
        exrInput(descriptor(kOutputAnalysisMaximumDimensionV1, 1)));
    expectCode(expectations, dimensionAtLimit, Facet::ExternalDependencies, State::Exact,
               Code::None, "the exact maximum dimension does not exceed the resource limit");
    const auto dimensionOver = analyzeFlatExrRgba32fLinRec709SceneV1(
        exrInput(descriptor(kOutputAnalysisMaximumDimensionV1 + 1U, 1)));
    expectCode(expectations, dimensionOver, Facet::ExternalDependencies, State::Missing,
               Code::ResourceLimitExceeded, "one pixel over the maximum dimension is rejected");

    const auto pixelsAtLimit =
        analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(8'192, 8'192)));
    expectCode(expectations, pixelsAtLimit, Facet::ExternalDependencies, State::Exact, Code::None,
               "the exact maximum pixel count does not exceed the resource limit");
    const auto pixelsOver =
        analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(8'192, 8'193)));
    expectCode(expectations, pixelsOver, Facet::ExternalDependencies, State::Missing,
               Code::ResourceLimitExceeded, "one row over the maximum pixel count is rejected");

    const auto data = window(std::numeric_limits<std::int32_t>::max(), 0, 32'769, 1);
    const auto display = window(0, 0, 32'769, 1);
    const auto coexist = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(data, display)));
    expectCode(expectations, coexist, Facet::DataWindow, State::Unsupported, Code::WindowOutOfRange,
               "window representability remains truthful when a resource limit also fails");
    expectCode(expectations, coexist, Facet::ExternalDependencies, State::Missing,
               Code::ResourceLimitExceeded,
               "resource and window failures coexist without suppressing either facet");

    const auto largeDisplay = descriptor(window(0, 0, 1, 1), window(0, 0, 32'769, 1));
    const auto displayLimit = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(largeDisplay));
    expectCode(expectations, displayLimit, Facet::ExternalDependencies, State::Missing,
               Code::ResourceLimitExceeded,
               "the source display-window extent independently participates in resource limits");
}

void testInvalidInputsFailClosed(Expectations& expectations) {
    auto expectFailure = [&expectations](const OutputAnalysisAnalyzerResultV1& result,
                                         const AnalyzerError error,
                                         const std::string_view message) {
        expectations.expect(!result && !result.hasReport() && result.report() == nullptr &&
                                result.error() == error,
                            message);
    };

    auto unknownProcess = exrInput(descriptor());
    unknownProcess.process.state =
        std::bit_cast<OutputAnalysisProcessSourceStateV1>(std::uint8_t{0xFF});
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(unknownProcess)),
                  AnalyzerError::InvalidProcessSourceState,
                  "an unknown process-source enum fails closed");
    auto unknownAdapter = exrInput(descriptor());
    unknownAdapter.adapter = std::bit_cast<OutputAnalysisAdapterStateV1>(std::uint8_t{0xFF});
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(unknownAdapter)),
                  AnalyzerError::InvalidAdapterState, "an unknown adapter enum fails closed");
    auto unknownCompression = exrInput(descriptor());
    unknownCompression.compression =
        std::bit_cast<OutputAnalysisCompressionStateV1>(std::uint8_t{0xFF});
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(unknownCompression)),
                  AnalyzerError::InvalidCompressionState,
                  "an unknown compression enum fails closed");
    auto unknownDependency = exrInput(descriptor());
    unknownDependency.otherDependency =
        std::bit_cast<OutputAnalysisOtherDependencyStateV1>(std::uint8_t{0xFF});
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(unknownDependency)),
                  AnalyzerError::InvalidOtherDependencyState,
                  "an unknown output-dependency enum fails closed");
    auto unknownColor = pngInput(descriptor());
    unknownColor.colorResolution =
        std::bit_cast<PngRgba8SrgbColorResolutionStateV1>(std::uint8_t{0xFF});
    expectFailure(analyzePngRgba8SrgbV1(std::move(unknownColor)),
                  AnalyzerError::InvalidColorResolutionState,
                  "an unknown PNG color-resolution enum fails closed");

    OutputAnalysisProcessSourceV1 impossibleReady{
        .state = OutputAnalysisProcessSourceStateV1::Ready,
        .readyIdentity = {},
        .missingDescriptor = descriptor(),
    };
    auto readyInput = exrInput(descriptor());
    readyInput.process = std::move(impossibleReady);
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(readyInput)),
                  AnalyzerError::InvalidProcessSource,
                  "a Ready arm without an identity and with Missing data is impossible");
    auto absentMissing = exrInput(descriptor());
    absentMissing.process.missingDescriptor.reset();
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(absentMissing)),
                  AnalyzerError::InvalidProcessSource,
                  "a Missing arm cannot omit its validated descriptor");

    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto bothReadyArms = exrInput(descriptor());
    bothReadyArms.process = {.state = OutputAnalysisProcessSourceStateV1::Ready,
                             .readyIdentity = identity,
                             .missingDescriptor = descriptor()};
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(bothReadyArms)),
                  AnalyzerError::InvalidProcessSource,
                  "a Ready source cannot also populate the Missing descriptor arm");
    auto bothMissingArms = exrInput(descriptor());
    bothMissingArms.process.readyIdentity = identity;
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(bothMissingArms)),
                  AnalyzerError::InvalidProcessSource,
                  "a Missing source cannot also retain a Ready identity arm");

    auto invalidatedImage = std::move(const_cast<render::Rgba32fImage&>(frame->processImage()));
    auto invalidReady = exrInput(descriptor());
    invalidReady.process = {.state = OutputAnalysisProcessSourceStateV1::Ready,
                            .readyIdentity = identity,
                            .missingDescriptor = std::nullopt};
    expectFailure(analyzeFlatExrRgba32fLinRec709SceneV1(std::move(invalidReady)),
                  AnalyzerError::InvalidSourceDescriptor,
                  "a test-invalidated retained Ready frame fails descriptor validation");
    expectations.expect(invalidatedImage.isValid(),
                        "the test-only mutation retained the moved image outside the frame");
}

void testFailureSeamsPublishNothing(Expectations& expectations) {
    struct Fixture final {
        Fault fault;
        AnalyzerError error;
    };
    constexpr std::array fixtures{
        Fixture{Fault::AllocationFailure, AnalyzerError::AllocationFailure},
        Fixture{Fault::DescriptorTooLong, AnalyzerError::DescriptorTooLong},
        Fixture{Fault::DescriptorStorageTooLarge, AnalyzerError::DescriptorStorageTooLarge},
        Fixture{Fault::GeneratedReportInvariantViolation,
                AnalyzerError::GeneratedReportInvariantViolation},
    };
    for (const auto& fixture : fixtures) {
        const auto png =
            detail::analyzePngRgba8SrgbV1WithFaultForTest(pngInput(descriptor()), fixture.fault);
        expectations.expect(!png && !png.hasReport() && png.report() == nullptr &&
                                png.error() == fixture.error,
                            "every injected PNG failure publishes no partial report");
        const auto exr = detail::analyzeFlatExrRgba32fLinRec709SceneV1WithFaultForTest(
            exrInput(descriptor()), fixture.fault);
        expectations.expect(!exr && !exr.hasReport() && exr.report() == nullptr &&
                                exr.error() == fixture.error,
                            "every injected EXR failure publishes no partial report");
        if (fixture.fault == Fault::GeneratedReportInvariantViolation) {
            expectations.expect(png.generatedReportIssue().code ==
                                        OutputAnalysisReportErrorCodeV1::FacetOutOfOrder &&
                                    exr.generatedReportIssue().code ==
                                        OutputAnalysisReportErrorCodeV1::FacetOutOfOrder,
                                "generated invariant failures preserve the closed validator issue");
        }
    }
}

} // namespace

void runOutputAnalysisAnalyzerEdgeTests(Expectations& expectations) {
    testClosedCapabilityStatesAndPrecedence(expectations);
    testPngColorResolutionStates(expectations);
    testWindowAndPixelAspectDerivation(expectations);
    testResourceBoundariesAndCoexistence(expectations);
    testInvalidInputsFailClosed(expectations);
    testFailureSeamsPublishNothing(expectations);
}

} // namespace bloom::output::test
