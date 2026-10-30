#include "gpu_coverage_contract_plans.hpp"
#include "gpu_process_frame_preparation_private.hpp"
#include "gpu_scene_coverage.hpp"
#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace bloom::runtime {
struct GpuSceneBuilderTestAccess final {
    static void setCheckpoint(CpuGpuSceneBuilder& builder, std::function<void()> callback) {
        builder.checkpoint_ = std::move(callback);
    }
};
} // namespace bloom::runtime
namespace {
using namespace bloom::runtime;
using namespace bloom::render;
namespace document = bloom::document;
struct ExrPixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

void writeExrRgbaWithChromaticities(const std::filesystem::path& path, const int width,
                                    const int height, const std::vector<ExrPixel>& pixels,
                                    const Imf::Chromaticities& chromaticities) {
    if (static_cast<int>(pixels.size()) != width * height) {
        throw std::logic_error("EXR fixture pixel count does not match its dimensions");
    }
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(chromaticities));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));

    std::vector<float> red(pixels.size()), green(pixels.size()), blue(pixels.size()),
        alpha(pixels.size());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        red[index] = pixels[index].red;
        green[index] = pixels[index].green;
        blue[index] = pixels[index].blue;
        alpha[index] = pixels[index].alpha;
    }
    Imf::FrameBuffer frameBuffer;
    const auto stride = static_cast<std::size_t>(width) * sizeof(float);
    frameBuffer.insert(
        "R", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(red.data()), sizeof(float), stride));
    frameBuffer.insert(
        "G", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(green.data()), sizeof(float), stride));
    frameBuffer.insert(
        "B", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(blue.data()), sizeof(float), stride));
    frameBuffer.insert(
        "A", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(alpha.data()), sizeof(float), stride));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

void writeExrRgba(const std::filesystem::path& path, const int width, const int height,
                  const std::vector<ExrPixel>& pixels) {
    writeExrRgbaWithChromaticities(path, width, height, pixels, Imf::Chromaticities());
}

[[nodiscard]] document::AssetRecord imageAsset(const std::filesystem::path& path,
                                               const std::string& name, const std::uint64_t rawId) {
    const auto probe = bloom::media::probeImage(path);
    if (!probe.value.has_value()) {
        throw std::logic_error("EXR fixture could not be probed: " + probe.diagnostic);
    }
    document::AssetRecord asset;
    asset.id = bloom::document::AssetId::fromRaw(rawId);
    asset.kind = document::AssetKind::Image;
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:" + path.string();
    asset.contentDigest = probe.value->contentDigest;
    asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
    asset.width = probe.value->width;
    asset.height = probe.value->height;
    asset.name = name;
    return asset;
}

[[nodiscard]] std::vector<ExrPixel> signedHdrPixels() {
    return {ExrPixel{1.5F, -0.25F, 0.5F, 1.0F},  ExrPixel{0.25F, 0.5F, 4.0F, 1.0F},
            ExrPixel{2.0F, 0.125F, 0.75F, 0.5F}, ExrPixel{0.5F, 2.5F, -0.5F, 1.0F},
            ExrPixel{0.0F, 0.0F, 0.0F, 0.0F},    ExrPixel{3.0F, 1.0F, 0.25F, 0.25F}};
}
constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024;
void require(const bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void finalPreparation() {
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(1);
    auto plan = bloom::gpu_coverage_fixtures::twoLayerPlan(
        bloom::gpu_coverage_fixtures::format(1920, 1080), {}, {}, 600.0, 400.0, 899000);
    auto request = bloom::gpu_coverage_fixtures::requestFor(*plan);
    request.pixelStorageByteLimit = gib;
    GpuProcessFrameEvaluatorOptions options;
    options.processingPhasePolicy = policy;
    options.requestByteBudget = gib;
    options.readbackByteBudget = gib;
    auto cold = bloom::runtime::detail::prepareExportScene(options, plan, request, {});
    require(!cold.scene && cold.diagnosticCode == GpuProcessFrameDiagnosticCode::AwaitingCapacity,
            "final preparation awaits paired capacity before preparing a scene");
    auto omitted = options;
    omitted.requestByteBudget.reset();
    omitted.readbackByteBudget.reset();
    auto coldDefault = bloom::runtime::detail::prepareExportScene(omitted, plan, request, {});
    require(!coldDefault.scene &&
                coldDefault.diagnosticCode == GpuProcessFrameDiagnosticCode::AwaitingCapacity,
            "omitted final limits never manufacture capacity before root delivery");
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/final",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 6 * gib});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    require(policy->reportDevice(111, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = gib,
                                       .available_bytes = 0}),
            "measured zero final device report accepted");
    auto measuredZero = bloom::runtime::detail::prepareExportScene(omitted, plan, request, {});
    require(!measuredZero.scene &&
                measuredZero.diagnosticCode == GpuProcessFrameDiagnosticCode::BudgetRefused,
            "omitted final limits refuse measured zero without a fabricated floor");
    require(policy->reportDevice(111, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = gib,
                                       .available_bytes = gib}),
            "final owner report accepted");
    {
        auto prepared = bloom::runtime::detail::prepareExportScene(options, plan, request, {});
        require(prepared.scene && prepared.phaseLease &&
                    prepared.phaseLease->sourceByteLimit() == 0 &&
                    prepared.readbackByteAllowance == 1920ULL * 1080 * 32 + 1024ULL * 1024,
                "source-free final admits selected output overlap rather than the whole ceiling");
        auto competing = bloom::runtime::detail::prepareExportScene(options, plan, request, {});
        require(!competing.scene &&
                    competing.diagnosticCode == GpuProcessFrameDiagnosticCode::AwaitingCapacity,
                "final shares the same bounded phase count and typed wait");
    }
    {
        auto encoded = bloom::runtime::detail::prepareExportScene(options, plan, request, {}, true);
        require(encoded.scene && encoded.phaseLease &&
                    encoded.readbackByteAllowance ==
                        512ULL * 1024 * 1024 + 1920ULL * 1080 * 64 + 1024ULL * 1024,
                "encoded final reserves program, transfer and independent host adoption overlap");
    }
    options.requestByteBudget.reset();
    options.readbackByteBudget.reset();
    {
        auto automatic = bloom::runtime::detail::prepareExportScene(options, plan, request, {});
        require(automatic.scene && automatic.phaseLease &&
                    automatic.phaseLease->executorByteLimit() > gib / 4 &&
                    automatic.readbackByteAllowance == 1920ULL * 1080 * 32 + 1024ULL * 1024,
                "omitted final budgets use sampled root capacity beyond the old quarter gate");
    }
    options.readbackByteBudget = 1;
    auto minimum = bloom::runtime::detail::prepareExportScene(options, plan, request, {});
    require(!minimum.scene &&
                minimum.diagnosticCode == GpuProcessFrameDiagnosticCode::BudgetRefused &&
                policy->snapshot().reservations.admittedPhases == 0,
            "impossible final output minimum refuses before source preparation");
}

void prepare() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(1);
    auto plan = bloom::gpu_coverage_fixtures::twoLayerPlan(
        bloom::gpu_coverage_fixtures::format(1920, 1080), {}, {}, 600.0, 400.0, 890000);
    auto request = bloom::gpu_coverage_fixtures::requestFor(*plan);
    request.pixelStorageByteLimit = gib;
    GpuScenePreparationPhase phase{.policy = policy,
                                   .envelope = {.metadataBytes = 1024ULL * 1024,
                                                .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                                                .requestedExecutorBytes = gib,
                                                .deviceWorkingBytes = 320ULL * 1024 * 1024}};
    CpuGpuSceneBuilder builder(nullptr, {}, {}, phase);
    std::size_t callbacks = 0;
    GpuSceneBuilderTestAccess::setCheckpoint(builder, [&] { ++callbacks; });
    auto cold = builder.build(plan, request);
    require(!cold.scene &&
                cold.diagnostic.code == PreparedGpuSceneDiagnosticCode::AwaitingCapacity &&
                callbacks == 0,
            "cold phase waits before any heavy preparer checkpoint");
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = now;
    sample.process.domains.push_back({.path = "/own",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 6 * gib});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib, now);
    require(policy->reportDevice(101, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = gib,
                                       .available_bytes = 0}),
            "native measured zero reported");
    auto zero = builder.build(plan, request);
    require(!zero.scene && zero.diagnostic.code == PreparedGpuSceneDiagnosticCode::BudgetRefused &&
                callbacks == 0,
            "measured zero refuses before source preparation without CPU substitution");
    require(policy->reportDevice(101, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = gib,
                                       .available_bytes = gib}),
            "native quota becomes available");
    {
        auto solid = builder.build(plan, request);
        require(solid.scene && solid.phaseLease && solid.phaseLease->sourceByteLimit() == 0 &&
                    callbacks == 1,
                "real1080p solid preparation admits under original1GiB request cap");
        auto competing = builder.build(plan, request);
        require(!competing.scene &&
                    competing.diagnostic.code == PreparedGpuSceneDiagnosticCode::AwaitingCapacity &&
                    callbacks == 1,
                "competing production request waits before heavy callback");
    }
    require(policy->snapshot().reservations.admittedPhases == 0,
            "preparation result teardown releases last phase owner");
    auto invalidPhase = phase;
    invalidPhase.envelope.minimumExecutorBytes = gib + 1;
    CpuGpuSceneBuilder invalidBuilder(nullptr, {}, {}, invalidPhase);
    auto invalid = invalidBuilder.build(plan, request);
    require(!invalid.scene &&
                invalid.diagnostic.code == PreparedGpuSceneDiagnosticCode::InvalidRequest &&
                !invalid.diagnostic.admissionRetry &&
                policy->snapshot().reservations.admittedPhases == 0,
            "invalid envelope is terminal with accurate classification and no retry metadata");
    auto definition = plan->copyDefinition();
    definition.operations[0] = bloom::gpu_coverage_plans::text(891000);
    auto textPlan = bloom::gpu_coverage_fixtures::publish(std::move(definition));
    {
        auto text = builder.build(textPlan, request);
        require(text.scene && text.phaseLease && text.phaseLease->sourceByteLimit() == 0 &&
                    callbacks == 2,
                "real1080p text coverage preparation admits with source-free envelope");
    }
    require(policy->snapshot().reservations.admittedPhases == 0,
            "source-free text quota is released with its prepared result");
}
// Owner-reported regression: with the delivered default, every native scene preparation asks for
// the whole device as its executor ceiling. The first live phase (a RAM-preview fill) used to be
// granted all remaining device room, so the Viewer's concurrent request was refused "does not fit
// the current device-memory capacity" with ~22 GiB of the device free.
void concurrentDefaultCeiling() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    auto view = std::make_shared<TaskBudgetView>();
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(32, nullptr, view);
    auto plan = bloom::gpu_coverage_fixtures::twoLayerPlan(
        bloom::gpu_coverage_fixtures::format(2048, 858), {}, {}, 600.0, 400.0, 892000);
    auto request = bloom::gpu_coverage_fixtures::requestFor(*plan);
    request.pixelStorageByteLimit = gib;
    GpuScenePreparationPhase phase{.policy = policy,
                                   .envelope = {.metadataBytes = 1024ULL * 1024,
                                                .retainedCoverageByteLimit = 16ULL * 1024 * 1024}};
    CpuGpuSceneBuilder builder(nullptr, {}, {}, phase);
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.configuredBytes = 30 * gib;
    sample.processingEffectiveBytes = 10 * gib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = now;
    sample.process.domains.push_back({.path = "/own",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 64 * gib,
                                      .currentBytes = 2 * gib});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib,
                            MemoryBudgetAllocation{.usableByteBudget = 60 * gib,
                                                   .operationCacheByteBudget = 18 * gib,
                                                   .previewFrameCacheByteBudget = 12 * gib},
                            now);
    require(view->load() != nullptr, "delivered default budget is published");
    require(policy->reportDevice(103, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = 24 * gib,
                                       .available_bytes = 22 * gib}),
            "device capacity reported");
    auto fill = builder.build(plan, request);
    auto viewer = builder.build(plan, request);
    auto prefetch = builder.build(plan, request);
    require(fill.scene && fill.phaseLease, "first default-ceiling preparation admits");
    require(viewer.scene && viewer.phaseLease,
            "concurrent default-ceiling preparation admits while the device has room");
    require(prefetch.scene && prefetch.phaseLease,
            "a third concurrent default-ceiling preparation still admits");
    require(policy->snapshot().reservations.deviceReservedBytes <= 22 * gib,
            "shared ceiling grants never over-reserve the device");
}

void mixedAndNestedQuotas() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("bloom-phase-preparation-" + std::to_string(now.time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup final {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } cleanup{directory};
    const auto path = directory / "source.exr";
    writeExrRgba(path, 3, 2, signedHdrPixels());
    const auto asset = imageAsset(path, "source", 892000);
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(directory);
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(1);
    GpuScenePreparationPhase phase{.policy = policy,
                                   .envelope = {.metadataBytes = 1024,
                                                .retainedCoverageByteLimit = 65536,
                                                .requestedExecutorBytes = gib,
                                                .deviceWorkingBytes = 320ULL * 1024 * 1024}};
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = now;
    const auto captured = bloom::media::captureImageSourceRevision(path, gib);
    if (!captured.value.has_value())
        throw std::runtime_error("mixed fixture captures actual encoded revision bytes");
    const std::uint64_t sourceAllowance = (*captured.value)->retainedBytes() + 1024;
    // The decoded upload is still96B. Immutable encoded backing/header metadata must now also
    // fit the source working envelope; none may borrow the independent coverage partition.
    // Native output and processing scratch each own a separate metadata allowance.
    const std::size_t fixedHeadroom = 120ULL * 8 + 2ULL * 1024 + 2ULL * 65536;
    const std::size_t headroom = fixedHeadroom + 3 * sourceAllowance;
    sample.process.domains.push_back({.path = "/own",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = headroom,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib, now);
    require(policy->reportDevice(102, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = gib,
                                       .available_bytes = gib}),
            "mixed fixture native quota reported");
    const CpuGpuSceneBuilder builder(nullptr, GpuSceneMediaContext::fromEvaluator(evaluator), {},
                                     phase);
    const auto base = bloom::gpu_coverage_fixtures::twoLayerPlan(
        bloom::gpu_coverage_fixtures::format(12, 10), {}, {}, 6.0, 4.0, 893000);
    const CompiledOperation source = CompiledImageSource{
        bloom::document::NodeId::fromRaw(894000), asset, 0, 0, 0, std::string{}, false};
    {
        auto lowDefinition = base->copyDefinition();
        lowDefinition.operations[0] = source;
        lowDefinition.operations[2] = bloom::gpu_coverage_plans::text(895000);
        auto lowPlan = bloom::gpu_coverage_fixtures::publish(std::move(lowDefinition));
        auto lowRequest = bloom::gpu_coverage_fixtures::requestFor(*lowPlan);
        lowRequest.pixelStorageByteLimit = gib;
        sample.process.domains[0].limitBytes = fixedHeadroom + 3ULL * 96;
        sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
        ++sample.sampleSerial;
        policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
        const auto low = builder.build(lowPlan, lowRequest);
        require(!low.scene &&
                    low.diagnostic.code == PreparedGpuSceneDiagnosticCode::MediaUnavailable &&
                    low.diagnostic.message.find("Image revision and decode exceed") !=
                        std::string::npos &&
                    policy->snapshot().reservations.admittedPhases == 0,
                "former decoded-only96B envelope refuses encoded revision and decode without "
                "borrowing coverage");
        sample.process.domains[0].limitBytes = headroom;
        sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
        ++sample.sampleSerial;
        policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    }
    const auto decodedUploadBytes = [](const PreparedGpuScene& scene) {
        std::uint64_t bytes = 0;
        for (const auto& command : scene.commands())
            if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command))
                bytes += upload->image->pixels().size_bytes();
        return bytes;
    };
    std::shared_ptr<const CompiledCompositionPlan> mixed;
    for (const bool textFirst : {true, false}) {
        auto definition = base->copyDefinition();
        definition.duration = bloom::core::RationalTime::fromInteger(100);
        definition.operations[textFirst ? 0 : 2] = bloom::gpu_coverage_plans::text(895000);
        definition.operations[textFirst ? 2 : 0] = source;
        auto plan = bloom::gpu_coverage_fixtures::publish(std::move(definition));
        auto request = bloom::gpu_coverage_fixtures::requestFor(*plan);
        request.pixelStorageByteLimit = gib;
        auto prepared = builder.build(plan, request);
        if (!prepared.scene) {
            std::cerr << "mixed textFirst=" << textFirst << " code="
                      << bloom::gpu_coverage_fixtures::codeName(prepared.diagnostic.code)
                      << " detail=" << prepared.diagnostic.message << '\n';
        } else if (prepared.phaseLease) {
            std::cerr << "mixed S=" << prepared.phaseLease->sourceByteLimit() << '\n';
        }
        require(prepared.scene && prepared.phaseLease &&
                    prepared.phaseLease->sourceByteLimit() >= sourceAllowance &&
                    decodedUploadBytes(*prepared.scene) == 96,
                "text-before-media and media-before-text admit96B upload plus encoded revision "
                "independently "
                "of coverage");
        require(policy->snapshot().reservations.totalHostClaimBytes == fixedHeadroom + 96,
                "native execution retains decoded sources, not unused uploads or spent "
                "decoder/conversion workspace");
        mixed = std::move(plan);
    }
    require(policy->snapshot().reservations.admittedPhases == 0,
            "mixed fixtures release parent phase between orderings");
    auto parent = bloom::gpu_coverage_plans::nestedParentPlan(mixed, 896000, 777);
    auto request = bloom::gpu_coverage_fixtures::requestFor(*parent);
    request.pixelStorageByteLimit = gib;
    {
        auto nested = builder.build(parent, request);
        if (nested.phaseLease)
            std::cerr << "nested S=" << nested.phaseLease->sourceByteLimit()
                      << " claim=" << nested.phaseLease->sourceClaimBytes()
                      << " allowance=" << sourceAllowance
                      << " phases=" << policy->snapshot().reservations.admittedPhases << '\n';
        else
            std::cerr << "nested code="
                      << bloom::gpu_coverage_fixtures::codeName(nested.diagnostic.code)
                      << " detail=" << nested.diagnostic.message << '\n';
        require(nested.scene && nested.phaseLease &&
                    nested.phaseLease->sourceByteLimit() >= sourceAllowance &&
                    policy->snapshot().reservations.admittedPhases == 1,
                "nested mixed media/text borrows one parent phase and disjoint quotas");
    }
    auto sourceOnlyDefinition = base->copyDefinition();
    sourceOnlyDefinition.duration = bloom::core::RationalTime::fromInteger(100);
    sourceOnlyDefinition.operations[0] = source;
    auto sourceOnly = bloom::gpu_coverage_fixtures::publish(std::move(sourceOnlyDefinition));
    auto sourceParent = bloom::gpu_coverage_plans::nestedParentPlan(sourceOnly, 897000, 778);
    auto zeroCoverage = phase;
    zeroCoverage.envelope.retainedCoverageByteLimit = 0;
    const CpuGpuSceneBuilder sourceBuilder(nullptr, GpuSceneMediaContext::fromEvaluator(evaluator),
                                           {}, zeroCoverage);
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.sampleSerial = 4;
    sample.process.domains[0].limitBytes = 120ULL * 8 + 2ULL * 1024 + 3 * sourceAllowance;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    {
        auto sourceRequest = bloom::gpu_coverage_fixtures::requestFor(*sourceParent);
        sourceRequest.pixelStorageByteLimit = gib;
        auto nested = sourceBuilder.build(sourceParent, sourceRequest);
        require(nested.scene && nested.phaseLease &&
                    nested.phaseLease->sourceByteLimit() >= sourceAllowance,
                "nested source-onlyC0 preserves foreground decode/capture allowance independently");
    }
    auto solidDefinition = base->copyDefinition();
    solidDefinition.duration = bloom::core::RationalTime::fromInteger(100);
    auto solidOnly = bloom::gpu_coverage_fixtures::publish(std::move(solidDefinition));
    auto solidParent = bloom::gpu_coverage_plans::nestedParentPlan(solidOnly, 898000, 779);
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.sampleSerial = 5;
    sample.process.domains[0].limitBytes = 120ULL * 8 + 2ULL * 1024 + 3;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    {
        auto solidRequest = bloom::gpu_coverage_fixtures::requestFor(*solidParent);
        solidRequest.pixelStorageByteLimit = gib;
        auto nested = sourceBuilder.build(solidParent, solidRequest);
        require(nested.scene && nested.phaseLease && nested.phaseLease->sourceByteLimit() >= 1,
                "nested source-free output keeps foreground preflight independent of tiny "
                "retainedS1/C0");
    }
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.sampleSerial = 6;
    sample.process.domains[0].limitBytes = headroom;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    std::uint64_t oneTextCoverage = 0;
    auto textDefinition = base->copyDefinition();
    textDefinition.duration = bloom::core::RationalTime::fromInteger(100);
    textDefinition.operations[0] = bloom::gpu_coverage_plans::text(895000);
    auto textOnly = bloom::gpu_coverage_fixtures::publish(std::move(textDefinition));
    {
        auto textRequest = bloom::gpu_coverage_fixtures::requestFor(*textOnly);
        textRequest.pixelStorageByteLimit = gib;
        auto prepared = builder.build(textOnly, textRequest);
        require(prepared.scene != nullptr, "single text footprint prepares");
        for (const auto& command : prepared.scene->commands()) {
            if (const auto* coverage = std::get_if<GpuSceneCoverageSolidCommand>(&command)) {
                oneTextCoverage += bloom::runtime::detail::gpuSceneCoverageHostBytes(*coverage);
            }
        }
    }
    require(oneTextCoverage != 0, "text footprint owns genuine retained coverage");
    auto siblingDefinition = base->copyDefinition();
    siblingDefinition.compositionId = bloom::document::CompositionId::fromRaw(780);
    siblingDefinition.duration = bloom::core::RationalTime::fromInteger(100);
    siblingDefinition.operations[0] = bloom::gpu_coverage_plans::text(895000);
    siblingDefinition.operations[2] = CompiledCompositionSource{
        bloom::document::NodeId::fromRaw(899000), 0,
        CompiledCompositionTimeMapping{{bloom::document::ParameterId::fromRaw(899001), 0.0},
                                       {bloom::document::ParameterId::fromRaw(899002), 1.0},
                                       0}};
    siblingDefinition.nestedPlans.push_back(textOnly);
    auto sibling = bloom::gpu_coverage_fixtures::publish(std::move(siblingDefinition));
    auto exactCoverage = phase;
    exactCoverage.envelope.retainedCoverageByteLimit = oneTextCoverage;
    const CpuGpuSceneBuilder siblingBuilder(nullptr, GpuSceneMediaContext::fromEvaluator(evaluator),
                                            {}, exactCoverage);
    auto siblingRequest = bloom::gpu_coverage_fixtures::requestFor(*sibling);
    siblingRequest.pixelStorageByteLimit = gib;
    auto exhausted = siblingBuilder.build(sibling, siblingRequest);
    require(!exhausted.scene &&
                exhausted.diagnostic.code == PreparedGpuSceneDiagnosticCode::BudgetRefused &&
                exhausted.diagnostic.message.find("Borrowed coverage capacity") !=
                    std::string::npos,
            "parent-usedC refuses text child before borrowed vector preparation");
    const auto legacyPath = directory / "legacy-source.exr";
    writeExrRgba(legacyPath, 16, 16,
                 std::vector<ExrPixel>(256, ExrPixel{0.25F, 0.5F, 0.75F, 1.0F}));
    const auto legacyAsset = imageAsset(legacyPath, "legacy-source", 900001);
    auto legacyMedia = GpuSceneMediaContext::fromEvaluator(evaluator);
    legacyMedia.preparedUploadCache = std::make_shared<GpuPreparedUploadCache>(1024ULL * 1024, 16);
    const CpuGpuSceneBuilder legacy(nullptr, legacyMedia);
    for (const bool textFirst : {true, false}) {
        auto definition = base->copyDefinition();
        definition.operations[textFirst ? 0 : 2] = bloom::gpu_coverage_plans::text(900010);
        definition.operations[textFirst ? 2 : 0] = CompiledImageSource{
            bloom::document::NodeId::fromRaw(900020), legacyAsset, 0, 0, 0, std::string{}, false};
        auto plan = bloom::gpu_coverage_fixtures::publish(std::move(definition));
        auto legacyRequest = bloom::gpu_coverage_fixtures::requestFor(*plan);
        legacyRequest.pixelStorageByteLimit = gib;
        {
            auto warm = legacy.build(plan, legacyRequest);
            require(warm.scene != nullptr, "legacy mixed source prewarms genuine upload cache");
        }
        legacyRequest.pixelStorageByteLimit = 4096;
        auto limited = legacy.build(plan, legacyRequest);
        require(
            !limited.scene && limited.diagnostic.code ==
                                  PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
            "legacy no-policy shared4096B cap refuses upload+coverage in either operation order");
    }
    auto tinyCoverage = phase;
    tinyCoverage.envelope.retainedCoverageByteLimit = 1;
    const CpuGpuSceneBuilder constrained(nullptr, GpuSceneMediaContext::fromEvaluator(evaluator),
                                         {}, tinyCoverage);
    auto refused = constrained.build(parent, request);
    require(!refused.scene &&
                refused.diagnostic.code == PreparedGpuSceneDiagnosticCode::BudgetRefused,
            "nested coverage cannot escape exhausted parent coverage quota");
}
} // namespace
int main() {
    try {
        prepare();
        finalPreparation();
        mixedAndNestedQuotas();
        concurrentDefaultCeiling();
        std::cout << "production before-preparation denial and1080 solid/text admission PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
