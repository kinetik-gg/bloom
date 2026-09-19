// Tests for the GPU Neutral display preview product. The adoption/pointer-preservation cases run
// without a device; the qualification-driven product cases pin an explicit loader and require a
// device, and otherwise skip with a clear message (there is deliberately no way to fabricate a
// successful qualification report).

#include <bloom/runtime/gpu_preview_display_product.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::core::RationalTime;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuNeutralDisplay;
using bloom::render::ImageWindow;
using bloom::render::PreparedReferenceDisplayBuffer;
using bloom::render::ReferenceDisplayBufferDescriptor;
using bloom::render::Rgba8;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CompositionFormatResolution;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::EvaluationStatus;
using bloom::runtime::GpuNeutralDisplayQualificationReport;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreviewCpuStage;
using bloom::runtime::PreviewOutput;
using bloom::runtime::PreviewRequestIdentity;
using bloom::runtime::PreviewResolutionPolicy;
using bloom::runtime::ProcessFrame;
using bloom::runtime::ViewAdjust;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
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

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<ReferenceDisplayBufferDescriptor>
makeDescriptor(const std::uint32_t width, const std::uint32_t height) {
    const auto window = ImageWindow::create(0, 0, width, height);
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor =
        ReferenceDisplayBufferDescriptor::create(*window.value(), PixelAspectRatio::square());
    if (!descriptor) {
        return std::nullopt;
    }
    return *descriptor.value();
}

void testAdoptRejectsSizeMismatchAndPreservesVector(Expectations& expectations) {
    const auto descriptor = makeDescriptor(2, 2);
    expectations.expect(descriptor.has_value(), "a 2x2 descriptor builds");
    if (!descriptor.has_value()) {
        return;
    }
    std::vector<Rgba8> pixels(3, Rgba8{1, 2, 3, 4});
    const Rgba8* const originalData = pixels.data();
    auto result = PreparedReferenceDisplayBuffer::adopt(*descriptor, std::move(pixels), 1U << 20U);
    expectations.expect(!result.hasValue(), "a wrong pixel count is rejected");
    expectations.expect(pixels.size() == 3 && pixels.data() == originalData,
                        "a rejected adoption leaves the caller's vector owned and unchanged");
}

void testAdoptRejectsBudgetAndPreservesVector(Expectations& expectations) {
    const auto descriptor = makeDescriptor(4, 4);
    expectations.expect(descriptor.has_value(), "a 4x4 descriptor builds");
    if (!descriptor.has_value()) {
        return;
    }
    std::vector<Rgba8> pixels(16, Rgba8{9, 8, 7, 6});
    const Rgba8* const originalData = pixels.data();
    auto result = PreparedReferenceDisplayBuffer::adopt(*descriptor, std::move(pixels), 8U);
    expectations.expect(!result.hasValue(), "an over-budget adoption is rejected");
    expectations.expect(pixels.size() == 16 && pixels.data() == originalData,
                        "a budget-rejected adoption leaves the caller's vector owned");
}

void testAdoptMovesPayloadAndPreservesPointer(Expectations& expectations) {
    const auto descriptor = makeDescriptor(4, 4);
    expectations.expect(descriptor.has_value(), "a 4x4 descriptor builds");
    if (!descriptor.has_value()) {
        return;
    }
    std::vector<Rgba8> pixels(16, Rgba8{5, 4, 3, 2});
    // The minimal adoption policy requires capacity == size; make that exact so the move path is
    // the one under test rather than the capacity rejection.
    pixels.shrink_to_fit();
    const Rgba8* const originalData = pixels.data();
    auto result = PreparedReferenceDisplayBuffer::adopt(*descriptor, std::move(pixels), 1U << 20U);
    expectations.expect(result.hasValue(), "a valid adoption succeeds");
    if (!result.hasValue()) {
        return;
    }
    expectations.expect(result.value()->pixels().data() == originalData,
                        "adoption moves the payload rather than copying it");
    expectations.expect(result.value()->pixels().size() == 16 &&
                            result.value()->descriptor() != nullptr,
                        "the adopted buffer keeps the payload size and descriptor");
}

void testAdoptRejectsExcessReserveAndPreservesVector(Expectations& expectations) {
    const auto descriptor = makeDescriptor(4, 4);
    expectations.expect(descriptor.has_value(), "a 4x4 descriptor builds");
    if (!descriptor.has_value()) {
        return;
    }
    std::vector<Rgba8> pixels;
    pixels.reserve(64);
    for (int index = 0; index < 16; ++index) {
        pixels.push_back(Rgba8{static_cast<std::uint8_t>(index), 1, 2, 3});
    }
    expectations.expect(pixels.size() == 16 && pixels.capacity() > pixels.size(),
                        "the fixture vector owns spare capacity");
    const Rgba8* const originalData = pixels.data();
    const std::size_t originalCapacity = pixels.capacity();
    auto result = PreparedReferenceDisplayBuffer::adopt(*descriptor, std::move(pixels), 1U << 20U);
    expectations.expect(!result.hasValue(), "a vector with excess capacity is rejected");
    expectations.expect(pixels.size() == 16 && pixels.data() == originalData &&
                            pixels.capacity() == originalCapacity,
                        "an excess-reserve rejection leaves the caller's vector untouched");
}

// --- Device-gated product fixture -------------------------------------------------------------

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStackNode = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutputNode = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);
constexpr auto kColorParam = bloom::document::ParameterId::fromRaw(40);
constexpr auto kPositionParam = bloom::document::ParameterId::fromRaw(41);
constexpr auto kOpacityParam = bloom::document::ParameterId::fromRaw(42);
constexpr auto kAnchorParam = bloom::document::ParameterId::fromRaw(43);
constexpr auto kScaleParam = bloom::document::ParameterId::fromRaw(44);
constexpr auto kRotationParam = bloom::document::ParameterId::fromRaw(45);
constexpr auto kBlendModeParam = bloom::document::ParameterId::fromRaw(46);

[[nodiscard]] bloom::document::CompositionFormat format(const std::uint32_t width,
                                                        const std::uint32_t height) {
    const auto value = bloom::document::CompositionFormat::create(width, height);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
oneSolidPlan(const bloom::document::CompositionFormat compositionFormat) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{kSolidNode,
                      {kColorParam, Color4d{1.0, 0.0, 0.0, 1.0}},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                       static_cast<double>(compositionFormat.width())},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                       static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(CompiledLayerOutput{
        kLayerNode, kLayer, OperationIndex::fromRaw(0),
        CompiledVec2Parameter{kPositionParam, bloom::document::Vec2d{2.0, 1.0}},
        CompiledVec2Parameter{kAnchorParam, bloom::document::kDefaultAnchor},
        CompiledVec2Parameter{kScaleParam, bloom::document::kDefaultScale},
        CompiledScalarParameter{kRotationParam, bloom::document::kDefaultRotationDegrees},
        CompiledScalarParameter{kOpacityParam, 1.0}, kBlendModeParam,
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(
        CompiledMerge{kStackNode, {{kSlot, kLayer, OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{kOutputNode, OperationIndex::fromRaw(2)});
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildNeutralHandle() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

[[nodiscard]] std::shared_ptr<const ProcessFrame>
evaluateFrame(const std::shared_ptr<const CompiledCompositionPlan>& plan,
              const std::size_t budget) {
    CpuCompositionEvaluator evaluator;
    auto result = evaluator.evaluate(plan,
                                     {.time = RationalTime::fromInteger(0),
                                      .output = plan->output(),
                                      .resolution = CompositionFormatResolution{},
                                      .quality = EvaluationQuality::Reference,
                                      .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                      .pixelStorageByteLimit = budget},
                                     bloom::runtime::CancellationToken{});
    if (result.status() != EvaluationStatus::Evaluated) {
        return nullptr;
    }
    return result.frame();
}

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
chooseEligibleSize(const GpuNeutralDisplayQualificationReport& report) {
    const auto& interval = report.eligibleInterval();
    if (!interval.has_value()) {
        return std::nullopt;
    }
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> kCandidates{
        {{256, 144}, {960, 540}, {1280, 720}, {1920, 1080}, {3840, 2160}}};
    for (const auto& [width, height] : kCandidates) {
        const auto pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels >= interval->min_pixels && pixels <= interval->max_pixels) {
            return std::make_pair(width, height);
        }
    }
    return std::nullopt;
}

void testQualifiedNativeProduct(
    Expectations& expectations, GpuDevice& device, GpuNeutralDisplay& pipeline,
    const std::shared_ptr<const PreparedCpuDisplayProcessorHandle>& sharedProcessor) {
    auto report = std::make_shared<GpuNeutralDisplayQualificationReport>(
        bloom::runtime::qualifyGpuNeutralDisplay(*sharedProcessor, device, pipeline));
    expectations.expect(report->eligible(), "the device qualifies for the fixed operation");
    if (!report->eligible()) {
        std::cout << "SKIP: qualification measured no eligible interval on this device\n";
        return;
    }
    const auto size = chooseEligibleSize(*report);
    expectations.expect(size.has_value(),
                        "a measured eligible size exists inside the report interval");
    if (!size.has_value()) {
        return;
    }
    constexpr std::size_t kBudget = std::size_t{1} << 28;

    const auto plan = oneSolidPlan(format(size->first, size->second));
    std::shared_ptr<const ProcessFrame> processFrame = evaluateFrame(plan, kBudget);
    expectations.expect(processFrame != nullptr, "the eligible-size ProcessFrame evaluates");
    if (processFrame == nullptr) {
        return;
    }
    const std::weak_ptr<const ProcessFrame> processLifetime = processFrame;

    const auto identity =
        PreviewRequestIdentity{.projectId = plan->projectId(),
                               .compositionId = plan->compositionId(),
                               .sourceRevision = plan->sourceRevision(),
                               .requestGeneration = 1,
                               .time = RationalTime::fromInteger(0),
                               .output = PreviewOutput::Composition,
                               .resolution = CompositionFormatResolution{},
                               .quality = EvaluationQuality::Reference,
                               .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                               .resolutionPolicy = PreviewResolutionPolicy::Auto,
                               .viewAdjust = ViewAdjust{},
                               .displayName = {},
                               .viewName = {},
                               .showLook = true};
    std::optional<bloom::runtime::PreparedPreviewFrame> product;
    {
        PreviewCpuStage stage(identity, processFrame, sharedProcessor, kBudget, {});
        std::string reason;
        expectations.expect(
            bloom::runtime::gpuNeutralDisplayStageIsEligible(stage, *report, reason),
            "the stage is eligible: " + reason);

        const auto begin = pipeline.begin(processFrame->processImage().pixels(), kBudget);
        expectations.expect(begin.code == bloom::render::GpuNeutralDisplayDiagnosticCode::None,
                            "the native dispatch is accepted: " + begin.message);
        for (int spin = 0; spin < 1'000'000; ++spin) {
            const auto poll = pipeline.poll();
            if (poll == bloom::render::GpuNeutralDisplayPollResult::Ready) {
                break;
            }
            if (poll == bloom::render::GpuNeutralDisplayPollResult::Failure) {
                break;
            }
        }
        auto readback = pipeline.readback();
        expectations.expect(readback.hasValue(), "the native readback succeeds");
        if (!readback.hasValue()) {
            return;
        }
        const Rgba8* const adoptedData = readback.pixels.data();
        const std::size_t expectedPixels = static_cast<std::size_t>(size->first) * size->second;
        expectations.expect(readback.pixels.size() == expectedPixels,
                            "the native readback covers exactly the requested pixels");

        product = bloom::runtime::makeGpuNeutralDisplayPreview(stage, report, std::move(readback));
        expectations.expect(product.has_value(), "the eligible native result wraps into a product");
        if (!product.has_value()) {
            return;
        }
        const auto displayOnly = product->displayOnlyFrame();
        expectations.expect(displayOnly != nullptr, "the product is a display-only frame");
        if (displayOnly == nullptr) {
            return;
        }
        expectations.expect(displayOnly->provenance().provider ==
                                bloom::runtime::PreviewDisplayProvider::GpuNeutral,
                            "the product carries GPU Neutral provenance");
        expectations.expect(displayOnly->provenance().gpuQualification == report,
                            "the product retains exactly the dispatch's qualification report");
        expectations.expect(displayOnly->isOcioQualified(),
                            "the product is the qualified display product");
        expectations.expect(displayOnly->desiredIdentity() == identity,
                            "the product preserves the request identity");
        expectations.expect(displayOnly->processIdentity() == processFrame->identity(),
                            "the product preserves the process identity and plan");
        expectations.expect(displayOnly->evaluatedBounds().size() ==
                                processFrame->evaluatedBounds().size(),
                            "the product preserves evaluated geometry");
        const auto view = displayOnly->displayBufferView();
        expectations.expect(view.has_value() && view->pixels.data() == adoptedData,
                            "the adopted packed pointer is unchanged through the envelope");
        expectations.expect(view.has_value() &&
                                view->displayWindow ==
                                    processFrame->processImage().descriptor()->displayWindow(),
                            "the product's display window is the process display window");

        // A typed native failure publishes nothing.
        bloom::render::GpuNeutralDisplayReadback failed;
        failed.pixels.assign(expectedPixels, Rgba8{0, 0, 0, 0});
        failed.diagnostic.code = bloom::render::GpuNeutralDisplayDiagnosticCode::ShaderRejected;
        failed.diagnostic.message = "test forced native rejection";
        expectations.expect(
            !bloom::runtime::makeGpuNeutralDisplayPreview(stage, report, std::move(failed))
                 .has_value(),
            "a typed native failure is rejected");

        // A stage whose process frame is null (the public constructor permits it) must be rejected
        // by the authoritative eligibility check BEFORE any dereference, and the passed readback's
        // bytes must be preserved because no adoption happened.
        PreviewCpuStage nullStage(identity, nullptr, sharedProcessor, kBudget, {});
        std::string nullReason;
        expectations.expect(
            !bloom::runtime::gpuNeutralDisplayStageIsEligible(nullStage, *report, nullReason),
            "a null process frame is ineligible");
        bloom::render::GpuNeutralDisplayReadback preserved;
        preserved.pixels.assign(expectedPixels, Rgba8{7, 6, 5, 4});
        const Rgba8* const preservedData = preserved.pixels.data();
        const std::size_t preservedSize = preserved.pixels.size();
        expectations.expect(
            !bloom::runtime::makeGpuNeutralDisplayPreview(nullStage, report, std::move(preserved))
                 .has_value(),
            "a null-process stage is rejected without dereferencing it");
        expectations.expect(preserved.pixels.size() == preservedSize &&
                                preserved.pixels.data() == preservedData,
                            "a null-process rejection preserves the caller's readback bytes");
    }

    processFrame.reset();
    expectations.expect(processLifetime.expired(),
                        "the product retains no ProcessFrame after the stage is released");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }

        Expectations expectations;
        testAdoptRejectsSizeMismatchAndPreservesVector(expectations);
        testAdoptRejectsBudgetAndPreservesVector(expectations);
        testAdoptMovesPayloadAndPreservesPointer(expectations);
        testAdoptRejectsExcessReserveAndPreservesVector(expectations);

        auto processor = buildNeutralHandle();
        expectations.expect(processor.has_value(), "the default Bloom Neutral processor builds");
        if (!processor.has_value()) {
            return 1;
        }

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message
                      << "; the adoption/pointer checks above still passed\n";
            return expectations.failures() == 0 ? 0 : 1;
        }

        auto pipeline = GpuNeutralDisplay::create(*device.device);
        expectations.expect(pipeline.hasValue(), "the native display pipeline is created");
        if (!pipeline) {
            std::cerr << "FAIL: pipeline creation failed: " << pipeline.diagnostic.message << '\n';
            return 1;
        }
        // The handle is move-only, exactly as the provider publishes it; move it once into the
        // shared owner the stage retains.
        auto sharedProcessor =
            std::make_shared<const PreparedCpuDisplayProcessorHandle>(std::move(*processor));
        testQualifiedNativeProduct(expectations, *device.device, *pipeline.display,
                                   sharedProcessor);

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " GPU product expectation(s) failed\n";
            return 1;
        }
        std::cout << "GPU product: all expectations passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
