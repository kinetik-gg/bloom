#include "output_semantic_identity.hpp"

#include "output_analysis_analyzer_test_support.hpp"
#include "output_semantic_identity_test_access.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace color = bloom::color;
namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace test = bloom::output::test;

using namespace std::chrono_literals;

// An independent byte-oriented oracle assembled every frozen field with explicit big-endian
// packing. These lengths and SHA-256 values cover the complete tiny PNG and EXR preimages.
constexpr std::string_view kExpectedPngAnalysisDigest =
    "a032aec2ed0b51e7d76120fa6229f720650ef2dab557968c776b0dfd03f4c6a6";
constexpr std::string_view kExpectedPngOutputDigest =
    "cbef24efb48761fde472cd7ff1d6fbea2206420bf0fc27e84fe75d263b34e1c0";
constexpr std::string_view kExpectedExrOutputDigest =
    "2645132e8d63892ed500119e27ed6a1ff6d93ea8d58a5e51a3bfc23aab7ec375";
constexpr std::uint64_t kExpectedPngPreimageBytes = 669;
constexpr std::uint64_t kExpectedExrPreimageBytes = 567;

template <typename Input>
concept HasIndependentProcessIdentity = requires(Input input) { input.processIdentity; };

template <typename Input>
concept HasIndependentDisplayIdentity = requires(Input input) { input.displayProcessorIdentity; };

template <typename Input>
concept HasIndependentBoundAnalysis = requires(Input input) { input.boundAnalysis; };

template <typename Input>
concept HasIndependentDimensions = requires(Input input) { input.dimensions; };

template <typename Input>
concept HasIndependentMetadata = requires(Input input) { input.metadata; };

template <typename Payload, typename Storage>
concept HasPublicAdopt = requires(Storage storage) { Payload::adopt(std::move(storage)); };

static_assert(!HasIndependentProcessIdentity<output::PngRgba8SrgbOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentProcessIdentity<
              output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentDisplayIdentity<output::PngRgba8SrgbOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentDisplayIdentity<
              output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentBoundAnalysis<output::PngRgba8SrgbOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentBoundAnalysis<
              output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1>);
static_assert(!HasIndependentDimensions<output::PngRgba8SrgbOutputSemanticIdentityInputV1>);
static_assert(
    !HasIndependentMetadata<output::FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1>);
static_assert(
    !HasPublicAdopt<output::PngRgba8SrgbVerifiedSemanticProductV1, std::vector<std::uint8_t>>);
static_assert(!HasPublicAdopt<output::FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1,
                              std::vector<std::uint32_t>>);
static_assert(
    !std::is_constructible_v<output::PngRgba8SrgbVerifiedSemanticProductV1,
                             std::shared_ptr<const output::PngRgba8SrgbBoundOutputAnalysisV1>,
                             render::ImageExtent, std::vector<std::uint8_t>>);
static_assert(!std::is_constructible_v<
              output::FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1,
              std::shared_ptr<const output::FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>,
              output::FlatExrRgba32fSemanticMetadataV1, std::vector<std::uint32_t>>);

[[nodiscard]] render::ImageExtent extent(const std::uint32_t width, const std::uint32_t height) {
    const auto result = render::ImageExtent::create(width, height);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

[[nodiscard]] output::PngRgba8SrgbVerifiedSemanticProductV1
verifiedPngForTest(std::shared_ptr<const output::PngRgba8SrgbBoundOutputAnalysisV1> boundAnalysis,
                   const render::ImageExtent dimensions, std::vector<std::uint8_t>&& rgbaBytes) {
    return output::detail::OutputSemanticPayloadV1TestAccess::forgePngForTest(
        std::move(boundAnalysis), dimensions, std::move(rgbaBytes));
}

[[nodiscard]] output::FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1 verifiedExrForTest(
    std::shared_ptr<const output::FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> boundAnalysis,
    const output::FlatExrRgba32fSemanticMetadataV1 metadata,
    std::vector<std::uint32_t>&& rgbaComponentBits) {
    return output::detail::OutputSemanticPayloadV1TestAccess::forgeExrForTest(
        std::move(boundAnalysis), metadata, std::move(rgbaComponentBits));
}

[[nodiscard]] std::shared_ptr<const output::OutputAnalysisReportV1>
pngReport(const std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>& processIdentity) {
    const auto analyzed = output::analyzePngRgba8SrgbV1(
        {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                     .readyIdentity = processIdentity,
                     .missingDescriptor = std::nullopt},
         .expectedOcioRevision = core::Sha256Digest::fromBytes(test::kOcioRevisionBytes)});
    if (!analyzed || !analyzed.report()->approvable()) {
        std::abort();
    }
    return analyzed.report();
}

[[nodiscard]] std::shared_ptr<const output::OutputAnalysisReportV1>
exrReport(const std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>& processIdentity) {
    const auto analyzed = output::analyzeFlatExrRgba32fLinRec709SceneV1(
        {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                     .readyIdentity = processIdentity,
                     .missingDescriptor = std::nullopt}});
    if (!analyzed || !analyzed.report()->approvable()) {
        std::abort();
    }
    return analyzed.report();
}

[[nodiscard]] std::shared_ptr<const color::DisplayProcessorIdentityV1> displayIdentity() {
    const auto revision = core::Sha256Digest::fromBytes(test::kOcioRevisionBytes);
    return std::make_shared<const color::DisplayProcessorIdentityV1>(
        test::makeDisplayIdentity(revision));
}

[[nodiscard]] std::shared_ptr<const output::PngRgba8SrgbBoundOutputAnalysisV1>
bindPng(const std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>& processIdentity,
        const std::shared_ptr<const output::OutputAnalysisReportV1>& report,
        const std::shared_ptr<const color::DisplayProcessorIdentityV1>& display) {
    const auto bound = output::bindPngRgba8SrgbOutputAnalysisV1(
        processIdentity, report, core::Sha256Digest::fromBytes(test::kOcioRevisionBytes), display);
    if (bound.analysis() == nullptr) {
        std::abort();
    }
    return bound.analysis();
}

[[nodiscard]]
std::shared_ptr<const output::FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>
bindExr(const std::shared_ptr<const output::ProcessFrameSemanticIdentityV1>& processIdentity,
        const std::shared_ptr<const output::OutputAnalysisReportV1>& report) {
    const auto bound =
        output::bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(processIdentity, report);
    if (bound.analysis() == nullptr) {
        std::abort();
    }
    return bound.analysis();
}

[[nodiscard]] std::vector<std::uint32_t>
processComponentBits(const output::ProcessFrameSemanticIdentityV1& processIdentity) {
    std::vector<std::uint32_t> bits;
    const auto& frame = processIdentity.processFrame();
    if (frame == nullptr) {
        std::abort();
    }
    bits.reserve(frame->processImage().pixels().size() * 4U);
    for (const auto pixel : frame->processImage().pixels()) {
        for (const auto component : pixel.components()) {
            bits.push_back(std::bit_cast<std::uint32_t>(component));
        }
    }
    return bits;
}

[[nodiscard]] output::FlatExrRgba32fSemanticMetadataV1
exrMetadata(const output::ProcessFrameSemanticIdentityV1& processIdentity,
            const std::uint32_t pixelAspectBits = 0x3F800000U) {
    const auto* descriptor = processIdentity.processFrame()->processImage().descriptor();
    if (descriptor == nullptr) {
        std::abort();
    }
    const auto convert = [](const render::ImageWindow window) {
        return output::FlatExrInclusiveWindowV1{
            .xMin = static_cast<std::int32_t>(window.originX()),
            .yMin = static_cast<std::int32_t>(window.originY()),
            .xMax = static_cast<std::int32_t>(window.maxXExclusive() - 1),
            .yMax = static_cast<std::int32_t>(window.maxYExclusive() - 1),
        };
    };
    return {.dataWindow = convert(descriptor->dataWindow()),
            .displayWindow = convert(descriptor->displayWindow()),
            .pixelAspectRatioBits = pixelAspectBits};
}

void printDigest(const std::string_view label, const core::Sha256Digest& digest,
                 const std::uint64_t preimageBytes) {
    const auto hex = digest.toLowercaseHex();
    std::cerr << label << '=' << std::string_view(hex.data(), hex.size())
              << " preimage=" << preimageBytes << '\n';
}

void testBoundAnalysisAndPngGolden(test::Expectations& expectations) {
    auto process = test::prepareIdentity(test::evaluateTinyFrame());
    auto report = pngReport(process);
    auto display = displayIdentity();
    const auto* exactProcess = process.get();
    const auto* exactReport = report.get();
    const auto* exactDisplay = display.get();
    const auto boundResult = output::bindPngRgba8SrgbOutputAnalysisV1(
        process, report, core::Sha256Digest::fromBytes(test::kOcioRevisionBytes), display);
    expectations.expect(boundResult.analysis() != nullptr &&
                            boundResult.error() == output::BoundOutputAnalysisErrorCodeV1::None &&
                            boundResult.digestError() ==
                                output::OutputAnalysisDigestErrorCodeV1::None,
                        "PNG bound-analysis success has exactly one value arm and no error arm");
    auto bound = boundResult.analysis();
    if (bound == nullptr) {
        std::abort();
    }

    expectations.expect(test::hasDigest(&bound->digest(), kExpectedPngAnalysisDigest),
                        "PNG binding retains the independently golden analysis digest");
    expectations.expect(bound->processIdentity().get() == exactProcess &&
                            bound->report().get() == exactReport &&
                            bound->displayProcessorIdentity().get() == exactDisplay,
                        "PNG binding retains the exact process, report, and display owners");

    const auto wrongPreset = output::bindPngRgba8SrgbOutputAnalysisV1(
        process, exrReport(process), core::Sha256Digest::fromBytes(test::kOcioRevisionBytes),
        display);
    expectations.expect(
        wrongPreset.analysis() == nullptr &&
            wrongPreset.error() == output::BoundOutputAnalysisErrorCodeV1::DigestRejected &&
            wrongPreset.digestError() != output::OutputAnalysisDigestErrorCodeV1::None,
        "a PNG bound-analysis failure has only an error arm");

    process.reset();
    report.reset();
    display.reset();
    std::vector<output::OutputSemanticIdentityProgressV1> progress;
    const output::OutputSemanticIdentityV1Preparer preparer;
    const auto prepared = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct = verifiedPngForTest(bound, extent(1, 1), {0x00U, 0x7FU, 0x80U, 0xFFU})},
        {}, [&progress](const auto& update) { progress.push_back(update); });
    expectations.expect(
        prepared.identity() != nullptr &&
            prepared.status() == output::OutputSemanticIdentityPreparationStatusV1::Prepared &&
            prepared.identity()->payloadKind() == output::OutputSemanticPayloadKindV1::PngRgba8 &&
            test::hasDigest(&prepared.identity()->analysisDigest(), kExpectedPngAnalysisDigest) &&
            prepared.identity()->processIdentity().get() == exactProcess &&
            prepared.identity()->displayProcessorIdentity() == exactDisplay &&
            std::ranges::equal(prepared.identity()->pngRgba8Bytes(),
                               std::array<std::uint8_t, 4>{0, 127, 128, 255}),
        "PNG publication retains its one preset-correlated owner and payload arm");
    bound.reset();
    expectations.expect(prepared.identity() != nullptr &&
                            prepared.identity()->processIdentity().get() == exactProcess &&
                            prepared.identity()->displayProcessorIdentity() == exactDisplay,
                        "the PNG output identity owns its bound analysis after caller release");
    if (prepared.identity() != nullptr &&
        (!test::hasDigest(&prepared.identity()->digest(), kExpectedPngOutputDigest) ||
         prepared.identity()->preimageByteCount() != kExpectedPngPreimageBytes)) {
        printDigest("actual-png-output", prepared.identity()->digest(),
                    prepared.identity()->preimageByteCount());
    }
    expectations.expect(
        prepared.identity() != nullptr &&
            test::hasDigest(&prepared.identity()->digest(), kExpectedPngOutputDigest) &&
            prepared.identity()->preimageByteCount() == kExpectedPngPreimageBytes,
        "PNG exact preimage length and digest match the independent golden vector");

    bool monotonic = !progress.empty();
    for (std::size_t index = 1; index < progress.size(); ++index) {
        const auto previousStage = static_cast<std::uint8_t>(progress[index - 1].stage);
        const auto stage = static_cast<std::uint8_t>(progress[index].stage);
        monotonic = monotonic && stage >= previousStage &&
                    progress[index].completed <= progress[index].total;
        if (stage == previousStage) {
            monotonic = monotonic && progress[index].completed >= progress[index - 1].completed &&
                        progress[index].total == progress[index - 1].total;
        }
    }
    expectations.expect(
        monotonic &&
            progress.front() ==
                output::OutputSemanticIdentityProgressV1{
                    .stage = output::OutputSemanticIdentityProgressStageV1::Preflight,
                    .completed = 0,
                    .total = 1} &&
            progress.back() ==
                output::OutputSemanticIdentityProgressV1{
                    .stage = output::OutputSemanticIdentityProgressStageV1::Publishing,
                    .completed = 1,
                    .total = 1},
        "output-identity progress is monotonic across closed stages");
}

void testExrGoldenBitsAndMalformedPayload(test::Expectations& expectations) {
    const auto frame = test::evaluateTinyFrame({-0.0, -1.0, 10.0, 1.0});
    const auto process = test::prepareIdentity(frame);
    const auto report = exrReport(process);
    auto bound = bindExr(process, report);
    const auto bits = processComponentBits(*process);
    expectations.expect(
        bits == std::vector<std::uint32_t>{0x80000000U, 0xBF800000U, 0x41200000U, 0x3F800000U},
        "EXR fixture carries exact signed-zero, negative, HDR, and alpha bits");

    const output::OutputSemanticIdentityV1Preparer preparer;
    const auto prepared = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct = verifiedExrForTest(bound, exrMetadata(*process), std::vector(bits))},
        {});
    expectations.expect(
        prepared.identity() != nullptr &&
            prepared.identity()->payloadKind() ==
                output::OutputSemanticPayloadKindV1::FlatExrRgba32f &&
            prepared.identity()->displayProcessorIdentity() == nullptr &&
            std::ranges::equal(prepared.identity()->flatExrRgba32fComponentBits(), bits),
        "EXR publication retains exact component bits and structurally no display");
    if (prepared.identity() != nullptr &&
        (!test::hasDigest(&prepared.identity()->digest(), kExpectedExrOutputDigest) ||
         prepared.identity()->preimageByteCount() != kExpectedExrPreimageBytes)) {
        printDigest("actual-exr-output", prepared.identity()->digest(),
                    prepared.identity()->preimageByteCount());
    }
    expectations.expect(
        prepared.identity() != nullptr &&
            test::hasDigest(&prepared.identity()->digest(), kExpectedExrOutputDigest) &&
            prepared.identity()->preimageByteCount() == kExpectedExrPreimageBytes,
        "EXR exact preimage length and digest match the independent golden vector");

    auto malformed = bits;
    malformed[0] = 0x7FC00000U;
    const auto invalid = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct = verifiedExrForTest(bound, exrMetadata(*process), std::move(malformed))},
        {});
    expectations.expect(
        invalid.identity() == nullptr &&
            invalid.error() == output::OutputSemanticIdentityErrorCodeV1::InvalidSemanticPayload,
        "non-finite reopened EXR bits are diagnosed as payload failure, not process failure");

    auto noncanonical = bits;
    noncanonical = {0x80000000U, 0U, 0U, 0U};
    const auto invalidTransparent = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct =
             verifiedExrForTest(bound, exrMetadata(*process), std::move(noncanonical))},
        {});
    expectations.expect(invalidTransparent.identity() == nullptr &&
                            invalidTransparent.error() ==
                                output::OutputSemanticIdentityErrorCodeV1::InvalidSemanticPayload,
                        "noncanonical transparent reopened EXR bits publish no identity");
}

[[nodiscard]] std::shared_ptr<const runtime::ProcessFrame>
tinyFrameWithPixelAspect(const core::PixelAspectRatio pixelAspect) {
    const auto format = document::CompositionFormat::create(1, 1, pixelAspect);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        test::kSolidNodeId, test::kColorParameterId, {0.25, 0.5, 0.75, 1.0}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        test::kLayerNodeId, test::kLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{test::kPositionParameterId, document::Vec2d{0.5, 0.5}},
        runtime::CompiledScalarParameter{test::kOpacityParameterId, 1.0}});
    operations.emplace_back(runtime::CompiledLayerStack{
        test::kStackNodeId,
        {{test::kLayerSlotId, test::kLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(runtime::CompiledCompositionOutput{
        test::kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            .sourceRevision = test::kRevision,
            .projectId = test::kProjectId,
            .compositionId = test::kCompositionId,
            .format = *format,
            .operations = std::move(operations),
            .output = runtime::OperationIndex::fromRaw(3),
        });
    const runtime::EvaluationRequest request{
        .time = core::RationalTime::fromInteger(0),
        .output = plan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = 1U << 20U,
    };
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(plan, request, {});
    if (evaluated.frame() == nullptr) {
        std::abort();
    }
    return evaluated.frame();
}

void testWindowAndRelationshipBoundaries(test::Expectations& expectations) {
    const output::OutputSemanticIdentityV1Preparer preparer;
    const auto missingBinding = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct = verifiedPngForTest({}, extent(1, 1), std::vector<std::uint8_t>(4, 0))},
        {});
    expectations.expect(
        missingBinding.identity() == nullptr &&
            missingBinding.error() ==
                output::OutputSemanticIdentityErrorCodeV1::MissingBoundAnalysis,
        "an output identity cannot be prepared without one exact bound analysis product");

    for (const auto origin :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        const auto pixelCount = output::detail::flatExrInclusiveWindowPixelCountV1(
            {.xMin = origin, .yMin = origin, .xMax = origin, .yMax = origin});
        expectations.expect(pixelCount == 1U,
                            "inclusive signed-32 one-pixel EXR window boundary is counted exactly");
    }

    const std::array aspectBoundaries{
        std::pair{core::PixelAspectRatio::create(1, std::numeric_limits<std::uint32_t>::max()),
                  0x2F800000U},
        std::pair{core::PixelAspectRatio::create(std::numeric_limits<std::uint32_t>::max(), 1),
                  0x4F800000U},
    };
    for (const auto& [aspect, roundedBits] : aspectBoundaries) {
        if (!aspect) {
            std::abort();
        }
        const auto boundaryProcess = test::prepareIdentity(tinyFrameWithPixelAspect(*aspect));
        const auto boundaryBound = bindExr(boundaryProcess, exrReport(boundaryProcess));
        const auto prepared = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
            {.verifiedProduct =
                 verifiedExrForTest(boundaryBound, exrMetadata(*boundaryProcess, roundedBits),
                                    processComponentBits(*boundaryProcess))},
            {});
        expectations.expect(
            prepared.identity() != nullptr,
            "minimum/maximum uint32 EXR PAR terms round to the exact frozen binary32 bits");
    }

    const auto process = test::prepareIdentity(test::evaluateTinyFrame());
    const auto exrBound = bindExr(process, exrReport(process));
    auto wrongWindow = exrMetadata(*process);
    wrongWindow.dataWindow.xMax = 1;
    auto bits = processComponentBits(*process);
    bits.insert(bits.end(), bits.begin(), bits.end());
    const auto mismatchedWindow = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct = verifiedExrForTest(exrBound, wrongWindow, std::move(bits))}, {});
    expectations.expect(
        mismatchedWindow.identity() == nullptr &&
            mismatchedWindow.error() ==
                output::OutputSemanticIdentityErrorCodeV1::ProcessDescriptorMismatch,
        "a self-consistent EXR payload/window cannot substitute different bound process semantics");

    auto invalidWindowMetadata = exrMetadata(*process);
    invalidWindowMetadata.dataWindow.xMin = 1;
    invalidWindowMetadata.dataWindow.xMax = 0;
    const auto invalidWindow = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct =
             verifiedExrForTest(exrBound, invalidWindowMetadata, processComponentBits(*process))},
        {});
    expectations.expect(
        invalidWindow.identity() == nullptr &&
            invalidWindow.error() == output::OutputSemanticIdentityErrorCodeV1::InvalidWindow,
        "an inverted verifier-issued EXR window is rejected before payload hashing");

    auto wrongAspect = exrMetadata(*process);
    wrongAspect.pixelAspectRatioBits = 0x3F000000U;
    const auto mismatchedAspect = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
        {.verifiedProduct =
             verifiedExrForTest(exrBound, wrongAspect, processComponentBits(*process))},
        {});
    expectations.expect(
        mismatchedAspect.identity() == nullptr &&
            mismatchedAspect.error() ==
                output::OutputSemanticIdentityErrorCodeV1::InvalidPixelAspectRatio,
        "EXR PAR bits must equal the deterministic rounding of the bound process rational");

    const auto pngBound = bindPng(process, pngReport(process), displayIdentity());
    const auto wrongDimensions = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct =
             verifiedPngForTest(pngBound, extent(2, 1), std::vector<std::uint8_t>(8, 0))},
        {});
    expectations.expect(
        wrongDimensions.identity() == nullptr &&
            wrongDimensions.error() ==
                output::OutputSemanticIdentityErrorCodeV1::ProcessDescriptorMismatch,
        "PNG dimensions cannot substitute the exact bound process descriptor");

    const auto wrongPayloadSize = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct =
             verifiedPngForTest(pngBound, extent(1, 1), std::vector<std::uint8_t>(3, 0))},
        {});
    expectations.expect(
        wrongPayloadSize.identity() == nullptr &&
            wrongPayloadSize.error() ==
                output::OutputSemanticIdentityErrorCodeV1::PayloadSizeMismatch,
        "a verifier-issued PNG product with inconsistent dimensions and storage is rejected");

    bool oversizedReachedHashing = false;
    const auto oversized = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct = verifiedPngForTest(
             pngBound, extent(output::kOutputAnalysisMaximumDimensionV1 + 1U, 1), {})},
        {}, [&oversizedReachedHashing](const output::OutputSemanticIdentityProgressV1& progress) {
            oversizedReachedHashing =
                oversizedReachedHashing ||
                progress.stage ==
                    output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload;
        });
    expectations.expect(oversized.identity() == nullptr &&
                            oversized.error() ==
                                output::OutputSemanticIdentityErrorCodeV1::ResourceLimitExceeded &&
                            !oversizedReachedHashing,
                        "PNG resource limits are rejected during preflight before hashing begins");
}

void testThrowingProgress(test::Expectations& expectations) {
    const auto process = test::prepareIdentity(test::evaluateTinyFrame());
    const auto pngBound = bindPng(process, pngReport(process), displayIdentity());

    std::size_t throwingCallbackCalls = 0;
    const output::OutputSemanticIdentityV1Preparer preparer;
    const auto preparedDespiteThrowingProgress = preparer.preparePngRgba8SrgbV1(
        {.verifiedProduct =
             verifiedPngForTest(pngBound, extent(1, 1), {0x00U, 0x7FU, 0x80U, 0xFFU})},
        {}, [&throwingCallbackCalls](const output::OutputSemanticIdentityProgressV1&) {
            ++throwingCallbackCalls;
            throw std::runtime_error("test progress callback failure");
        });
    expectations.expect(
        throwingCallbackCalls > 0 && preparedDespiteThrowingProgress.identity() != nullptr &&
            preparedDespiteThrowingProgress.status() ==
                output::OutputSemanticIdentityPreparationStatusV1::Prepared &&
            test::hasDigest(&preparedDespiteThrowingProgress.identity()->digest(),
                            kExpectedPngOutputDigest),
        "throwing progress monitoring cannot change PNG identity preparation or its digest");
}

void testCancellationAndAllocationFailure(test::Expectations& expectations) {
    const auto frame = test::evaluateTinyFrame({0.25, 0.5, 0.75, 1.0});
    const auto process = test::prepareIdentity(frame);
    const auto bound = bindExr(process, exrReport(process));
    const auto allocationFailure =
        output::detail::prepareFlatExrOutputSemanticIdentityV1WithAllocationFailure(
            {.verifiedProduct =
                 verifiedExrForTest(bound, exrMetadata(*process), processComponentBits(*process))},
            {});
    expectations.expect(allocationFailure.identity() == nullptr &&
                            allocationFailure.error() ==
                                output::OutputSemanticIdentityErrorCodeV1::AllocationFailure,
                        "publication allocation failure exposes no partial output identity");

    const auto pngBound = bindPng(process, pngReport(process), displayIdentity());
    const auto pngAllocationFailure =
        output::detail::preparePngOutputSemanticIdentityV1WithAllocationFailure(
            {.verifiedProduct =
                 verifiedPngForTest(pngBound, extent(1, 1), {0x00U, 0x7FU, 0x80U, 0xFFU})},
            {});
    expectations.expect(pngAllocationFailure.identity() == nullptr &&
                            pngAllocationFailure.error() ==
                                output::OutputSemanticIdentityErrorCodeV1::AllocationFailure,
                        "PNG publication allocation failure exposes no partial output identity");

    const auto largeFrame = [&] {
        const auto plan = test::planFor(64, 64, {0.25, 0.5, 0.75, 1.0});
        const runtime::EvaluationRequest request{
            .time = core::RationalTime::fromInteger(0),
            .output = plan->output(),
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = 1U << 20U,
        };
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated = evaluator.evaluate(plan, request, {});
        if (evaluated.frame() == nullptr) {
            std::abort();
        }
        return evaluated.frame();
    }();
    const auto largeProcess = test::prepareIdentity(largeFrame);
    const auto largeBound = bindExr(largeProcess, exrReport(largeProcess));
    auto largeBits = processComponentBits(*largeProcess);
    const auto largeMetadata = exrMetadata(*largeProcess);

    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool reachedChunk = false;
    bool released = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool initiallyCancelled = false;
    std::atomic_bool published = false;
    const output::OutputSemanticIdentityV1Preparer preparer;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Output semantic identity cancellation",
            {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [process, bound, pngBound, largeBound, largeMetadata, largeBits, &mutex, &condition,
         &reachedChunk, &released, &cancelled, &initiallyCancelled, &published,
         &preparer](runtime::TaskContext& context) mutable {
            const auto result = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
                {.verifiedProduct =
                     verifiedExrForTest(largeBound, largeMetadata, std::move(largeBits))},
                context.cancellation(),
                [&mutex, &condition, &reachedChunk,
                 &released](const output::OutputSemanticIdentityProgressV1& update) {
                    if (update.stage !=
                            output::OutputSemanticIdentityProgressStageV1::HashingSemanticPayload ||
                        update.completed != 1024U) {
                        return;
                    }
                    std::unique_lock lock(mutex);
                    reachedChunk = true;
                    condition.notify_all();
                    condition.wait(lock, [&released] { return released; });
                });
            bool initialProgressObserved = false;
            const auto initialProgress =
                [&initialProgressObserved](const output::OutputSemanticIdentityProgressV1&) {
                    initialProgressObserved = true;
                };
            const auto initialPng = preparer.preparePngRgba8SrgbV1(
                {.verifiedProduct =
                     verifiedPngForTest(pngBound, extent(1, 1), std::vector<std::uint8_t>(4, 0))},
                context.cancellation(), initialProgress);
            const auto initialExr = preparer.prepareFlatExrRgba32fLinRec709SceneV1(
                {.verifiedProduct = verifiedExrForTest(bound, exrMetadata(*process),
                                                       processComponentBits(*process))},
                context.cancellation(), initialProgress);
            const bool initialCancellationSucceeded =
                initialPng.status() ==
                    output::OutputSemanticIdentityPreparationStatusV1::Cancelled &&
                initialExr.status() ==
                    output::OutputSemanticIdentityPreparationStatusV1::Cancelled &&
                initialPng.identity() == nullptr && initialExr.identity() == nullptr &&
                !initialProgressObserved;
            initiallyCancelled.store(initialCancellationSucceeded, std::memory_order_release);
            const bool cancellationSucceeded =
                result.status() == output::OutputSemanticIdentityPreparationStatusV1::Cancelled &&
                initialCancellationSucceeded;
            cancelled.store(cancellationSucceeded, std::memory_order_release);
            published.store(result.identity() != nullptr || initialPng.identity() != nullptr ||
                                initialExr.identity() != nullptr,
                            std::memory_order_release);
            return cancellationSucceeded ? runtime::TaskResult<void>::cancelled()
                                         : runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(
            submission.accepted() &&
                condition.wait_for(lock, 2s, [&reachedChunk] { return reachedChunk; }),
            "cancellation fixture reaches a deterministic payload chunk boundary");
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(
        taskResult && taskResult->state() == runtime::TaskState::Cancelled &&
            cancelled.load(std::memory_order_acquire) &&
            initiallyCancelled.load(std::memory_order_acquire) &&
            !published.load(std::memory_order_acquire),
        "chunk and initial cancellation publish no PNG or EXR identity or progress");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "cancellation fixture reaches quiescence");
}

} // namespace

int main() {
    test::Expectations expectations;
    testBoundAnalysisAndPngGolden(expectations);
    testExrGoldenBitsAndMalformedPayload(expectations);
    testWindowAndRelationshipBoundaries(expectations);
    testThrowingProgress(expectations);
    testCancellationAndAllocationFailure(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
