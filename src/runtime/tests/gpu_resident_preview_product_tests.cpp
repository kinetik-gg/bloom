// Real acceptance test for the fourth, closed PreparedPreviewFrame arm: the GPU-resident display
// product.
//
// On a real Vulkan device it builds the actual device + typed pipelines + canonical CPU OCIO
// processor, runs the REAL qualifyResidentPreview() (whose parity/timing pass is the only way to
// obtain an eligible report), produces a real resident process image with GpuSolid, feeds it to the
// real GpuResidentDisplay, publishes the resulting native display image into an owner-bound
// GpuResidentFrameLeaseRegistry, and then validates the product factory. It asserts the accepted
// frame carries genuine geometry/identity/provenance/cost and NO CPU pixels, and exercises the
// rejection gates: foreign registry/device/report, wrong dimensions/identity/budget, and an
// unqualified processor. Finally it invalidates the lease and proves the frame can no longer be
// restamped or served.
//
// Geometry is validated from the TRUSTED expected descriptor the request carries (the actual
// prepared-scene output / CPU process descriptor), not from the full composition format and not
// from the returned image: full-format resolution, explicit ProxyResolution, Half/Quarter policies,
// non-square pixel aspect, and a nonzero-origin ROI are all accepted inside the measured eligible
// interval, while a full-format descriptor under a proxy request and a one-pixel mismatch are
// rejected.
//
// Native readback exists only inside the qualification and is never part of this product path.

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
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_resident_preview_product.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

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
using bloom::render::GpuDeviceState;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuSolid;
using bloom::render::ImageWindow;
using bloom::render::Rgba32fImageDescriptor;
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
using bloom::runtime::EvaluationResolution;
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
using bloom::runtime::ProxyResolution;
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

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildNonDefaultProcessor() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return std::nullopt;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    for (const bloom::color::DisplayViewEntry& entry : resolved->displays()) {
        auto built =
            bloom::color::buildCpuDisplayProcessorForView(*resolved, entry.display, entry.view);
        if (built) {
            return std::move(built).takeHandle();
        }
    }
    return std::nullopt;
}

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStackNode = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutputNode = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);

[[nodiscard]] bloom::document::CompositionFormat
format(const std::uint32_t width, const std::uint32_t height,
       const PixelAspectRatio pixelAspect = PixelAspectRatio::square()) {
    const auto value = bloom::document::CompositionFormat::create(width, height, pixelAspect);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::optional<ImageWindow> window(const std::int64_t originX,
                                                const std::int64_t originY,
                                                const std::uint32_t width,
                                                const std::uint32_t height) {
    const auto result = ImageWindow::create(originX, originY, width, height);
    if (!result.hasValue()) {
        return std::nullopt;
    }
    return *result.value();
}

[[nodiscard]] std::optional<Rgba32fImageDescriptor> descriptor(const ImageWindow dataWindow,
                                                               const ImageWindow displayWindow,
                                                               const PixelAspectRatio pixelAspect) {
    const auto result = Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!result.hasValue()) {
        return std::nullopt;
    }
    return *result.value();
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

#include "gpu_resident_preview_product_cases.ipp"

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "usage: --loader <path> [--require-device]\n";
            return 2;
        }
        Expectations expectations;

        auto processor = buildCanonicalProcessor();
        expectations.expect(processor.has_value(), "the canonical CPU OCIO processor builds");
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
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message
                      << "; the resident product needs a real device\n";
            return expectations.failures() == 0 ? 0 : 1;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");

        PipelineSet pipelines = createPipelines(*device.device);
        expectations.expect(pipelines.valid(), "the typed pipelines are created");
        if (!pipelines.valid()) {
            return 1;
        }

        // The real qualification is the only source of an eligible report.
        GpuResidentPreviewQualificationReport report = bloom::runtime::qualifyResidentPreview(
            *sharedProcessor, *device.device, pipelines.view());
        expectations.expect(report.eligible(),
                            "the real resident qualification measured an eligible interval");
        if (!report.eligible()) {
            std::cerr << "FAIL: resident qualification not eligible: "
                      << report.diagnostic().message << '\n';
            return 1;
        }
        auto reportHandle =
            std::make_shared<const GpuResidentPreviewQualificationReport>(std::move(report));
        const auto size = chooseEligibleSize(*reportHandle);
        expectations.expect(size.has_value(), "a measured eligible size exists");
        if (!size.has_value()) {
            return 1;
        }
        constexpr std::size_t kBudget = std::size_t{1} << 28;
        const auto interval = reportHandle->eligibleInterval();
        expectations.expect(interval.has_value(), "the eligible report exposes an interval");
        if (!interval.has_value()) {
            return 1;
        }

        auto registry = GpuResidentFrameLeaseRegistry::create(*device.device);
        expectations.expect(registry != nullptr, "the owner-bound lease registry is created");
        if (registry == nullptr) {
            return 1;
        }

        // --- Accepted geometry cases inside the measured eligible interval -----------------------
        const std::uint32_t fullWidth = size->first;
        const std::uint32_t fullHeight = size->second;
        const auto fullDataWindow = window(0, 0, fullWidth, fullHeight);
        expectations.expect(fullDataWindow.has_value(), "the full window builds");
        if (!fullDataWindow.has_value()) {
            return 1;
        }
        runAcceptedCase(expectations, *device.device, pipelines, *registry, *sharedProcessor,
                        reportHandle,
                        CaseSpec{.compositionFormat = format(fullWidth, fullHeight),
                                 .resolution = CompositionFormatResolution{},
                                 .policy = PreviewResolutionPolicy::Full,
                                 .dataWindow = *fullDataWindow,
                                 .displayWindow = *fullDataWindow},
                        "full-format", kBudget);

        // Explicit ProxyResolution at the same eligible extent. The composition is twice the size,
        // so a reduced-resolution request is the only reason this geometry is expected; the trusted
        // descriptor carries the proxy extent, never the full format.
        runAcceptedCase(expectations, *device.device, pipelines, *registry, *sharedProcessor,
                        reportHandle,
                        CaseSpec{.compositionFormat = format(2 * fullWidth, 2 * fullHeight),
                                 .resolution = ProxyResolution{fullDataWindow->extent()},
                                 .policy = PreviewResolutionPolicy::Auto,
                                 .dataWindow = *fullDataWindow,
                                 .displayWindow = *fullDataWindow},
                        "proxy-resolution", kBudget);

        // Half and Quarter policies resolve to ProxyResolution at the same reduced extent.
        runAcceptedCase(expectations, *device.device, pipelines, *registry, *sharedProcessor,
                        reportHandle,
                        CaseSpec{.compositionFormat = format(2 * fullWidth, 2 * fullHeight),
                                 .resolution = ProxyResolution{fullDataWindow->extent()},
                                 .policy = PreviewResolutionPolicy::Half,
                                 .dataWindow = *fullDataWindow,
                                 .displayWindow = *fullDataWindow},
                        "half-policy", kBudget);
        {
            const auto pixels = static_cast<std::uint64_t>(fullWidth) * fullHeight;
            if (pixels >= interval->min_pixels && pixels <= interval->max_pixels) {
                runAcceptedCase(expectations, *device.device, pipelines, *registry,
                                *sharedProcessor, reportHandle,
                                CaseSpec{.compositionFormat = format(4 * fullWidth, 4 * fullHeight),
                                         .resolution = ProxyResolution{fullDataWindow->extent()},
                                         .policy = PreviewResolutionPolicy::Quarter,
                                         .dataWindow = *fullDataWindow,
                                         .displayWindow = *fullDataWindow},
                                "quarter-policy", kBudget);
            } else {
                std::cout << "NOTE: quarter-policy area is outside the measured interval; gate "
                             "not exercised\n";
            }
        }

        // Non-square format pixel aspect at full resolution.
        {
            const auto par = PixelAspectRatio::create(4, 3);
            expectations.expect(par.has_value(), "the 4:3 pixel aspect builds");
            if (par.has_value()) {
                runAcceptedCase(expectations, *device.device, pipelines, *registry,
                                *sharedProcessor, reportHandle,
                                CaseSpec{.compositionFormat = format(fullWidth, fullHeight, *par),
                                         .resolution = CompositionFormatResolution{},
                                         .policy = PreviewResolutionPolicy::Full,
                                         .dataWindow = *fullDataWindow,
                                         .displayWindow = *fullDataWindow,
                                         .pixelAspect = *par},
                                "non-square-format-par", kBudget);
            }
        }

        // Non-square PROXY-derived pixel aspect: a 2:1 composition sampled at the 1:1 proxy extent
        // derives a 2:1 pixel aspect, carried by the trusted descriptor.
        {
            const auto derivedPar = PixelAspectRatio::create(2, 1);
            expectations.expect(derivedPar.has_value(), "the derived 2:1 pixel aspect builds");
            if (derivedPar.has_value()) {
                runAcceptedCase(expectations, *device.device, pipelines, *registry,
                                *sharedProcessor, reportHandle,
                                CaseSpec{.compositionFormat = format(2 * fullWidth, fullHeight),
                                         .resolution = ProxyResolution{fullDataWindow->extent()},
                                         .policy = PreviewResolutionPolicy::Auto,
                                         .dataWindow = *fullDataWindow,
                                         .displayWindow = *fullDataWindow,
                                         .pixelAspect = *derivedPar},
                                "non-square-proxy-par", kBudget);
            }
        }

        // Nonzero-origin ROI: dataWindow is the ROI, displayWindow is the full resolution window.
        {
            const auto roi = window(8, 4, fullWidth, fullHeight);
            const auto roiDisplay = window(0, 0, 2 * fullWidth, 2 * fullHeight);
            expectations.expect(roi.has_value() && roiDisplay.has_value(),
                                "the nonzero-origin ROI geometry builds");
            if (roi.has_value() && roiDisplay.has_value()) {
                const auto pixels =
                    static_cast<std::uint64_t>(roi->extent().width()) * roi->extent().height();
                if (pixels >= interval->min_pixels && pixels <= interval->max_pixels) {
                    runAcceptedCase(
                        expectations, *device.device, pipelines, *registry, *sharedProcessor,
                        reportHandle,
                        CaseSpec{.compositionFormat = format(2 * fullWidth, 2 * fullHeight),
                                 .resolution = CompositionFormatResolution{},
                                 .policy = PreviewResolutionPolicy::Full,
                                 .dataWindow = *roi,
                                 .displayWindow = *roiDisplay,
                                 .roi = roi},
                        "nonzero-origin-roi", kBudget);
                } else {
                    std::cout << "NOTE: ROI area is outside the measured interval; gate not "
                                 "exercised\n";
                }
            }
        }

        // --- Rejection gates ---------------------------------------------------------------------
        const auto baseSpec = CaseSpec{.compositionFormat = format(fullWidth, fullHeight),
                                       .resolution = CompositionFormatResolution{},
                                       .policy = PreviewResolutionPolicy::Full,
                                       .dataWindow = *fullDataWindow,
                                       .displayWindow = *fullDataWindow};
        const auto basePlan = oneSolidPlan(baseSpec.compositionFormat);
        const auto baseDescriptor =
            descriptor(baseSpec.dataWindow, baseSpec.displayWindow, baseSpec.pixelAspect);
        const auto baseIdentity = requestIdentityFor(baseSpec, *basePlan);
        const auto baseProcess = gpuProcessIdentityFor(baseSpec, basePlan);
        const auto baseBounds = boundsFor(baseSpec.dataWindow);
        auto baseImage = produceResidentDisplay(
            *device.device, *pipelines.solid, *pipelines.display, baseSpec.dataWindow,
            baseSpec.displayWindow, baseSpec.pixelAspect, kBudget);
        expectations.expect(baseDescriptor.has_value() && baseImage != nullptr,
                            "the base rejection fixture builds");
        if (!baseDescriptor.has_value() || baseImage == nullptr) {
            return 1;
        }

        const auto refuse = [&](const GpuResidentDisplayProductRequest& candidate,
                                const std::string& label) {
            std::string why;
            expectations.expect(
                !bloom::runtime::gpuResidentDisplayProductIsEligible(
                    *device.device, *registry, *sharedProcessor, *reportHandle, candidate, why),
                label + " is ineligible: " + why);
            expectations.expect(
                !bloom::runtime::makeGpuResidentDisplayPreview(
                     *device.device, *registry, *sharedProcessor, reportHandle, candidate)
                     .has_value(),
                label + " is refused by the factory");
        };

        GpuResidentDisplayProductRequest baseRequest;
        baseRequest.identity = baseIdentity;
        baseRequest.processIdentity = baseProcess;
        baseRequest.bounds = baseBounds;
        baseRequest.expectedDescriptor = *baseDescriptor;
        baseRequest.display = baseImage;
        baseRequest.pixelStorageByteLimit = kBudget;

        // Full-format descriptor under a reduced-resolution request must be rejected: the trusted
        // descriptor must name the proxy extent, never the full composition format.
        {
            const auto proxyWindow = window(0, 0, fullWidth, fullHeight);
            const auto widePlan = oneSolidPlan(format(2 * fullWidth, 2 * fullHeight));
            const auto wideWindow = window(0, 0, 2 * fullWidth, 2 * fullHeight);
            expectations.expect(proxyWindow.has_value() && wideWindow.has_value(),
                                "the proxy and wide geometry windows build");
            if (!proxyWindow.has_value() || !wideWindow.has_value()) {
                return 1;
            }
            const auto fullDescriptor =
                descriptor(*wideWindow, *wideWindow, PixelAspectRatio::square());
            expectations.expect(fullDescriptor.has_value(), "the full-format descriptor builds");
            if (!fullDescriptor.has_value()) {
                return 1;
            }
            const auto wideSpec =
                CaseSpec{.compositionFormat = format(2 * fullWidth, 2 * fullHeight),
                         .resolution = ProxyResolution{proxyWindow->extent()},
                         .policy = PreviewResolutionPolicy::Auto,
                         .dataWindow = *proxyWindow,
                         .displayWindow = *proxyWindow};
            auto wrong = baseRequest;
            wrong.identity = requestIdentityFor(wideSpec, *widePlan);
            wrong.processIdentity = gpuProcessIdentityFor(wideSpec, widePlan);
            wrong.expectedDescriptor = *fullDescriptor;
            refuse(wrong, "a full-format descriptor under a proxy request");
        }
        // A one-pixel mismatch between the trusted descriptor and the actual image must fail even
        // though the trusted descriptor is consistent with the request.
        {
            const auto mismatchWindow = window(0, 0, fullWidth - 1, fullHeight);
            expectations.expect(mismatchWindow.has_value(), "the one-pixel mismatch window builds");
            if (!mismatchWindow.has_value()) {
                return 1;
            }
            auto mismatchImage = produceResidentDisplay(
                *device.device, *pipelines.solid, *pipelines.display, *mismatchWindow,
                *mismatchWindow, PixelAspectRatio::square(), kBudget);
            auto wrong = baseRequest;
            wrong.display = mismatchImage;
            refuse(wrong, "a one-pixel mismatch between descriptor and image");
        }
        // Wrong plan dimensions: the trusted descriptor no longer matches the resolved window.
        {
            const auto otherPlan = oneSolidPlan(format(fullWidth + 16, fullHeight + 16));
            auto wrong = baseRequest;
            wrong.identity = requestIdentityFor(baseSpec, *otherPlan);
            wrong.processIdentity = gpuProcessIdentityFor(baseSpec, otherPlan);
            refuse(wrong, "a request whose plan dimensions differ from the trusted descriptor");
        }
        // Wrong identity: the request time no longer matches the process frame.
        {
            auto wrong = baseRequest;
            wrong.identity.time = RationalTime::fromInteger(5);
            refuse(wrong, "a request whose identity time differs from the process frame");
        }
        // Over budget: the native allocation cannot fit the request byte budget.
        {
            auto wrong = baseRequest;
            wrong.pixelStorageByteLimit = 16;
            refuse(wrong, "a request whose byte budget is below the native allocation");
        }
        // A null trusted descriptor must be rejected.
        {
            auto wrong = baseRequest;
            wrong.expectedDescriptor = std::nullopt;
            refuse(wrong, "a request with no trusted expected descriptor");
        }
        // Unqualified processor: the report does not qualify a non-default processor.
        {
            const auto nonDefault = buildNonDefaultProcessor();
            if (nonDefault.has_value()) {
                expectations.expect(!reportHandle->eligibleFor(*device.device, *nonDefault),
                                    "the report rejects a non-default processor");
                expectations.expect(
                    !bloom::runtime::makeGpuResidentDisplayPreview(
                         *device.device, *registry, *nonDefault, reportHandle, baseRequest)
                         .has_value(),
                    "the factory refuses a non-default processor");
            } else {
                std::cout << "NOTE: no non-default processor buildable; gate not exercised\n";
            }
        }

        // Foreign registry/device: a second device (and its registry) must not accept the report.
        auto second = GpuDevice::create(createOptions);
        if (second) {
            auto secondRegistry = GpuResidentFrameLeaseRegistry::create(*second.device);
            expectations.expect(secondRegistry != nullptr,
                                "the second device's registry is created");
            expectations.expect(!reportHandle->eligibleFor(*second.device, *sharedProcessor),
                                "the report rejects a second same-physical-GPU device");
            if (secondRegistry != nullptr) {
                std::string why;
                expectations.expect(!bloom::runtime::gpuResidentDisplayProductIsEligible(
                                        *second.device, *secondRegistry, *sharedProcessor,
                                        *reportHandle, baseRequest, why),
                                    "a foreign device/registry is ineligible: " + why);
                expectations.expect(!bloom::runtime::makeGpuResidentDisplayPreview(
                                         *second.device, *secondRegistry, *sharedProcessor,
                                         reportHandle, baseRequest)
                                         .has_value(),
                                    "the factory refuses a foreign device/registry");
            }
        } else {
            std::cout
                << "NOTE: a second device is unavailable; foreign-device gate not exercised\n";
        }

        // --- Lease invalidation ----------------------------------------------------------------
        {
            auto product = bloom::runtime::makeGpuResidentDisplayPreview(
                *device.device, *registry, *sharedProcessor, reportHandle, baseRequest);
            expectations.expect(product.has_value(), "the invalidation fixture publishes");
            if (product.has_value() && product->residentFrame() != nullptr) {
                const auto residentHandle = product->residentFrame();
                registry->invalidateAll();
                expectations.expect(!residentHandle->isDisplayValid(),
                                    "an invalidated lease makes the resident frame invalid");
                expectations.expect(!product->isDisplayValid(),
                                    "the envelope reports an invalidated resident display");
                expectations.expect(
                    !PreparedPreviewFrame::createResident(2, residentHandle).has_value(),
                    "an invalidated lease can never be restamped into a servable frame");
            }
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " resident product expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU resident preview product\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
