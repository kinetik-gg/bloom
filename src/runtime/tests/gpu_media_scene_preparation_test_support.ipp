// Shared fixtures, command finders, CPU-oracle parity replay helper, and the cancelled-token helper
// for the CPU GPU-scene media preparation tests. Every EXR fixture is a REAL file written to a
// throwaway directory and resolved through the production image pipeline, so the tests exercise
// genuine decode/colour-conversion behaviour. Included by gpu_media_scene_preparation_tests.cpp
// inside its anonymous namespace.

using bloom::runtime::GpuPreparedUploadCache;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneMediaStatistics;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProxyResolution;

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const bloom::runtime::GpuSceneTranslationCommand*
firstTranslation(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* translation =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command)) {
            return translation;
        }
    }
    return nullptr;
}

struct MediaFixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    document::AssetRecord asset;
};

[[nodiscard]] MediaFixture makeFixture() {
    MediaFixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_scene_prep_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "signed_hdr.exr";
    writeExrRgba(fixture.path, 3, 2, signedHdrPixels());
    fixture.asset = imageAsset(fixture.path, "signed_hdr", 900);
    return fixture;
}

// Full parity against a genuine, uncached CpuCompositionEvaluator frame: identity, bounds, output
// descriptor and every pixel, plus upload bounds.
void checkMediaParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                      const std::shared_ptr<const CompiledCompositionPlan>& plan,
                      const EvaluationRequest& request, const std::string& label) {
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (!frame.frame()) {
        return;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": identity matches");
    expectations.expect(
        std::ranges::equal(prepared.scene->bounds(), frame.frame()->evaluatedBounds()),
        label + ": bounds match");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches");
    expectations.expect(prepared.scene->mediaStatistics().imageSources == 1 &&
                            prepared.scene->mediaStatistics().imageConversions == 1,
                        label + ": exactly one image source was converted");
    double hScale = 1.0;
    double vScale = 1.0;
    if (const auto* proxy = std::get_if<bloom::runtime::ProxyResolution>(&request.resolution)) {
        hScale = static_cast<double>(proxy->extent.width()) /
                 static_cast<double>(plan->format().width());
        vScale = static_cast<double>(proxy->extent.height()) /
                 static_cast<double>(plan->format().height());
    }
    std::vector<std::shared_ptr<const Rgba32fImage>> images;
    expectations.expect(replayScene(*prepared.scene, images, hScale, vScale), label + ": replays");
    const auto& replayed = images[prepared.scene->outputCommand()];
    expectations.expect(
        replayed != nullptr &&
            replayed->pixels().size() == frame.frame()->processImage().pixels().size() &&
            std::memcmp(replayed->pixels().data(), frame.frame()->processImage().pixels().data(),
                        replayed->pixels().size() * sizeof(Rgba32f)) == 0,
        replayed == nullptr
            ? label + ": replay produced no image"
            : label + ": pixel parity " + firstMismatch(*replayed, frame.frame()->processImage()));
}

// Obtain a live, already-cancelled CancellationToken. The only public source of one is a real
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
        TaskRequest("gpu media cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(77)}),
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
