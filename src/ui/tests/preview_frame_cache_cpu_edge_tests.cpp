// CPU-only edge tests for the RAM preview cache, independent of any GPU device. They build genuine
// CPU reference display frames with the real CpuCompositionEvaluator + CpuReferenceDisplayPreparer
// and exercise:
//   * CACHEFIX-2 same-key CPU recovery/reuse: a live frame with an already-retained key is reused
//     (one entry, no double byte charge, generation re-stamped on take);
//   * CACHEFIX-3 other-project retention: a standalone cache with no provenance policy keeps
//     another project's entries, while a same-project older revision is still dropped;
//   * dead-entry visibility is a resident concern (covered by the device test).
// It never creates a GpuDevice and never touches Vulkan.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QCoreApplication>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
using bloom::core::Color4d;
using bloom::runtime::PreparedPreviewFrame;
using bloom::runtime::PreviewRequestIdentity;
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (!condition) {
            ++failures_;
            std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStackNode = document::NodeId::fromRaw(12);
constexpr auto kOutputNode = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);

[[nodiscard]] document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(64, 36);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const document::ProjectId projectId = kProjectId,
             const document::Revision revision = document::Revision::fromRaw(7)) {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), Color4d{1.0, 0.0, 0.0, 1.0}},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                                static_cast<double>(compositionFormat.width())},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                                static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{revision, projectId, kCompositionId,
                                                   compositionFormat, std::move(operations),
                                                   runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
cpuFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const std::uint64_t generation) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(0),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr) {
        return nullptr;
    }
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = {},
                             .displayName = {},
                             .viewName = {},
                             .showLook = true},
                         {});
    if (display.frame() == nullptr) {
        return nullptr;
    }
    const auto prepared = PreparedPreviewFrame::create(generation, display.frame());
    if (!prepared.has_value()) {
        return nullptr;
    }
    return std::make_shared<const PreparedPreviewFrame>(std::move(*prepared));
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        Expectations expectations;
        constexpr std::size_t kBudget = std::size_t{1} << 28;

        auto planA = oneSolidPlan();
        auto frameA1 = cpuFrame(planA, 1);
        expectations.expect(frameA1 != nullptr && frameA1->displayBufferView().has_value(),
                            "the CPU reference frame is built with a display buffer");
        if (frameA1 == nullptr) {
            return 1;
        }
        const auto keyA = PreviewFrameCacheKey::forIdentity(frameA1->desiredIdentity());
        const auto costA = PreviewFrameCache::frameByteCost(*frameA1);

        // CACHEFIX-2 CPU path: a live frame with an already-retained key is reused in place, not
        // re-charged. This is the branch the dead-entry fix must not disturb.
        PreviewFrameCache reuseCache(kBudget);
        reuseCache.insert(frameA1);
        expectations.expect(reuseCache.size() == 1 && reuseCache.residentBytes() == costA,
                            "the first CPU insert retains exactly one charged entry");
        auto frameA2 = cpuFrame(planA, 2);
        expectations.expect(frameA2 != nullptr, "the second same-key CPU frame is built");
        if (frameA2 != nullptr) {
            reuseCache.insert(frameA2);
            expectations.expect(reuseCache.size() == 1,
                                "CACHEFIX-2: a live same-key CPU insert keeps one entry");
            expectations.expect(reuseCache.residentBytes() == costA,
                                "CACHEFIX-2: a live same-key CPU insert does not double-charge");
            expectations.expect(reuseCache.contains(keyA),
                                "CACHEFIX-2: contains() still sees the reused CPU entry");
            auto identity = frameA1->desiredIdentity();
            identity.requestGeneration = 3;
            auto taken = reuseCache.take(identity);
            expectations.expect(taken != nullptr && taken->desiredIdentity().requestGeneration == 3,
                                "CACHEFIX-2: take() re-stamps the reused CPU frame's generation");
            expectations.expect(reuseCache.statistics().hits == 1,
                                "CACHEFIX-2: the reused CPU take counts a hit");
        }

        // CACHEFIX-3: a standalone cache with no provenance policy must retain another project's
        // entries. The pre-fix dropStaleRevisions() dropped every entry with a different projectId.
        auto planB = oneSolidPlan(document::ProjectId::fromRaw(999));
        auto frameB = cpuFrame(planB, 1);
        expectations.expect(frameB != nullptr, "the second project's CPU frame is built");
        if (frameB == nullptr) {
            return 1;
        }
        PreviewFrameCache projectCache(kBudget);
        projectCache.insert(frameA1);
        projectCache.insert(frameB);
        expectations.expect(projectCache.size() == 2,
                            "CACHEFIX-3: both projects' CPU frames are retained together");
        expectations.expect(projectCache.contains(keyA),
                            "CACHEFIX-3: a second project's insert preserves the first project");
        expectations.expect(projectCache.timesFor(keyA).size() == 1,
                            "CACHEFIX-3: timesFor() still lists the first project's entry");

        // The fix must NOT weaken same-project revision invalidation: an older-revision entry of
        // the SAME project is still dropped when a newer revision arrives.
        auto planA2 = oneSolidPlan(kProjectId, document::Revision::fromRaw(8));
        auto frameA2rev = cpuFrame(planA2, 1);
        expectations.expect(frameA2rev != nullptr, "the newer same-project CPU frame is built");
        if (frameA2rev != nullptr) {
            PreviewFrameCache revisionCache(kBudget);
            revisionCache.insert(frameA1);
            const auto staleBefore = revisionCache.statistics().staleDrops;
            revisionCache.insert(frameA2rev);
            expectations.expect(revisionCache.size() == 1 &&
                                    revisionCache.statistics().staleDrops == staleBefore + 1,
                                "CACHEFIX-3: same-project older revision is still dropped");
            expectations.expect(!revisionCache.contains(keyA),
                                "CACHEFIX-3: the superseded same-project entry is gone");
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " cache CPU edge expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: preview frame cache CPU edge cases\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected CPU edge test exception: " << exception.what() << '\n';
        return 1;
    }
}
