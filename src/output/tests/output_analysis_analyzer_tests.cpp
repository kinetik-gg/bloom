#include "output_analysis_analyzer_test_support.hpp"

#include <bloom/output/output_analysis_digest.hpp>

#include "output_analysis_analyzer_test_access.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bloom::output::test {

namespace {

using Code = OutputFacetStableCodeV1;
using Facet = OutputFacetIdV1;
using State = OutputPreservationStateV1;

template <typename Report>
concept ReadsViewFromConstLvalue = requires(const Report& report) { report.view(); };

template <typename Report>
concept ReadsViewFromRvalue = requires(Report report) { std::move(report).view(); };

template <typename Result>
concept ReadsReportFromConstLvalue = requires(const Result& result) { result.report(); };

template <typename Result>
concept ReadsReportFromRvalue = requires(Result result) { std::move(result).report(); };

static_assert(!std::is_default_constructible_v<OutputAnalysisReportV1>);
static_assert(!std::is_copy_constructible_v<OutputAnalysisReportV1>);
static_assert(std::is_move_constructible_v<OutputAnalysisReportV1>);
static_assert(!std::is_move_assignable_v<OutputAnalysisReportV1>);
static_assert(ReadsViewFromConstLvalue<OutputAnalysisReportV1>);
static_assert(!ReadsViewFromRvalue<OutputAnalysisReportV1>);
static_assert(!std::is_default_constructible_v<OutputAnalysisAnalyzerResultV1>);
static_assert(std::is_copy_constructible_v<OutputAnalysisAnalyzerResultV1>);
static_assert(std::is_copy_assignable_v<OutputAnalysisAnalyzerResultV1>);
static_assert(ReadsReportFromConstLvalue<OutputAnalysisAnalyzerResultV1>);
static_assert(!ReadsReportFromRvalue<OutputAnalysisAnalyzerResultV1>);
static_assert(kOutputAnalysisReportDescriptorStorageMaximumBytesV1 ==
              std::size_t{22} * std::size_t{1024});

[[nodiscard]] OutputAnalysisProcessSourceV1
readySource(const std::shared_ptr<const ProcessFrameSemanticIdentityV1>& identity) {
    return {.state = OutputAnalysisProcessSourceStateV1::Ready,
            .readyIdentity = identity,
            .missingDescriptor = std::nullopt};
}

void expectFacet(Expectations& expectations, const OutputAnalysisReportV1& report,
                 const Facet facetId, const State state, const Code code,
                 const std::string_view source, const std::string_view target) {
    const auto& assessment = facet(report, facetId);
    expectations.expect(
        assessment.facet == facetId && assessment.state == state && assessment.stableCode == code &&
            assessment.sourceDescriptor == source && assessment.targetDescriptor == target,
        "the analyzer emits the frozen facet tuple and descriptors");
}

void testNominalPresetsAndDigestGoldens(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    const auto revision = core::Sha256Digest::fromBytes(kOcioRevisionBytes);

    auto pngInputValue = pngInput(descriptor());
    pngInputValue.process = readySource(identity);
    const auto png = analyzePngRgba8SrgbV1(std::move(pngInputValue));
    expectations.expect(png && png.error() == OutputAnalysisAnalyzerErrorCodeV1::None &&
                            png.report() != nullptr && png.report()->approvable() &&
                            png.report()->permissionMask().allPermitted(),
                        "the nominal PNG analysis publishes one approvable owning report");
    if (!png || png.report() == nullptr) {
        return;
    }
    const auto pngView = png.report()->view();
    const auto pngValidation = validateOutputAnalysisReportV1(pngView);
    expectations.expect(pngView.preset == OutputPresetV1::PngRgba8SrgbV1 &&
                            pngView.facets.size() == kOutputAnalysisFacetCountV1 &&
                            pngValidation.approvable(),
                        "the PNG owner exposes an independently valid eleven-facet view");
    expectFacet(expectations, *png.report(), Facet::Pixels, State::Approximated,
                Code::PngDisplayTransformClampQuantize,
                "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1",
                "height=u:1;packing=id:rgba;sample-type=id:uint8;width=u:1");
    expectFacet(expectations, *png.report(), Facet::Color, State::Approximated,
                Code::PngLinRec709SceneToSrgb, "color-id=id:lin_rec709_scene",
                "color-id=id:srgb_rec709_display");
    expectFacet(expectations, *png.report(), Facet::ExternalDependencies, State::ExternalReference,
                Code::PngOcioExternalReference, "kind=id:none;revision=id:none",
                "kind=id:ocio;revision=id:"
                "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    auto displayIdentity = makeDisplayIdentity(revision);
    const auto pngDigest = computeOutputAnalysisDigestV1(
        *identity, png.report()->view(),
        {.expectedOcioRevision = revision, .displayProcessorIdentity = &displayIdentity});
    expectations.expect(
        pngDigest && pngDigest.preimageByteCount() == 1922 &&
            hasDigest(pngDigest.digest(),
                      "a032aec2ed0b51e7d76120fa6229f720650ef2dab557968c776b0dfd03f4c6a6"),
        "the analyzer-produced PNG report preserves the independent digest golden");

    auto exrInputValue = exrInput(descriptor());
    exrInputValue.process = readySource(identity);
    const auto exr = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(exrInputValue));
    expectations.expect(exr && exr.report() != nullptr && exr.report()->approvable(),
                        "the nominal EXR analysis publishes one approvable owning report");
    if (!exr || exr.report() == nullptr) {
        return;
    }
    const auto exrView = exr.report()->view();
    const auto exrValidation = validateOutputAnalysisReportV1(exrView);
    expectations.expect(exrView.preset == OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 &&
                            exrView.facets.size() == kOutputAnalysisFacetCountV1 &&
                            exrValidation.approvable(),
                        "the EXR owner exposes an independently valid eleven-facet view");
    expectFacet(expectations, *exr.report(), Facet::Pixels, State::Exact, Code::None,
                "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1",
                "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1");
    expectFacet(expectations, *exr.report(), Facet::PixelAspect, State::Exact, Code::None,
                "denominator=u:1;numerator=u:1", "value=f32:3f800000");
    expectFacet(expectations, *exr.report(), Facet::ExternalDependencies, State::Exact, Code::None,
                "kind=id:none;revision=id:none", "kind=id:none;revision=id:none");
    const auto exrDigest = computeOutputAnalysisDigestV1(*identity, exr.report()->view());
    expectations.expect(
        exrDigest && exrDigest.preimageByteCount() == 1485 &&
            hasDigest(exrDigest.digest(),
                      "791a0c2e688e0afe55a74c737aea787f1622ed2eef7847b8d62ee314718076c1"),
        "the analyzer-produced EXR report preserves the independent digest golden");
}

void testMissingSourceIsolation(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto readyInput = exrInput(descriptor());
    readyInput.process = readySource(identity);
    const auto ready = analyzeFlatExrRgba32fLinRec709SceneV1(std::move(readyInput));
    const auto missing = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor()));
    expectations.expect(ready && missing && ready.report() != nullptr &&
                            missing.report() != nullptr,
                        "both Ready and Missing process sources produce owning reports");
    if (!ready || !missing || ready.report() == nullptr || missing.report() == nullptr) {
        return;
    }
    const auto readyView = ready.report()->view();
    const auto missingView = missing.report()->view();
    expectations.expect(missingView.facets.front().state == State::Missing &&
                            missingView.facets.front().stableCode == Code::ProcessFrameMissing &&
                            !missing.report()->approvable(),
                        "a Missing process source denies only the Pixels facet");
    bool remainingFacetsEqual = true;
    for (std::size_t index = 1; index < readyView.facets.size(); ++index) {
        const auto& left = readyView.facets[index];
        const auto& right = missingView.facets[index];
        remainingFacetsEqual = remainingFacetsEqual && left.facet == right.facet &&
                               left.state == right.state && left.stableCode == right.stableCode &&
                               left.sourceDescriptor == right.sourceDescriptor &&
                               left.targetDescriptor == right.targetDescriptor;
    }
    expectations.expect(remainingFacetsEqual,
                        "Missing process identity leaves the other ten derivations unchanged");
}

void testOwningLifetimeAndResultCoherence(Expectations& expectations) {
    auto result = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor()));
    expectations.expect(result && result.report() != nullptr,
                        "a valid analysis owns a shared immutable report");
    if (!result || result.report() == nullptr) {
        return;
    }
    const auto retained = result.report();
    const auto* const sourceAddress = result.report().get();
    const auto pixelsText = facet(*retained, Facet::Pixels).sourceDescriptor;
    // The result is intentionally copy-only: an rvalue construction shares the immutable report
    // and leaves the source coherent instead of consuming it.
    auto rvalueCopy = std::move(result); // NOLINT(performance-move-const-arg)
    expectations.expect(result &&        // NOLINT(bugprone-use-after-move)
                            rvalueCopy && result.report().get() == sourceAddress &&
                            rvalueCopy.report().get() == sourceAddress,
                        "copy-only result semantics keep both non-const move endpoints coherent");
    result = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(2, 1)));
    rvalueCopy = analyzeFlatExrRgba32fLinRec709SceneV1(exrInput(descriptor(3, 1)));
    expectations.expect(retained->view().facets.size() == kOutputAnalysisFacetCountV1 &&
                            facet(*retained, Facet::Pixels).sourceDescriptor == pixelsText,
                        "a retained report keeps every descriptor view alive after results change");
    expectations.expect(retained->descriptorByteCount() <=
                            kOutputAnalysisReportDescriptorStorageMaximumBytesV1,
                        "the report exposes its bounded aggregate descriptor byte count by value");
}

} // namespace

} // namespace bloom::output::test

int main() {
    bloom::output::test::Expectations expectations;
    bloom::output::test::testNominalPresetsAndDigestGoldens(expectations);
    bloom::output::test::testMissingSourceIsolation(expectations);
    bloom::output::test::testOwningLifetimeAndResultCoherence(expectations);
    bloom::output::test::runOutputAnalysisAnalyzerEdgeTests(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
