// Tests for the RAM preview cache's fourth-arm (GPU-resident) retention. On a real Vulkan device
// the fixture produces a genuine Solid -> resident-display -> owner-registry lease and wraps it
// with the real product factory; the cache is then exercised for insertion cost, hit/re-stamp,
// contains()/timesFor() visibility, and the invalidation rule: an invalidated lease must never be a
// hit, never listed, and never inserted. Without a device the test skips cleanly.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_resident_preview_product.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QCoreApplication>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::core::RationalTime;
using bloom::render::GpuComposite;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuSolid;
using bloom::render::ImageWindow;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CompositionFormatResolution;
using bloom::runtime::ContentBounds;
using bloom::runtime::EvaluatedOperationBounds;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationProvider;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::GpuResidentDisplayProductRequest;
using bloom::runtime::GpuResidentFrameLeaseRegistry;
using bloom::runtime::GpuResidentPreviewPipelines;
using bloom::runtime::GpuResidentPreviewQualificationReport;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedPreviewFrame;
using bloom::runtime::PreviewOutput;
using bloom::runtime::PreviewRequestIdentity;
using bloom::runtime::PreviewResolutionPolicy;
using bloom::runtime::ProcessFrameIdentity;
using bloom::runtime::ViewAdjust;
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
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildCanonicalProcessor() {
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

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStackNode = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutputNode = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);

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
                      {bloom::document::ParameterId::fromRaw(40), Color4d{1.0, 0.0, 0.0, 1.0}},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                       static_cast<double>(compositionFormat.width())},
                      {bloom::document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                       static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(CompiledLayerOutput{
        kLayerNode, kLayer, OperationIndex::fromRaw(0),
        CompiledVec2Parameter{bloom::document::ParameterId::fromRaw(41),
                              bloom::document::Vec2d{2.0, 1.0}},
        CompiledVec2Parameter{bloom::document::ParameterId::fromRaw(43),
                              bloom::document::kDefaultAnchor},
        CompiledVec2Parameter{bloom::document::ParameterId::fromRaw(44),
                              bloom::document::kDefaultScale},
        CompiledScalarParameter{bloom::document::ParameterId::fromRaw(45),
                                bloom::document::kDefaultRotationDegrees},
        CompiledScalarParameter{bloom::document::ParameterId::fromRaw(42), 1.0},
        bloom::document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(
        CompiledMerge{kStackNode, {CompiledMergeInput{kSlot, kLayer, OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{kOutputNode, OperationIndex::fromRaw(2)});
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
chooseEligibleSize(const GpuResidentPreviewQualificationReport& report) {
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

struct PipelineSet final {
    std::unique_ptr<GpuSolid> solid;
    std::unique_ptr<GpuImageUpload> upload;
    std::unique_ptr<GpuComposite> composite;
    std::unique_ptr<GpuResidentDisplay> display;

    [[nodiscard]] bool valid() const noexcept {
        return solid != nullptr && upload != nullptr && composite != nullptr && display != nullptr;
    }
    [[nodiscard]] GpuResidentPreviewPipelines view() noexcept {
        return GpuResidentPreviewPipelines{solid.get(), upload.get(), composite.get(),
                                           display.get()};
    }
};

[[nodiscard]] PipelineSet createPipelines(GpuDevice& device) {
    PipelineSet set;
    set.solid = std::move(GpuSolid::create(device).solid);
    set.upload = std::move(GpuImageUpload::create(device).upload);
    set.composite = std::move(GpuComposite::create(device).composite);
    set.display = std::move(GpuResidentDisplay::create(device).display);
    return set;
}

[[nodiscard]] std::shared_ptr<const GpuDisplayImage>
produceResidentDisplay(GpuDevice& device, GpuSolid& solid, GpuResidentDisplay& display,
                       const std::uint32_t width, const std::uint32_t height,
                       const std::uint64_t budget) {
    const auto windowResult = ImageWindow::create(0, 0, width, height);
    if (!windowResult.hasValue()) {
        return nullptr;
    }
    const auto& window = *windowResult.value();
    const auto pixelResult =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{1.0, 0.25, 0.0, 1.0});
    if (!pixelResult.hasValue()) {
        return nullptr;
    }
    if (solid
            .begin({.pixel = *pixelResult.value(),
                    .dataWindow = window,
                    .displayWindow = window,
                    .pixelAspect = PixelAspectRatio::square()},
                   budget)
            .code != bloom::render::GpuSolidDiagnosticCode::None) {
        return nullptr;
    }
    for (int spin = 0; spin < 1'000'000; ++spin) {
        const auto poll = solid.poll();
        if (poll == bloom::render::GpuSolidPollResult::Ready) {
            break;
        }
        if (poll == bloom::render::GpuSolidPollResult::Failure) {
            return nullptr;
        }
    }
    GpuImage processImage = solid.takeImage();
    if (!processImage.isValid() || !processImage.isBoundTo(device)) {
        return nullptr;
    }
    auto sharedProcess = std::make_shared<const GpuImage>(std::move(processImage));
    if (display.begin(sharedProcess, budget).code !=
        bloom::render::GpuResidentDisplayDiagnosticCode::None) {
        return nullptr;
    }
    for (int spin = 0; spin < 1'000'000; ++spin) {
        const auto poll = display.poll();
        if (poll == bloom::render::GpuResidentDisplayPollResult::Ready) {
            break;
        }
        if (poll == bloom::render::GpuResidentDisplayPollResult::Failure) {
            return nullptr;
        }
    }
    GpuDisplayImage image = display.takeImage();
    if (!image.isValid() || !image.isBoundTo(device)) {
        return nullptr;
    }
    return std::make_shared<const GpuDisplayImage>(std::move(image));
}

} // namespace

int main(int argc, char** argv) {
    // PreviewFrameCache owns a QTimer, so an application instance must exist before the cache is
    // constructed; without one Qt warns that timers need a QThread-started event dispatcher.
    QCoreApplication application(argc, argv);
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "usage: --loader <path> [--require-device]\n";
            return 2;
        }
        Expectations expectations;

        auto processor = buildCanonicalProcessor();
        expectations.expect(processor.has_value(), "the canonical processor builds");
        if (!processor.has_value()) {
            return 1;
        }
        auto sharedProcessor =
            std::make_shared<const PreparedCpuDisplayProcessorHandle>(std::move(*processor));

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available\n";
            return expectations.failures() == 0 ? 0 : 1;
        }

        PipelineSet pipelines = createPipelines(*device.device);
        expectations.expect(pipelines.valid(), "the typed pipelines are created");
        if (!pipelines.valid()) {
            return 1;
        }
        GpuResidentPreviewQualificationReport report = bloom::runtime::qualifyResidentPreview(
            *sharedProcessor, *device.device, pipelines.view());
        expectations.expect(report.eligible(), "the resident route qualifies");
        if (!report.eligible()) {
            return 1;
        }
        auto reportHandle =
            std::make_shared<const GpuResidentPreviewQualificationReport>(std::move(report));
        const auto size = chooseEligibleSize(*reportHandle);
        expectations.expect(size.has_value(), "an eligible size exists");
        if (!size.has_value()) {
            return 1;
        }
        constexpr std::size_t kBudget = std::size_t{1} << 28;

        const auto plan = oneSolidPlan(format(size->first, size->second));
        const PreviewRequestIdentity identity{.projectId = plan->projectId(),
                                              .compositionId = plan->compositionId(),
                                              .sourceRevision = plan->sourceRevision(),
                                              .requestGeneration = 1,
                                              .time = RationalTime::fromInteger(0),
                                              .output = PreviewOutput::Composition,
                                              .resolution = CompositionFormatResolution{},
                                              .quality = EvaluationQuality::Reference,
                                              .colorIntent =
                                                  EvaluationColorIntent::LinearRec709Scene,
                                              .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                              .roi = std::nullopt,
                                              .viewAdjust = ViewAdjust{},
                                              .displayName = {},
                                              .viewName = {},
                                              .showLook = true};
        const ProcessFrameIdentity processIdentity{.plan = plan,
                                                   .time = RationalTime::fromInteger(0),
                                                   .output = plan->output(),
                                                   .resolution = CompositionFormatResolution{},
                                                   .quality = EvaluationQuality::Reference,
                                                   .colorIntent =
                                                       EvaluationColorIntent::LinearRec709Scene,
                                                   .provider = EvaluationProvider::GpuResident};

        auto registry = GpuResidentFrameLeaseRegistry::create(*device.device);
        expectations.expect(registry != nullptr, "the lease registry is created");
        if (registry == nullptr) {
            return 1;
        }
        auto displayImage =
            produceResidentDisplay(*device.device, *pipelines.solid, *pipelines.display,
                                   size->first, size->second, kBudget);
        expectations.expect(displayImage != nullptr, "the resident display image is produced");
        if (displayImage == nullptr) {
            return 1;
        }

        const auto cacheWindow =
            bloom::render::ImageWindow::create(0, 0, size->first, size->second);
        const auto cacheDescriptor = bloom::render::Rgba32fImageDescriptor::create(
            *cacheWindow.value(), *cacheWindow.value(), bloom::core::PixelAspectRatio::square());
        GpuResidentDisplayProductRequest request;
        request.identity = identity;
        request.processIdentity = processIdentity;
        request.expectedDescriptor = *cacheDescriptor.value();
        request.bounds = {EvaluatedOperationBounds{
            .local = ContentBounds{0.0, 0.0, static_cast<double>(size->first),
                                   static_cast<double>(size->second)},
            .output = ContentBounds{0.0, 0.0, static_cast<double>(size->first),
                                    static_cast<double>(size->second)},
            .layerId = kLayer,
            .anchor = bloom::document::Vec2d{0.0, 0.0}}};
        request.display = displayImage;
        request.pixelStorageByteLimit = kBudget;
        auto product = bloom::runtime::makeGpuResidentDisplayPreview(
            *device.device, *registry, *sharedProcessor, reportHandle, request);
        expectations.expect(product.has_value(), "the resident product is published");
        if (!product.has_value() || product->residentFrame() == nullptr) {
            return 1;
        }
        const auto residentHandle = product->residentFrame();
        auto productHandle = std::make_shared<const PreparedPreviewFrame>(std::move(*product));
        const auto key = PreviewFrameCacheKey::forIdentity(identity);

        PreviewFrameCache cache(kBudget);
        cache.insert(productHandle);
        expectations.expect(cache.size() == 1, "the resident frame is retained once");
        expectations.expect(cache.contains(key), "contains() sees the live resident entry");
        expectations.expect(cache.residentBytes() ==
                                PreviewFrameCache::frameByteCost(*productHandle),
                            "the resident charge is the actual native allocation plus metadata");
        expectations.expect(PreviewFrameCache::frameByteCost(*productHandle) ==
                                residentHandle->retainedByteCost(),
                            "frameByteCost uses the resident retained cost");

        auto restamped = identity;
        restamped.requestGeneration = 2;
        auto taken = cache.take(restamped);
        expectations.expect(taken != nullptr && taken->residentFrame() != nullptr,
                            "a resident hit is re-stamped into a servable frame");
        if (taken != nullptr && taken->residentFrame() != nullptr) {
            expectations.expect(taken->residentFrame()->lease().id() ==
                                    residentHandle->lease().id(),
                                "a resident hit shares the same opaque lease");
            expectations.expect(taken->desiredIdentity().requestGeneration == 2,
                                "a resident hit answers the new request generation");
            expectations.expect(!taken->displayBufferView().has_value(),
                                "a resident hit still exposes no CPU pixels");
        }
        expectations.expect(cache.statistics().hits == 1, "the resident take counts a hit");
        expectations.expect(cache.timesFor(key).size() == 1,
                            "timesFor() lists the live resident entry");

        // Invalidation: a dead lease must never be a hit, never listed, never inserted.
        registry->invalidateAll();
        expectations.expect(!residentHandle->isDisplayValid(), "the lease is now invalid");
        expectations.expect(!cache.contains(key), "contains() hides an invalidated resident entry");
        expectations.expect(cache.take(restamped) == nullptr,
                            "take() is a miss for an invalidated resident entry");
        expectations.expect(cache.timesFor(key).empty(),
                            "timesFor() hides an invalidated resident entry");
        cache.clear();
        cache.insert(productHandle);
        expectations.expect(cache.size() == 0 && cache.statistics().rejections == 1,
                            "inserting an invalidated resident frame is refused");

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " cache resident expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: resident preview frame cache\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
