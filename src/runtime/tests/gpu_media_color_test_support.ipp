// Shared fixtures and CPU-oracle helpers for the bounded media-source GPU colour tests. Included by
// gpu_media_color_tests.cpp inside its anonymous namespace together with the still and video test
// groups, so the executables and test names are unchanged.

namespace document = bloom::document;

using bloom::render::ImageWindow;
using bloom::render::Rgba32fImage;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorCounters;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProxyResolution;

using bloom::runtime::media_executor_test::descriptorMatchesScene;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kReadbackBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

constexpr std::string_view kAcesWorking = "ACEScg";
constexpr std::string_view kAcesAlternateWorking = "ACES2065-1";
constexpr std::string_view kAcesTextureInput = "sRGB - Texture";
[[maybe_unused]] constexpr int kSkipExit = 77;

[[nodiscard]] bloom::runtime::CancellationToken makeCancelledToken();

struct Fixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    document::AssetRecord asset;
};

[[nodiscard]] Fixture makeAcesFixture() {
    Fixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_color_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "aces_ap0.exr";
    writeExrRgbaWithChromaticities(fixture.path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
    fixture.asset = imageAsset(fixture.path, "aces_ap0", 700);
    return fixture;
}

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const GpuSceneOcioEffectCommand* firstOcio(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            return ocio;
        }
    }
    return nullptr;
}

[[nodiscard]] const bloom::runtime::GpuScenePointResampleCommand*
firstResample(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* resample =
                std::get_if<bloom::runtime::GpuScenePointResampleCommand>(&command)) {
            return resample;
        }
    }
    return nullptr;
}

[[nodiscard]] bloom::runtime::EvaluationRequest acesRequest(const CompiledCompositionPlan& plan,
                                                            const std::string_view working) {
    auto request = requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = working,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    return request;
}

// Full parity for one non-identity still scene: the prepared command shape, the raw upload
// dimensions/display, the OCIO ProcessEffect wiring, and every output pixel versus the unchanged
// CPU oracle at the 2e-6 gate.
[[nodiscard]] bool expectMediaParity(Expectations& expectations, GpuSceneExecutor& executor,
                                     const CpuCompositionEvaluator& evaluator,
                                     const CpuGpuSceneBuilder& builder,
                                     const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                     const bloom::runtime::EvaluationRequest& request,
                                     const std::uint64_t sourceWidth,
                                     const std::uint64_t sourceHeight, const std::string& label) {
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return false;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && ocio != nullptr,
                        label + ": emits a raw upload and a real OCIO colour command");
    if (upload == nullptr || ocio == nullptr) {
        return false;
    }
    expectations.expect(prepared.scene->mediaStatistics().ocioCommandPreparations == 1 &&
                            prepared.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            prepared.scene->mediaStatistics().uploadCacheHits == 0,
                        label + ": the cold build decodes raw and prepares one OCIO command");
    const auto sourceWindow = ImageWindow::create(0, 0, sourceWidth, sourceHeight);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        label + ": the upload keeps the full source dimensions");
    expectations.expect(
        upload->descriptor.displayWindow() == prepared.scene->outputDescriptor().displayWindow() &&
            upload->descriptor.pixelAspect() == prepared.scene->outputDescriptor().pixelAspect(),
        label + ": the upload keeps the composition display window and PAR");
    expectations.expect(ocio->inputKey == upload->semanticKey && ocio->program != nullptr &&
                            ocio->program->encoding() ==
                                bloom::runtime::GpuOcioOutputEncoding::FinalRgba32f,
                        label + ": the OCIO command consumes the raw upload as a ProcessEffect");
    expectations.expect(ocio->semanticKey != upload->semanticKey &&
                            ocio->outputWindow == upload->descriptor.dataWindow(),
                        label + ": the effect identity is independent of the decode identity");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return false;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": process identity matches the CPU frame");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches the CPU frame");

    const auto run = runScene(executor, prepared.scene, kSceneBudget);
    if (!run.ready) {
        std::cerr << label
                  << ": executor diagnostic code=" << static_cast<int>(executor.diagnostic().code)
                  << " message=" << executor.diagnostic().message << '\n';
    }
    expectations.expect(run.ready, label + ": the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return false;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        label + ": the native descriptor matches the scene");
    const auto counters = executor.counters();
    expectations.expect(counters.ocioEffectDispatches >= 1,
                        label + ": the colour conversion was a dispatched GPU OCIO effect");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), label + ": the output reads back for the oracle");
    if (!readback) {
        return false;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        label + ": the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return false;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        label + ": every pixel is within the 2e-6 process gate");
    return true;
}

// A live, already-cancelled CancellationToken. The only public source of one is a real
// TaskScheduler task (cancellation.hpp), so this runs one, copies its token, cancels, and lets it
// finish.
[[nodiscard]] bloom::runtime::CancellationToken makeCancelledToken() {
    using namespace bloom::runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("gpu media colour cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(88)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted()) {
        throw std::logic_error("cancel token task was refused");
    }
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return token;
}
