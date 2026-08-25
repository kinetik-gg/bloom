#include <bloom/output/output_analysis_digest.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace color = bloom::color;
namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

using Code = output::OutputFacetStableCodeV1;
using DigestError = output::OutputAnalysisDigestErrorCodeV1;
using Facet = output::OutputFacetIdV1;
using Preset = output::OutputPresetV1;
using State = output::OutputPreservationStateV1;

template <typename Result>
concept ReadsDigestFromConstLvalue = requires(const Result& result) { result.digest(); };

template <typename Result>
concept ReadsDigestFromRvalue = requires(Result result) { std::move(result).digest(); };

template <typename Result>
concept ReadsDigestBytesFromConstLvalue =
    requires(const Result& result) { result.digest()->bytes(); };

template <typename Result>
concept ReadsDigestBytesFromRvalue =
    requires(Result result) { std::move(result).digest()->bytes(); };

template <typename Source>
concept AcceptsDigestSource =
    requires(const Source& source, const output::OutputAnalysisReportV1View report) {
        output::computeOutputAnalysisDigestV1(source, report);
    };

static_assert(ReadsDigestFromConstLvalue<output::OutputAnalysisDigestV1Result>);
static_assert(!ReadsDigestFromRvalue<output::OutputAnalysisDigestV1Result>);
static_assert(ReadsDigestBytesFromConstLvalue<output::OutputAnalysisDigestV1Result>);
static_assert(!ReadsDigestBytesFromRvalue<output::OutputAnalysisDigestV1Result>);
static_assert(AcceptsDigestSource<output::ProcessFrameSemanticIdentityV1>);
static_assert(!AcceptsDigestSource<runtime::ProcessFrame>);

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNodeId = document::NodeId::fromRaw(3);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(4);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(5);
constexpr auto kRevision = document::Revision::fromRaw(6);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(7);
constexpr auto kStackNodeId = document::NodeId::fromRaw(8);
constexpr auto kLayerId = document::LayerId::fromRaw(9);
constexpr auto kLayerSlotId = document::LayerSlotId::fromRaw(10);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(11);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(12);

constexpr std::string_view kChannels =
    "count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
    "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha";
constexpr std::string_view kSourcePixels =
    "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1";
constexpr std::string_view kPngPixels = "height=u:1;packing=id:rgba;sample-type=id:uint8;width=u:1";
constexpr std::string_view kWindow = "height=u:1;origin-x=i:0;origin-y=i:0;width=u:1";
constexpr std::string_view kNoDependencies = "kind=id:none;revision=id:none";
constexpr std::string_view kOcioDependency =
    "kind=id:ocio;revision=id:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

constexpr core::Sha256Digest::Bytes kRevisionBytes{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

// These are produced by an independent byte-oriented oracle for the exact fixture below.
constexpr std::string_view kExpectedExrDigest =
    "791a0c2e688e0afe55a74c737aea787f1622ed2eef7847b8d62ee314718076c1";
constexpr std::string_view kExpectedPngDigest =
    "a032aec2ed0b51e7d76120fa6229f720650ef2dab557968c776b0dfd03f4c6a6";

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool hasDigest(const core::Sha256Digest* const digest,
                             const std::string_view expected) noexcept {
    if (digest == nullptr) {
        return false;
    }
    const auto encoded = digest->toLowercaseHex();
    return std::string_view(encoded.data(), encoded.size()) == expected;
}

void printDigest(const std::string_view label, const core::Sha256Digest* const digest) {
    if (digest == nullptr) {
        return;
    }
    const auto encoded = digest->toLowercaseHex();
    std::cerr << label << '=' << std::string_view(encoded.data(), encoded.size()) << '\n';
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
planFor(const std::uint32_t width, const std::uint32_t height, const core::Color4d colorValue) {
    const auto format = document::CompositionFormat::create(width, height);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{kSolidNodeId, kColorParameterId, colorValue});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeId, kLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{
            kPositionParameterId,
            document::Vec2d{static_cast<double>(width) / 2.0, static_cast<double>(height) / 2.0}},
        runtime::CompiledScalarParameter{kOpacityParameterId, 1.0}});
    operations.emplace_back(runtime::CompiledLayerStack{
        kStackNodeId, {{kLayerSlotId, kLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const runtime::ProcessFrame>
evaluateFrame(const std::uint32_t width, const std::uint32_t height, const core::Color4d colorValue,
              const std::size_t pixelStorageByteLimit) {
    const auto plan = planFor(width, height, colorValue);
    const runtime::EvaluationRequest request{
        .time = core::RationalTime::fromInteger(0),
        .output = plan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = pixelStorageByteLimit,
    };
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(plan, request, {});
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
        std::abort();
    }
    return result.frame();
}

[[nodiscard]] std::shared_ptr<const runtime::ProcessFrame>
evaluateTinyFrame(const core::Color4d colorValue = {0.25, 0.5, 0.75, 1.0}) {
    return evaluateFrame(1, 1, colorValue, 1U << 20U);
}

[[nodiscard]] std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>
prepareIdentity(const std::shared_ptr<const runtime::ProcessFrame>& frame) {
    const output::ProcessFrameSemanticIdentityV1Preparer preparer;
    const auto result = preparer.prepare(frame, {});
    if (result.status() != output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        result.identity() == nullptr) {
        std::abort();
    }
    return result.identity();
}

[[nodiscard]] std::array<output::OutputFacetAssessmentV1View, output::kOutputAnalysisFacetCountV1>
makeExrReport() noexcept {
    return {{{Facet::Pixels, State::Exact, Code::None, kSourcePixels, kSourcePixels},
             {Facet::Precision, State::Exact, Code::None, "component-type=id:binary32",
              "component-type=id:binary32"},
             {Facet::Color, State::Exact, Code::None, "color-id=id:lin_rec709_scene",
              "color-id=id:lin_rec709_scene"},
             {Facet::AlphaAssociation, State::Exact, Code::None,
              "association=id:premultiplied;zero-alpha=id:canonical-zero",
              "association=id:premultiplied;zero-alpha=id:canonical-zero"},
             {Facet::Channels, State::Exact, Code::None, kChannels, kChannels},
             {Facet::DataWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::DisplayWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::PixelAspect, State::Exact, Code::None, "denominator=u:1;numerator=u:1",
              "value=f32:3f800000"},
             {Facet::Compression, State::Exact, Code::None, "", "method=id:zip"},
             {Facet::Metadata, State::Exact, Code::None, "profile=id:none", "profile=id:none"},
             {Facet::ExternalDependencies, State::Exact, Code::None, kNoDependencies,
              kNoDependencies}}};
}

[[nodiscard]] std::array<output::OutputFacetAssessmentV1View, output::kOutputAnalysisFacetCountV1>
makePngReport() noexcept {
    return {{{Facet::Pixels, State::Approximated, Code::PngDisplayTransformClampQuantize,
              kSourcePixels, kPngPixels},
             {Facet::Precision, State::Approximated, Code::PngFloat32ToUint8,
              "component-type=id:binary32", "component-type=id:uint8"},
             {Facet::Color, State::Approximated, Code::PngLinRec709SceneToSrgb,
              "color-id=id:lin_rec709_scene", "color-id=id:srgb_rec709_display"},
             {Facet::AlphaAssociation, State::Approximated, Code::PngPremultipliedToStraight,
              "association=id:premultiplied;zero-alpha=id:canonical-zero",
              "association=id:straight;zero-alpha=id:canonical-zero"},
             {Facet::Channels, State::Exact, Code::None, kChannels, kChannels},
             {Facet::DataWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::DisplayWindow, State::Exact, Code::None, kWindow, kWindow},
             {Facet::PixelAspect, State::Exact, Code::None, "denominator=u:1;numerator=u:1",
              "denominator=u:1;numerator=u:1"},
             {Facet::Compression, State::Exact, Code::None, "",
              "method=id:deflate-level-6-filter-none"},
             {Facet::Metadata, State::Exact, Code::None, "profile=id:none", "profile=id:none"},
             {Facet::ExternalDependencies, State::ExternalReference, Code::PngOcioExternalReference,
              kNoDependencies, kOcioDependency}}};
}

[[nodiscard]] color::DisplayProcessorIdentityV1InputView
displayInput(const core::Sha256Digest revision, const std::string_view displayName = "D") noexcept {
    static constexpr std::array contextVariables{
        color::DisplayProcessorContextVariableV1View{"A", "B"},
    };
    static constexpr std::array<std::string_view, 2> looks{"L", "M"};
    return {.expectedOcioRevision = revision,
            .contextVariables = contextVariables,
            .sourceColorSpaceId = color::kDisplayProcessorIdentitySourceColorSpaceId,
            .displayName = displayName,
            .viewName = "V",
            .lookMode = color::DisplayProcessorLookModeV1::Ordered,
            .lookNames = looks,
            .outputColorSpaceId = color::kDisplayProcessorIdentityOutputColorSpaceId,
            .qualityId = color::kDisplayProcessorIdentityQualityId,
            .semanticsProfileId = color::kDisplayProcessorIdentitySemanticsProfileId,
            .packingId = color::kDisplayProcessorIdentityPackingId};
}

[[nodiscard]] std::optional<color::DisplayProcessorIdentityV1>
makeDisplayIdentity(const core::Sha256Digest revision, const std::string_view displayName = "D") {
    const auto input = displayInput(revision, displayName);
    const auto validation = color::validateDisplayProcessorIdentityV1(input);
    if (!validation) {
        std::abort();
    }
    std::vector<std::byte> bytes(validation.requiredByteCount());
    if (!color::writeDisplayProcessorIdentityV1(input, bytes)) {
        std::abort();
    }
    auto adopted = color::adoptDisplayProcessorIdentityV1(std::move(bytes));
    if (!adopted) {
        std::abort();
    }
    return std::move(adopted).takeIdentity();
}

void testIndependentDigestVectors(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto exr = makeExrReport();
    const auto exrDigest = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    expectations.expect(exrDigest && exrDigest.preimageByteCount() == 1485,
                        "the tiny EXR preimage has its independently counted byte length");
    if (exrDigest && !hasDigest(exrDigest.digest(), kExpectedExrDigest)) {
        printDigest("actual-exr", exrDigest.digest());
    }
    expectations.expect(exrDigest && hasDigest(exrDigest.digest(), kExpectedExrDigest),
                        "the streamed EXR preimage matches the independent SHA-256 vector");

    const auto revision = core::Sha256Digest::fromBytes(kRevisionBytes);
    auto displayIdentity = makeDisplayIdentity(revision);
    if (!displayIdentity) {
        std::abort();
    }
    auto png = makePngReport();
    const output::OutputAnalysisDigestDependenciesV1 dependencies{
        .expectedOcioRevision = revision,
        .displayProcessorIdentity = &*displayIdentity,
    };
    const auto pngDigest = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::PngRgba8SrgbV1, png}, dependencies);
    expectations.expect(pngDigest && pngDigest.preimageByteCount() == 1922,
                        "the tiny PNG preimage has its independently counted byte length");
    if (pngDigest && !hasDigest(pngDigest.digest(), kExpectedPngDigest)) {
        printDigest("actual-png", pngDigest.digest());
    }
    expectations.expect(pngDigest && hasDigest(pngDigest.digest(), kExpectedPngDigest),
                        "the streamed PNG preimage matches the independent SHA-256 vector");
}

void testReportRevalidationAndFrameBinding(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto exr = makeExrReport();
    exr[8].state = State::Missing;
    exr[8].stableCode = Code::CompressionUnavailable;
    expectations.expect(
        output::computeOutputAnalysisDigestV1(*identity,
                                              {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                .error() == DigestError::ReportNotApprovable,
        "a structurally valid report with a denied permission bit has no approval digest");

    exr = makeExrReport();
    exr[8].state = State::Missing;
    expectations.expect(
        output::computeOutputAnalysisDigestV1(*identity,
                                              {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                .error() == DigestError::InvalidReport,
        "digesting revalidates state/code tuples instead of trusting an earlier borrowed token");

    exr = makeExrReport();
    exr[0].stableCode = std::bit_cast<Code>(std::uint8_t{0xFF});
    const auto unknownCode = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    expectations.expect(unknownCode.error() == DigestError::InvalidReport &&
                            !unknownCode.digest() &&
                            unknownCode.reportIssue().code ==
                                output::OutputAnalysisReportErrorCodeV1::InvalidStableCode,
                        "an unknown stable-code enum fails closed without exposing a digest");

    auto hostile = makeExrReport();
    auto unknownPreset = output::computeOutputAnalysisDigestV1(
        *identity, {std::bit_cast<Preset>(std::uint8_t{0xFF}), hostile});
    expectations.expect(unknownPreset.error() == DigestError::InvalidReport &&
                            unknownPreset.reportIssue().code ==
                                output::OutputAnalysisReportErrorCodeV1::InvalidPreset,
                        "an unknown preset enum fails closed at digest intake");
    hostile = makeExrReport();
    hostile[0].state = std::bit_cast<State>(std::uint8_t{0xFF});
    const auto unknownState = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, hostile});
    expectations.expect(unknownState.error() == DigestError::InvalidReport &&
                            unknownState.reportIssue().code ==
                                output::OutputAnalysisReportErrorCodeV1::InvalidState,
                        "an unknown state enum fails closed at digest intake");
    hostile = makeExrReport();
    hostile[0].facet = std::bit_cast<Facet>(std::uint8_t{0xFF});
    const auto unknownFacet = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, hostile});
    expectations.expect(unknownFacet.error() == DigestError::InvalidReport &&
                            unknownFacet.reportIssue().code ==
                                output::OutputAnalysisReportErrorCodeV1::InvalidFacet,
                        "an unknown facet enum fails closed at digest intake");

    exr = makeExrReport();
    constexpr std::string_view widthTwoPixels =
        "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:2";
    constexpr std::string_view widthTwoWindow = "height=u:1;origin-x=i:0;origin-y=i:0;width=u:2";
    exr[0].sourceDescriptor = widthTwoPixels;
    exr[0].targetDescriptor = widthTwoPixels;
    exr[5].sourceDescriptor = widthTwoWindow;
    exr[5].targetDescriptor = widthTwoWindow;
    expectations.expect(output::computeOutputAnalysisDigestV1(
                            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                                .error() == DigestError::ProcessFrameDescriptorMismatch,
                        "a self-consistent report cannot describe a different process frame");

    exr = makeExrReport();
    constexpr std::string_view translatedWindow = "height=u:1;origin-x=i:1;origin-y=i:0;width=u:1";
    exr[5].sourceDescriptor = translatedWindow;
    exr[5].targetDescriptor = translatedWindow;
    expectations.expect(output::computeOutputAnalysisDigestV1(
                            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                                .error() == DigestError::ProcessFrameDescriptorMismatch,
                        "source window origins are bound to the actual process frame");

    exr = makeExrReport();
    exr[6].sourceDescriptor = translatedWindow;
    exr[6].targetDescriptor = translatedWindow;
    expectations.expect(output::computeOutputAnalysisDigestV1(
                            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                                .error() == DigestError::ProcessFrameDescriptorMismatch,
                        "source display-window origins are bound to the actual process frame");

    exr = makeExrReport();
    exr[7].sourceDescriptor = "denominator=u:1;numerator=u:2";
    exr[7].targetDescriptor = "value=f32:40000000";
    expectations.expect(output::computeOutputAnalysisDigestV1(
                            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr})
                                .error() == DigestError::ProcessFrameDescriptorMismatch,
                        "source pixel aspect is bound to the actual process frame");

    const auto changedFrame = evaluateTinyFrame({0.25, 0.5, 0.5, 1.0});
    const auto changedIdentity = prepareIdentity(changedFrame);
    exr = makeExrReport();
    const auto original = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    const auto changed = output::computeOutputAnalysisDigestV1(
        *changedIdentity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    expectations.expect(original && changed && original.digest() != nullptr &&
                            changed.digest() != nullptr && *original.digest() != *changed.digest(),
                        "a one-component process-pixel mutation changes the analysis digest");
}

void testPresetDependencyClosure(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto png = makePngReport();
    expectations.expect(
        output::computeOutputAnalysisDigestV1(*identity, {Preset::PngRgba8SrgbV1, png}).error() ==
            DigestError::MissingPngExpectedOcioRevision,
        "PNG requires an explicit expected OCIO revision");

    const auto revision = core::Sha256Digest::fromBytes(kRevisionBytes);
    auto displayIdentity = makeDisplayIdentity(revision);
    if (!displayIdentity) {
        std::abort();
    }
    expectations.expect(output::computeOutputAnalysisDigestV1(*identity,
                                                              {Preset::PngRgba8SrgbV1, png},
                                                              {.expectedOcioRevision = revision})
                                .error() == DigestError::MissingPngDisplayIdentity,
                        "PNG requires an owning validated display identity");

    auto differentRevisionBytes = kRevisionBytes;
    differentRevisionBytes.back() =
        static_cast<std::uint8_t>(differentRevisionBytes.back() ^ std::uint8_t{1});
    const auto differentRevision = core::Sha256Digest::fromBytes(differentRevisionBytes);
    expectations.expect(
        output::computeOutputAnalysisDigestV1(*identity, {Preset::PngRgba8SrgbV1, png},
                                              {.expectedOcioRevision = differentRevision,
                                               .displayProcessorIdentity = &*displayIdentity})
                .error() == DigestError::OcioRevisionMismatch,
        "the expected revision must equal the display identity's embedded revision");

    png[10].targetDescriptor =
        "kind=id:ocio;revision=id:100102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    expectations.expect(
        output::computeOutputAnalysisDigestV1(
            *identity, {Preset::PngRgba8SrgbV1, png},
            {.expectedOcioRevision = revision, .displayProcessorIdentity = &*displayIdentity})
                .error() == DigestError::TargetDependencyRevisionMismatch,
        "the target dependency descriptor is bound to the same exact revision");

    auto exr = makeExrReport();
    expectations.expect(output::computeOutputAnalysisDigestV1(
                            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr},
                            {.expectedOcioRevision = revision})
                                .error() == DigestError::UnexpectedExrExpectedOcioRevision,
                        "process EXR rejects an extraneous OCIO revision");
    expectations.expect(
        output::computeOutputAnalysisDigestV1(
            *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr},
            {.expectedOcioRevision = std::nullopt, .displayProcessorIdentity = &*displayIdentity})
                .error() == DigestError::UnexpectedExrDisplayIdentity,
        "process EXR rejects an extraneous display identity");

    auto replacementIdentity = makeDisplayIdentity(revision, "E");
    if (!replacementIdentity) {
        std::abort();
    }
    png = makePngReport();
    const auto first = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::PngRgba8SrgbV1, png},
        {.expectedOcioRevision = revision, .displayProcessorIdentity = &*displayIdentity});
    const auto changed = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::PngRgba8SrgbV1, png},
        {.expectedOcioRevision = revision, .displayProcessorIdentity = &*replacementIdentity});
    expectations.expect(first && changed && first.digest() != nullptr &&
                            changed.digest() != nullptr && *first.digest() != *changed.digest(),
                        "a one-byte display identity mutation changes the analysis digest");

    auto movedIdentity = std::move(*replacementIdentity);
    expectations.expect(
        output::computeOutputAnalysisDigestV1(
            *identity, {Preset::PngRgba8SrgbV1, png},
            {.expectedOcioRevision = revision, .displayProcessorIdentity = &*replacementIdentity})
                    .error() == DigestError::InvalidDisplayIdentity &&
            movedIdentity.hasValue(),
        "a moved-from owning display identity cannot forge a trusted borrowed view");
}

void testDependencyClosureUsesPreparedIdentity(Expectations& expectations) {
    const auto frame = evaluateTinyFrame();
    const auto identity = prepareIdentity(frame);
    auto png = makePngReport();
    const auto missingRevision =
        output::computeOutputAnalysisDigestV1(*identity, {Preset::PngRgba8SrgbV1, png});
    expectations.expect(missingRevision.error() == DigestError::MissingPngExpectedOcioRevision &&
                            !missingRevision.digest(),
                        "missing PNG dependencies fail before process-pixel identity work");

    const auto revision = core::Sha256Digest::fromBytes(kRevisionBytes);
    auto displayIdentity = makeDisplayIdentity(revision);
    if (!displayIdentity) {
        std::abort();
    }
    auto differentRevisionBytes = kRevisionBytes;
    differentRevisionBytes.front() = std::uint8_t{1};
    const auto differentRevision = core::Sha256Digest::fromBytes(differentRevisionBytes);
    const auto mismatchedRevision = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::PngRgba8SrgbV1, png},
        {.expectedOcioRevision = differentRevision, .displayProcessorIdentity = &*displayIdentity});
    expectations.expect(mismatchedRevision.error() == DigestError::OcioRevisionMismatch &&
                            !mismatchedRevision.digest(),
                        "mismatched PNG dependencies fail before process-pixel identity work");

    auto exr = makeExrReport();
    const auto baseline = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    auto pixels = frame->processImage().pixels();
    const auto replacement = render::Rgba32f::fromPremultiplied(0.5F, 0.25F, 0.125F, 1.0F);
    if (!replacement) {
        std::abort();
    }
    const_cast<render::Rgba32f&>(pixels.front()) = *replacement.value();
    const auto afterTestOnlyMutation = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    expectations.expect(baseline && afterTestOnlyMutation && baseline.digest() != nullptr &&
                            afterTestOnlyMutation.digest() != nullptr &&
                            *baseline.digest() == *afterTestOnlyMutation.digest(),
                        "digesting consumes the prepared product without rereading frame pixels");
}

void testHardCapBoundaryPreflight(Expectations& expectations) {
    static_assert(output::kOutputAnalysisMaximumDimensionV1 == 32'768U);
    static_assert(output::kOutputAnalysisMaximumPixelCountV1 == 67'108'864U);
    static_assert(output::kOutputAnalysisMaximumProcessPixelBytesV1 == 1'073'741'824U);

    constexpr std::uint32_t width = output::kOutputAnalysisMaximumDimensionV1;
    const auto frame = evaluateFrame(width, 1, {0.25, 0.5, 0.75, 1.0}, 4U << 20U);
    const auto identity = prepareIdentity(frame);
    const auto widthText = std::to_string(width);
    const std::string pixels =
        "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:" + widthText;
    const std::string window = "height=u:1;origin-x=i:0;origin-y=i:0;width=u:" + widthText;
    auto exr = makeExrReport();
    exr[0].sourceDescriptor = pixels;
    exr[0].targetDescriptor = pixels;
    exr[5].sourceDescriptor = window;
    exr[5].targetDescriptor = window;
    exr[6].sourceDescriptor = window;
    exr[6].targetDescriptor = window;
    const auto atMaximum = output::computeOutputAnalysisDigestV1(
        *identity, {Preset::FlatExrRgba32fLinRec709SceneV1, exr});
    expectations.expect(atMaximum && atMaximum.digest(),
                        "the exact maximum dimension passes digest preflight");
}

} // namespace

int main() {
    Expectations expectations;
    testIndependentDigestVectors(expectations);
    testReportRevalidationAndFrameBinding(expectations);
    testPresetDependencyClosure(expectations);
    testDependencyClosureUsesPreparedIdentity(expectations);
    testHardCapBoundaryPreflight(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
