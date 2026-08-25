#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::output::test {

namespace color = bloom::color;
namespace core = bloom::core;
namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

inline constexpr auto kProjectId = document::ProjectId::fromRaw(1);
inline constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
inline constexpr auto kSolidNodeId = document::NodeId::fromRaw(3);
inline constexpr auto kOutputNodeId = document::NodeId::fromRaw(4);
inline constexpr auto kColorParameterId = document::ParameterId::fromRaw(5);
inline constexpr auto kRevision = document::Revision::fromRaw(6);
inline constexpr auto kLayerNodeId = document::NodeId::fromRaw(7);
inline constexpr auto kStackNodeId = document::NodeId::fromRaw(8);
inline constexpr auto kLayerId = document::LayerId::fromRaw(9);
inline constexpr auto kLayerSlotId = document::LayerSlotId::fromRaw(10);
inline constexpr auto kPositionParameterId = document::ParameterId::fromRaw(11);
inline constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(12);

inline constexpr core::Sha256Digest::Bytes kOcioRevisionBytes{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

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

[[nodiscard]] inline render::ImageWindow window(const std::int64_t originX,
                                                const std::int64_t originY,
                                                const std::uint32_t width,
                                                const std::uint32_t height) {
    const auto result = render::ImageWindow::create(originX, originY, width, height);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

[[nodiscard]] inline render::Rgba32fImageDescriptor
descriptor(const render::ImageWindow dataWindow, const render::ImageWindow displayWindow,
           const core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square()) {
    const auto result =
        render::Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

[[nodiscard]] inline render::Rgba32fImageDescriptor descriptor(const std::uint32_t width = 1,
                                                               const std::uint32_t height = 1) {
    const auto zeroWindow = window(0, 0, width, height);
    return descriptor(zeroWindow, zeroWindow);
}

[[nodiscard]] inline OutputAnalysisProcessSourceV1
missingSource(const render::Rgba32fImageDescriptor& sourceDescriptor) {
    return {.state = OutputAnalysisProcessSourceStateV1::Missing,
            .readyIdentity = {},
            .missingDescriptor = sourceDescriptor};
}

[[nodiscard]] inline PngRgba8SrgbAnalysisInputV1
pngInput(const render::Rgba32fImageDescriptor& sourceDescriptor) {
    return {.process = missingSource(sourceDescriptor),
            .expectedOcioRevision = core::Sha256Digest::fromBytes(kOcioRevisionBytes)};
}

[[nodiscard]] inline FlatExrRgba32fLinRec709SceneAnalysisInputV1
exrInput(const render::Rgba32fImageDescriptor& sourceDescriptor) {
    return {.process = missingSource(sourceDescriptor)};
}

[[nodiscard]] inline const OutputFacetAssessmentV1View& facet(const OutputAnalysisReportV1& report,
                                                              const OutputFacetIdV1 facetId) {
    const auto reportView = report.view();
    const auto index = static_cast<std::size_t>(static_cast<std::uint8_t>(facetId) - 1U);
    if (index >= reportView.facets.size()) {
        std::abort();
    }
    return reportView.facets[index];
}

[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
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

[[nodiscard]] inline std::shared_ptr<const runtime::ProcessFrame>
evaluateTinyFrame(const core::Color4d colorValue = {0.25, 0.5, 0.75, 1.0}) {
    const auto plan = planFor(1, 1, colorValue);
    const runtime::EvaluationRequest request{
        .time = core::RationalTime::fromInteger(0),
        .output = plan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = 1U << 20U,
    };
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(plan, request, {});
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
        std::abort();
    }
    return result.frame();
}

[[nodiscard]] inline std::shared_ptr<const ProcessFrameSemanticIdentityV1>
prepareIdentity(const std::shared_ptr<const runtime::ProcessFrame>& frame) {
    const ProcessFrameSemanticIdentityV1Preparer preparer;
    const auto result = preparer.prepare(frame, {});
    if (result.status() != ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        result.identity() == nullptr) {
        std::abort();
    }
    return result.identity();
}

[[nodiscard]] inline color::DisplayProcessorIdentityV1InputView
displayInput(const core::Sha256Digest revision) noexcept {
    static constexpr std::array contextVariables{
        color::DisplayProcessorContextVariableV1View{"A", "B"},
    };
    static constexpr std::array<std::string_view, 2> looks{"L", "M"};
    return {.expectedOcioRevision = revision,
            .contextVariables = contextVariables,
            .sourceColorSpaceId = color::kDisplayProcessorIdentitySourceColorSpaceId,
            .displayName = "D",
            .viewName = "V",
            .lookMode = color::DisplayProcessorLookModeV1::Ordered,
            .lookNames = looks,
            .outputColorSpaceId = color::kDisplayProcessorIdentityOutputColorSpaceId,
            .qualityId = color::kDisplayProcessorIdentityQualityId,
            .semanticsProfileId = color::kDisplayProcessorIdentitySemanticsProfileId,
            .packingId = color::kDisplayProcessorIdentityPackingId};
}

[[nodiscard]] inline color::DisplayProcessorIdentityV1
makeDisplayIdentity(const core::Sha256Digest revision) {
    const auto input = displayInput(revision);
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
    auto identity = std::move(adopted).takeIdentity();
    if (!identity) {
        std::abort();
    }
    return std::move(*identity);
}

[[nodiscard]] inline bool hasDigest(const core::Sha256Digest* const digest,
                                    const std::string_view expected) noexcept {
    if (digest == nullptr) {
        return false;
    }
    const auto encoded = digest->toLowercaseHex();
    return std::string_view(encoded.data(), encoded.size()) == expected;
}

void runOutputAnalysisAnalyzerEdgeTests(Expectations& expectations);

} // namespace bloom::output::test
