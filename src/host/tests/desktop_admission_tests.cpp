// Desktop-shaped admission repro: one shared processing root, as apps/bloom wires it, with the
// Viewer and a RAM-preview fill working on a 2048x858 footage-and-layers composition while the
// artist exports that composition. The export uses the real attempt runner (and the real GPU
// export provider when a loader is supplied) against the same root.
//
// The owner's report: on a 24 GiB GPU with about 20 GiB of free RAM, the desktop export failed
// on frame 1 with "stage=evaluating; code=9" (BudgetRefused) while the same composition exported
// headlessly. Admission must use actual capacity and each phase's actual need, so concurrent
// preview work never makes an export refuse while the machine is mostly free.
#include "gpu_export_provider_test_access.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/packed_host_retirement.hpp>
#include <bloom/runtime/task_budget_view.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace document = bloom::document;
namespace host = bloom::host;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
using namespace std::chrono_literals;

constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kGiB = 1024 * kMiB;
constexpr int kWidth = 2048;
constexpr int kHeight = 858;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
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

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-desktop-admission-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        if (const auto* result = ::mkdtemp(pattern.data()); result != nullptr)
            path_ = result;
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        std::error_code error;
        if (!path_.empty())
            std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void writeFootage(const std::filesystem::path& path) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    Imf::Header header(window, window);
    for (const char* name : {"R", "G", "B", "A"})
        header.channels().insert(name, Imf::Channel(Imf::HALF));
    const auto pixels = static_cast<std::size_t>(kWidth) * kHeight;
    std::array<std::vector<Imath::half>, 4> planes;
    for (std::size_t channel = 0; channel < 4; ++channel)
        planes[channel].resize(pixels);
    for (std::size_t index = 0; index < pixels; ++index) {
        const auto x = static_cast<float>(index % kWidth) / kWidth;
        const auto y = static_cast<float>(index / kWidth) / kHeight;
        planes[0][index] = Imath::half(x);
        planes[1][index] = Imath::half(y);
        planes[2][index] = Imath::half(0.5F * (x + y));
        planes[3][index] = Imath::half(1.0F);
    }
    Imf::FrameBuffer frameBuffer;
    const char* names[] = {"R", "G", "B", "A"};
    for (std::size_t channel = 0; channel < 4; ++channel)
        frameBuffer.insert(names[channel],
                           Imf::Slice(Imf::HALF, reinterpret_cast<char*>(planes[channel].data()),
                                      sizeof(Imath::half), sizeof(Imath::half) * kWidth));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(kHeight);
}

[[nodiscard]] std::optional<document::AssetRecord> footageAsset(const std::filesystem::path& path) {
    // The asset's content digest is the SHA-256 of the file bytes, as the image probe records it.
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::vector<char> fileBytes((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
    bloom::core::Sha256Hasher hasher;
    if (!hasher.update(std::as_bytes(std::span(fileBytes.data(), fileBytes.size()))))
        return std::nullopt;
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(0x2001);
    asset.kind = document::AssetKind::Image;
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:";
    asset.contentDigest = hasher.finalize();
    asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
    asset.width = static_cast<std::uint32_t>(kWidth);
    asset.height = static_cast<std::uint32_t>(kHeight);
    asset.name = "footage";
    return asset;
}

// Footage plus three transformed layers (scaled/rotated footage, two solids with opacity and
// translation), merged into the composition output.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
footagePlan(const document::AssetRecord& asset) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    if (!format)
        std::abort();
    std::uint64_t nextId = 0x3000;
    const auto node = [&] { return document::NodeId::fromRaw(++nextId); };
    const auto parameter = [&] { return document::ParameterId::fromRaw(++nextId); };
    std::vector<runtime::CompiledOperation> operations;
    std::vector<runtime::CompiledMergeInput> inputs;
    const auto layer = [&](const std::size_t source, const document::Vec2d position,
                           const document::Vec2d scale, const double rotation,
                           const double opacity) {
        const auto layerId = document::LayerId::fromRaw(++nextId);
        operations.emplace_back(runtime::CompiledLayerOutput{
            node(), layerId, runtime::OperationIndex::fromRaw(static_cast<std::uint32_t>(source)),
            runtime::CompiledVec2Parameter{parameter(), position},
            runtime::CompiledVec2Parameter{parameter(), document::kDefaultAnchor},
            runtime::CompiledVec2Parameter{parameter(), scale},
            runtime::CompiledScalarParameter{parameter(), rotation},
            runtime::CompiledScalarParameter{parameter(), opacity}, parameter(),
            bloom::core::kDefaultBlendMode});
        inputs.push_back(
            {document::LayerSlotId::fromRaw(++nextId), layerId,
             runtime::OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size() - 1))});
    };
    operations.emplace_back(runtime::CompiledImageSource{node(), asset, 0, 0, 0, {}, false});
    layer(0, {0.5, 0.5}, {1.0, 1.0}, 0.0, 1.0);
    operations.emplace_back(runtime::CompiledImageSource{node(), asset, 0, 0, 0, {}, false});
    layer(operations.size() - 1, {0.4, 0.55}, {0.6, 0.6}, 12.0, 0.8);
    operations.emplace_back(
        runtime::CompiledSolid{node(),
                               {parameter(), bloom::core::Color4d{0.9, 0.3, 0.1, 1.0}},
                               {parameter(), 640.0},
                               {parameter(), 360.0}});
    layer(operations.size() - 1, {0.25, 0.3}, {1.2, 0.8}, -20.0, 0.5);
    operations.emplace_back(
        runtime::CompiledSolid{node(),
                               {parameter(), bloom::core::Color4d{0.1, 0.4, 0.9, 1.0}},
                               {parameter(), 512.0},
                               {parameter(), 512.0}});
    layer(operations.size() - 1, {0.75, 0.7}, {0.9, 0.9}, 45.0, 0.7);
    operations.emplace_back(runtime::CompiledMerge{node(), std::move(inputs)});
    operations.emplace_back(runtime::CompiledCompositionOutput{
        node(),
        runtime::OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size() - 1))});
    const auto outputIndex =
        runtime::OperationIndex::fromRaw(static_cast<std::uint32_t>(operations.size() - 1));
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            .sourceRevision = document::Revision::fromRaw(0x2002),
            .projectId = document::ProjectId::fromRaw(0x2003),
            .compositionId = document::CompositionId::fromRaw(0x2004),
            .format = *format,
            .operations = std::move(operations),
            .output = outputIndex});
}

[[nodiscard]] const std::shared_ptr<render::PackedHostRetirementOwner>& retirementOwner() {
    static const auto owner = render::PackedHostRetirementOwner::create();
    return owner;
}

// The desktop root: one policy shared by the preview service, RAM preview and the export
// provider, fed by the off-UI host sampler and the preview owner's live device report.
struct DesktopRoot final {
    std::shared_ptr<runtime::TaskBudgetView> view = std::make_shared<runtime::TaskBudgetView>();
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirementOwner(), view);
    std::uint64_t serial = 0;
    DesktopRoot() { policy->setHeadroom(runtime::MemoryHeadroom{}); }

    void deliverHostSample() {
        runtime::MemoryBudgetState sample;
        sample.machine.availableBytes = 20 * kGiB;
        sample.process.status = runtime::ProcessMemoryStatus::Unsupported;
        sample.process.source = runtime::ProcessMemorySource::OtherHostOnly;
        sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
        sample.processSampleFresh = true;
        sample.sampleSerial = ++serial;
        policy->applyHostSample(sample, 64 * kGiB,
                                {.usableByteBudget = 32 * kGiB,
                                 .operationCacheByteBudget = 18 * kGiB,
                                 .previewFrameCacheByteBudget = 12 * kGiB},
                                sample.process.sampledAt);
    }
};

void printRoot(const DesktopRoot& root, const std::string_view label) {
    const auto snapshot = root.policy->snapshot().reservations;
    std::cerr << label << ": hostCapacity=" << snapshot.hostCapacityBytes.value_or(0)
              << " hostClaims=" << snapshot.totalHostClaimBytes
              << " deviceCapacity=" << snapshot.deviceCapacityBytes.value_or(0)
              << " deviceClaims=" << snapshot.totalDeviceClaimBytes
              << " phases=" << snapshot.admittedPhases << " refusals=" << snapshot.budgetRefusals
              << '\n';
}

// The preview GPU-scene envelope apps/bloom configures, with the request ceiling the preview
// controller reads from the delivered root view, for one 2048x858 Viewer/RAM-fill frame.
[[nodiscard]] render::GpuProcessingPhaseGrant reservePreviewFrame(DesktopRoot& root) {
    const auto delivered = root.view->load();
    const auto pixels = static_cast<std::uint64_t>(kWidth) * kHeight;
    return root.policy->reserve(
        {.output = runtime::GpuProcessingPhaseOutput::PackedPreview,
         .pixels = pixels,
         .metadataBytes = 16 * kMiB,
         .retainedCoverageByteLimit = 16 * kMiB,
         .requestedSourceBytes = delivered ? delivered->previewRequestBytes : 0,
         .requestedExecutorBytes =
             root.policy->snapshot().reservations.deviceCapacityBytes.value_or(0),
         .minimumExecutorBytes = pixels * 16,
         .deviceWorkingBytes = 512 * kMiB + 320 * kMiB});
}

[[nodiscard]] std::optional<host::OutputAnalysisAttemptOutcomeV1>
runExport(Expectations& checks, DesktopRoot& root,
          const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
          const std::filesystem::path& projectDirectory, const std::filesystem::path& target,
          std::shared_ptr<host::GpuExportProvider> provider) {
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    checks.expect(artifacts.succeeded(), "export: artifact coordinator");
    if (!artifacts)
        return std::nullopt;
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    host::OutputAnalysisAttemptRequestV1 request{
        .plan = plan,
        .evaluation = {.time = bloom::core::RationalTime::fromInteger(0),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = 0},
        .targetPath = target,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(9)},
        .processingPhasePolicy = root.policy};
    request.assetBaseDirectory = projectDirectory;
    request.useRootPixelCapacity = true;
    request.gpuProvider = std::move(provider);
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    checks.expect(static_cast<bool>(begin), "export: the attempt begins");
    if (!begin)
        return std::nullopt;
    auto runner = std::move(begin).takeHandle();
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = runner.tryComplete())
            return outcome;
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

void reportOutcome(const std::optional<host::OutputAnalysisAttemptOutcomeV1>& outcome) {
    if (!outcome) {
        std::cerr << "export outcome: none (timed out)\n";
        return;
    }
    std::cerr << "export outcome: " << host::describeOutputAnalysisAttemptV1(*outcome) << '\n';
}

std::shared_ptr<host::GpuExportProvider> exportProvider(Expectations& checks, DesktopRoot& root,
                                                        runtime::TaskScheduler& scheduler,
                                                        const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        checks.expect(!requireDevice, "--require-device needs BLOOM_TEST_VULKAN_LOADER");
        std::cout << "NOTE: no loader; the export takes the CPU reference host phase\n";
        return {};
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.processingPhasePolicy = root.policy;
    options.mediaContextProvider = [] {
        runtime::GpuSceneMediaContext context;
        context.preparedUploadCache = std::make_shared<runtime::GpuPreparedUploadCache>(0, 0);
        return context;
    };
    auto provider = host::GpuExportProvider::create(std::move(options));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    if (!provider->deviceAvailable()) {
        checks.expect(!requireDevice, "a GPU device is required");
        std::cout << "NOTE: no compatible device; the export takes the CPU reference host phase\n";
        static_cast<void>(provider->shutdownAndWait(10s));
        return {};
    }
    return provider;
}

// Viewer frame and RAM-fill frame in preparation on the shared root while the export starts.
void testExportBesidePreviewPreparation(Expectations& checks, const bool requireDevice) {
    TempDirectory directory;
    const auto footagePath = directory.path() / "footage.exr";
    writeFootage(footagePath);
    const auto asset = footageAsset(footagePath);
    checks.expect(asset.has_value(), "2048x858 footage is probed");
    if (!asset)
        return;
    const auto plan = footagePlan(*asset);
    DesktopRoot root;
    root.deliverHostSample();
    // The preview owner's live VK_EXT_memory_budget report: about 22 GiB free on a 24 GiB GPU.
    checks.expect(
        root.policy->reportDevice(1, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                      .device_local_bytes = 24 * kGiB,
                                      .available_bytes = 22 * kGiB}),
        "preview owner reports its live device budget");
    runtime::TaskScheduler providerScheduler;
    auto provider = exportProvider(checks, root, providerScheduler, requireDevice);
    root.deliverHostSample();

    auto viewer = reservePreviewFrame(root);
    checks.expect(viewer.lease != nullptr, "a Viewer frame is admitted on the shared root");
    auto ramFill = reservePreviewFrame(root);
    checks.expect(ramFill.lease != nullptr,
                  "a RAM-preview fill frame is admitted beside the Viewer frame");
    printRoot(root, "preview frames in preparation");
    const auto outcome =
        runExport(checks, root, plan, directory.path(), directory.path() / "frame.exr", provider);
    checks.expect(outcome.has_value() && static_cast<bool>(*outcome),
                  "the 2048x858 export frame renders beside Viewer and RAM-fill work");
    if (!outcome || !*outcome) {
        reportOutcome(outcome);
        printRoot(root, "after export");
    }
    viewer.lease.reset();
    ramFill.lease.reset();
    if (provider)
        checks.expect(provider->shutdownAndWait(10s), "export provider retires");
}
} // namespace

int main(const int argc, char** argv) {
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index)
        requireDevice = requireDevice || std::string_view(argv[index]) == "--require-device";
    Expectations checks;
    testExportBesidePreviewPreparation(checks, requireDevice);
    retirementOwner()->beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!retirementOwner()->exitAcknowledged() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    checks.expect(retirementOwner()->collect(), "packed host retirement collects");
    if (checks.failures() != 0) {
        std::cerr << checks.failures() << " desktop admission expectation(s) failed\n";
        return 1;
    }
    std::cout << "desktop admission: all expectations passed\n";
    return 0;
}
