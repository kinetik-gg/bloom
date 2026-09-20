// Native GPU-resident cache-limit regressions. On a real Vulkan device this builds the genuine
// product fixture copied from the finalized cache-edge-audit resident test (Solid ->
// GpuResidentDisplay -> owner-registry lease -> real product factory) and then exercises the
// additive GPU-resident sublimits added by this app-wiring slice:
//
//   * more than 128 distinct-frame keys with live leases and small native images, under a registry
//     cap of cacheEntries + headroom: the cache evicts first and the registry keeps publishing;
//   * the byte limit and the entry-count limit are each enforced independently;
//   * eviction is LRU among resident entries and CPU entries are never touched;
//   * a surviving viewer/native pin stays valid even after its cache entry is evicted;
//   * clear()/trimToBytes()/dead-entry removal release the resident accounting;
//   * the GPU sublimit eviction also advances statistics().evictions, which is the exact signal the
//     RAM preview controller watches to stop a run that has outgrown memory.
//
// No identity is laundered: every frame is produced by the real factory from a genuinely matching
// plan/process identity and native display image, and distinct cache keys come from distinct
// genuine frame times carried identically in the request and process identity.

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

#include <algorithm>
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
#include <vector>

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
using bloom::runtime::GpuResidentFrameLeaseBudgets;
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
oneSolidPlan(const bloom::document::CompositionFormat compositionFormat,
             const bloom::document::ProjectId projectId = kProjectId) {
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
        bloom::document::Revision::fromRaw(7), projectId, kCompositionId, compositionFormat,
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

// One genuinely matching resident frame for `frameTime`, produced by the real factory. The cache
// key is derived from the frame's own desired identity, never restamped or fabricated.
[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
makeResidentFrame(GpuDevice& device, GpuResidentFrameLeaseRegistry& registry,
                  const PreparedCpuDisplayProcessorHandle& processor,
                  const std::shared_ptr<const GpuResidentPreviewQualificationReport>& report,
                  const std::shared_ptr<const CompiledCompositionPlan>& plan,
                  PipelineSet& pipelines, const std::uint32_t width, const std::uint32_t height,
                  const bloom::render::Rgba32fImageDescriptor& descriptor,
                  const std::int64_t frameTime, const std::uint64_t generation,
                  const std::size_t budget, std::string& failure) {
    const auto time = RationalTime::fromInteger(frameTime);
    const PreviewRequestIdentity identity{.projectId = plan->projectId(),
                                          .compositionId = plan->compositionId(),
                                          .sourceRevision = plan->sourceRevision(),
                                          .requestGeneration = generation,
                                          .time = time,
                                          .output = PreviewOutput::Composition,
                                          .resolution = CompositionFormatResolution{},
                                          .quality = EvaluationQuality::Reference,
                                          .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                          .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                          .roi = std::nullopt,
                                          .viewAdjust = ViewAdjust{},
                                          .displayName = {},
                                          .viewName = {},
                                          .showLook = true};
    const ProcessFrameIdentity processIdentity{.plan = plan,
                                               .time = time,
                                               .output = plan->output(),
                                               .resolution = CompositionFormatResolution{},
                                               .quality = EvaluationQuality::Reference,
                                               .colorIntent =
                                                   EvaluationColorIntent::LinearRec709Scene,
                                               .provider = EvaluationProvider::GpuResident};
    auto display =
        produceResidentDisplay(device, *pipelines.solid, *pipelines.display, width, height, budget);
    if (display == nullptr) {
        failure = "the native resident display image was not produced";
        return nullptr;
    }
    GpuResidentDisplayProductRequest request;
    request.identity = identity;
    request.processIdentity = processIdentity;
    request.expectedDescriptor = descriptor;
    request.bounds = {EvaluatedOperationBounds{
        .local = ContentBounds{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)},
        .output = ContentBounds{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)},
        .layerId = kLayer,
        .anchor = bloom::document::Vec2d{0.0, 0.0}}};
    request.display = display;
    request.pixelStorageByteLimit = budget;
    if (!bloom::runtime::gpuResidentDisplayProductIsEligible(device, registry, processor, *report,
                                                             request, failure)) {
        return nullptr;
    }
    auto product =
        bloom::runtime::makeGpuResidentDisplayPreview(device, registry, processor, report, request);
    if (!product.has_value() || product->residentFrame() == nullptr) {
        failure = "the resident product was refused (registry budget or factory)";
        return nullptr;
    }
    return std::make_shared<const PreparedPreviewFrame>(std::move(*product));
}

[[nodiscard]] PreviewFrameCacheKey keyFor(const PreparedPreviewFrame& frame) {
    return PreviewFrameCacheKey::forIdentity(frame.desiredIdentity());
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
        const auto cacheWindow =
            bloom::render::ImageWindow::create(0, 0, size->first, size->second);
        const auto descriptorResult = bloom::render::Rgba32fImageDescriptor::create(
            *cacheWindow.value(), *cacheWindow.value(), bloom::core::PixelAspectRatio::square());
        expectations.expect(descriptorResult.hasValue(), "the trusted descriptor builds");
        if (!descriptorResult.hasValue()) {
            return 1;
        }
        const auto& descriptor = *descriptorResult.value();

        // ----------------------------------------------------------------------------------------
        // The explicit >128 gate: a cache resident cap of 128 with a registry cap of 128 + 16
        // headroom. 140 distinct genuine frames are inserted; the cache must evict first and the
        // registry must keep publishing every one.
        // ----------------------------------------------------------------------------------------
        constexpr std::size_t kCacheEntries = 128;
        constexpr std::size_t kRegistryEntries = kCacheEntries + 16;
        constexpr std::size_t kFrameCount = 140;
        constexpr std::uint64_t kCacheBytes = 256ULL * 1024ULL * 1024ULL;

        GpuResidentFrameLeaseBudgets registryBudgets;
        registryBudgets.maxBytes = 256ULL * 1024ULL * 1024ULL;
        registryBudgets.maxEntries = kRegistryEntries;
        auto registry = GpuResidentFrameLeaseRegistry::create(*device.device, registryBudgets);
        expectations.expect(registry != nullptr, "the bounded lease registry is created");
        if (registry == nullptr) {
            return 1;
        }

        PreviewFrameCache cache(kBudget);
        cache.setGpuResidentLimits(kCacheBytes, kCacheEntries);
        expectations.expect(cache.gpuResidentByteLimit() == kCacheBytes &&
                                cache.gpuResidentEntryLimit() == kCacheEntries,
                            "the cache installs the bounded resident sublimits");

        std::vector<PreviewFrameCacheKey> keys;
        keys.reserve(kFrameCount);
        std::shared_ptr<const PreparedPreviewFrame> oldestFrame;
        std::shared_ptr<const PreparedPreviewFrame> newestFrame;
        std::size_t refused = 0;
        std::size_t overCap = 0;
        for (std::size_t index = 0; index < kFrameCount; ++index) {
            std::string failure;
            auto frame =
                makeResidentFrame(*device.device, *registry, *sharedProcessor, reportHandle, plan,
                                  pipelines, size->first, size->second, descriptor,
                                  static_cast<std::int64_t>(index), index + 1, kBudget, failure);
            if (frame == nullptr) {
                ++refused;
                if (refused == 1) {
                    std::cerr << "first resident refusal at frame " << index << ": " << failure
                              << '\n';
                }
                continue;
            }
            keys.push_back(keyFor(*frame));
            cache.insert(frame);
            if (cache.gpuResidentEntryCount() > kCacheEntries) {
                ++overCap;
            }
            if (index == 0) {
                oldestFrame = frame;
            }
            if (index + 1 == kFrameCount) {
                newestFrame = frame;
            }
        }
        expectations.expect(refused == 0,
                            "the registry keeps publishing every resident frame under its cap");
        expectations.expect(keys.size() == kFrameCount, "all resident frames were produced");
        expectations.expect(overCap == 0,
                            "the cache never exceeds its resident entry cap during the run");
        expectations.expect(cache.gpuResidentEntryCount() == kCacheEntries,
                            "the cache holds exactly its resident entry cap");
        expectations.expect(cache.statistics().gpuResidentEvictions >= kFrameCount - kCacheEntries,
                            "the cache evicted the LRU resident entries as it filled");
        expectations.expect(cache.statistics().evictions == cache.statistics().gpuResidentEvictions,
                            "the overall budget was not the binding limit");
        expectations.expect(cache.statistics().evictions > 0,
                            "the GPU sublimit hit advances the RAM-preview budget signal");
        expectations.expect(registry->entryCount() <= kRegistryEntries,
                            "the registry stays within its metadata cap");
        expectations.expect(registry->chargedBytes() <= registryBudgets.maxBytes,
                            "the registry stays within its byte cap");

        // LRU order: the oldest keys are gone, the newest are retained.
        expectations.expect(!keys.empty() && !cache.contains(keys.front()),
                            "the oldest resident key was evicted first");
        expectations.expect(!keys.empty() && cache.contains(keys.back()),
                            "the newest resident key is retained");
        expectations.expect(newestFrame != nullptr && newestFrame->residentFrame() != nullptr &&
                                newestFrame->residentFrame()->isDisplayValid(),
                            "the current viewer's resident lease is still valid");
        expectations.expect(oldestFrame != nullptr && oldestFrame->residentFrame() != nullptr &&
                                oldestFrame->residentFrame()->isDisplayValid(),
                            "an evicted-but-pinned native lease is still valid");

        // No permanent CPU cliff: a fresh publish still succeeds after the cap was reached.
        {
            std::string failure;
            auto fresh = makeResidentFrame(*device.device, *registry, *sharedProcessor,
                                           reportHandle, plan, pipelines, size->first, size->second,
                                           descriptor, static_cast<std::int64_t>(kFrameCount + 1),
                                           kFrameCount + 2, kBudget, failure);
            expectations.expect(fresh != nullptr,
                                "a fresh resident publish still succeeds after the cap was hit");
        }

        // ----------------------------------------------------------------------------------------
        // The byte limit is enforced independently of the entry limit.
        // ----------------------------------------------------------------------------------------
        {
            auto byteRegistry =
                GpuResidentFrameLeaseRegistry::create(*device.device, registryBudgets);
            expectations.expect(byteRegistry != nullptr, "the byte-limit registry is created");
            if (byteRegistry == nullptr) {
                return 1;
            }
            PreviewFrameCache byteCache(kBudget);
            std::string failure;
            auto probe = makeResidentFrame(*device.device, *byteRegistry, *sharedProcessor,
                                           reportHandle, plan, pipelines, size->first, size->second,
                                           descriptor, 0, 1, kBudget, failure);
            expectations.expect(probe != nullptr, "the byte-limit probe frame is produced");
            if (probe == nullptr) {
                return 1;
            }
            const auto frameBytes = PreviewFrameCache::frameByteCost(*probe);
            expectations.expect(frameBytes > 0, "the resident frame has a positive byte cost");
            const auto byteLimit = frameBytes * 2;
            byteCache.setGpuResidentLimits(byteLimit, 4096);
            byteCache.insert(probe);
            for (std::size_t index = 1; index <= 6; ++index) {
                auto frame = makeResidentFrame(
                    *device.device, *byteRegistry, *sharedProcessor, reportHandle, plan, pipelines,
                    size->first, size->second, descriptor, static_cast<std::int64_t>(index),
                    index + 1, kBudget, failure);
                if (frame != nullptr) {
                    byteCache.insert(frame);
                }
            }
            expectations.expect(byteCache.gpuResidentBytes() <= byteLimit,
                                "the resident byte limit is enforced");
            expectations.expect(byteCache.gpuResidentEntryCount() <= 2,
                                "the byte limit bounds the resident entry count too");
        }

        // ----------------------------------------------------------------------------------------
        // The small configurable edge case: cache 3 vs registry 5.
        // ----------------------------------------------------------------------------------------
        {
            GpuResidentFrameLeaseBudgets smallBudgets;
            smallBudgets.maxBytes = 256ULL * 1024ULL * 1024ULL;
            smallBudgets.maxEntries = 5;
            auto smallRegistry =
                GpuResidentFrameLeaseRegistry::create(*device.device, smallBudgets);
            expectations.expect(smallRegistry != nullptr, "the small registry is created");
            if (smallRegistry == nullptr) {
                return 1;
            }
            PreviewFrameCache smallCache(kBudget);
            smallCache.setGpuResidentLimits(kCacheBytes, 3);
            std::size_t smallRefused = 0;
            for (std::size_t index = 0; index < 10; ++index) {
                std::string failure;
                auto frame = makeResidentFrame(
                    *device.device, *smallRegistry, *sharedProcessor, reportHandle, plan, pipelines,
                    size->first, size->second, descriptor, static_cast<std::int64_t>(index),
                    index + 1, kBudget, failure);
                if (frame == nullptr) {
                    ++smallRefused;
                    continue;
                }
                smallCache.insert(frame);
            }
            expectations.expect(smallRefused == 0,
                                "the small registry keeps publishing under its 5-entry cap");
            expectations.expect(smallCache.gpuResidentEntryCount() <= 3,
                                "the small cache never exceeds its 3-entry resident cap");
            expectations.expect(smallCache.statistics().evictions > 0,
                                "the small cache advanced the budget signal");
        }

        // ----------------------------------------------------------------------------------------
        // clear() and trimToBytes() release the resident accounting; a dead entry's removal
        // releases it too. These run last because invalidateAll() is registry-wide.
        // ----------------------------------------------------------------------------------------
        cache.clear();
        expectations.expect(cache.gpuResidentBytes() == 0 && cache.gpuResidentEntryCount() == 0,
                            "clear() releases the resident accounting");

        {
            auto trimRegistry =
                GpuResidentFrameLeaseRegistry::create(*device.device, registryBudgets);
            expectations.expect(trimRegistry != nullptr, "the trim registry is created");
            if (trimRegistry == nullptr) {
                return 1;
            }
            PreviewFrameCache trimCache(kBudget);
            trimCache.setGpuResidentLimits(kCacheBytes, 4096);
            for (std::size_t index = 0; index < 4; ++index) {
                std::string failure;
                auto frame = makeResidentFrame(
                    *device.device, *trimRegistry, *sharedProcessor, reportHandle, plan, pipelines,
                    size->first, size->second, descriptor, static_cast<std::int64_t>(index),
                    index + 1, kBudget, failure);
                if (frame != nullptr) {
                    trimCache.insert(frame);
                }
            }
            expectations.expect(trimCache.gpuResidentEntryCount() == 4,
                                "the trim cache starts with four resident entries");
            trimCache.trimToBytes(1);
            expectations.expect(trimCache.gpuResidentBytes() == 0 &&
                                    trimCache.gpuResidentEntryCount() == 0,
                                "trimToBytes() releases the resident accounting");
            expectations.expect(trimCache.residentBytes() == 0,
                                "trimToBytes() leaves no negative or residual charge");
        }

        {
            auto deadRegistry =
                GpuResidentFrameLeaseRegistry::create(*device.device, registryBudgets);
            expectations.expect(deadRegistry != nullptr, "the dead-entry registry is created");
            if (deadRegistry == nullptr) {
                return 1;
            }
            PreviewFrameCache deadCache(kBudget);
            deadCache.setGpuResidentLimits(kCacheBytes, 4096);
            std::string failure;
            auto frame = makeResidentFrame(*device.device, *deadRegistry, *sharedProcessor,
                                           reportHandle, plan, pipelines, size->first, size->second,
                                           descriptor, 0, 1, kBudget, failure);
            expectations.expect(frame != nullptr, "the dead-entry probe frame is produced");
            if (frame == nullptr) {
                return 1;
            }
            const auto key = keyFor(*frame);
            deadCache.insert(frame);
            expectations.expect(deadCache.gpuResidentEntryCount() == 1,
                                "the dead-entry cache starts with one resident entry");
            deadRegistry->invalidateAll();
            expectations.expect(!frame->residentFrame()->isDisplayValid(),
                                "the probe lease is invalid after invalidateAll()");
            auto identity = frame->desiredIdentity();
            identity.requestGeneration += 1;
            expectations.expect(deadCache.take(identity) == nullptr,
                                "a dead resident entry is a miss");
            expectations.expect(deadCache.gpuResidentBytes() == 0 &&
                                    deadCache.gpuResidentEntryCount() == 0,
                                "removing a dead resident entry releases its accounting");
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " resident cache limit expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: resident preview frame cache limits\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
