// Actual source-edit reuse through the production builder/executor and one live HOST LRU.
// CLI members are read-only inputs; no path or machine capacity is embedded in this fixture.
#include "final_host_test_support.hpp"
#include "gpu_media_executor_test_support.hpp"
#include "gpu_media_scene_preparation_test_support.hpp"

#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
namespace runtime = bloom::runtime;
namespace render = bloom::render;
using namespace std::chrono_literals;
using runtime::media_executor_test::pixelsClose;
using runtime::media_executor_test::runScene;
constexpr std::uint32_t kWidth = 4608;
constexpr std::uint32_t kHeight = 3164;
constexpr std::uint64_t kPayload = 16ULL * kWidth * kHeight;
// Separate configured native/finished and oracle controls, NOT the HOST reusable assignment.
constexpr std::uint64_t kNativeRetention = 2ULL << 30U;
constexpr std::uint64_t kOracleBudget = 1ULL << 30U;

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Member final {
    std::int64_t frame = 0;
    std::filesystem::path path;
    std::string digest;
};
struct Options final {
    std::filesystem::path loader;
    std::vector<Member> members;
    bool requireDevice = false;
};
Options parse(const int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--loader" && i + 1 < argc)
            result.loader = argv[++i];
        else if (arg == "--require-device")
            result.requireDevice = true;
        else if (arg == "--member" && i + 3 < argc) {
            Member member;
            member.frame = std::stoll(argv[++i]);
            member.path = argv[++i];
            member.digest = argv[++i];
            require(bloom::core::Sha256Digest::fromLowercaseHex(member.digest).has_value(),
                    "member digest must be SHA256");
            result.members.push_back(std::move(member));
        } else
            throw std::invalid_argument("invalid source-reuse fixture arguments");
    }
    return result;
}

// Sole independent sampler: Tasks/phase admission cannot depend on a stage that has not run yet.
struct Root final {
    render::test::FinalHostRetirementScope retirement;
    runtime::MemoryBudgetSample initial = runtime::memoryBudgetSample();
    runtime::MemoryBudgetLedger ledger{initial.physicalBytes,
                                       initial.machine.availableBytes.value_or(0)};
    std::shared_ptr<runtime::TaskBudgetView> view = std::make_shared<runtime::TaskBudgetView>();
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy;
    std::shared_ptr<runtime::OperationCache> pool;
    std::shared_ptr<runtime::GpuPreparedUploadCache> prepared;
    std::jthread sampler;
    std::shared_ptr<runtime::GpuSceneMediaContext> sourceContext =
        std::make_shared<runtime::GpuSceneMediaContext>();
    explicit Root(std::optional<std::uint64_t> cap = {}) {
        ledger.beginProcessSampling();
        policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1, retirement.owner, view);
        pool = std::make_shared<runtime::OperationCache>(ledger.allocate().operationCacheByteBudget,
                                                         ledger);
        prepared = std::make_shared<runtime::GpuPreparedUploadCache>(pool, retirement.owner, cap);
        deliver(std::move(initial));
        sampler = std::jthread([this](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                for (int i = 0; i < 100 && !stop.stop_requested(); ++i)
                    std::this_thread::sleep_for(50ms);
                if (!stop.stop_requested())
                    deliver(runtime::memoryBudgetSample());
            }
        });
    }
    ~Root() {
        sampler.request_stop();
        sampler.join();
        pool->closePreparedUploads();
    }
    void deliver(runtime::MemoryBudgetSample sample) {
        const auto at = runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto state = ledger.applySample(std::move(sample), at);
        policy->applyHostSample(state, ledger.physicalByteBudget(), ledger.allocate(), at);
    }
    runtime::GpuScenePreparationPhase phase() const {
        return {.policy = policy,
                .envelope = {.metadataBytes = 1ULL << 20U,
                             .retainedCoverageByteLimit = 16ULL << 20U,
                             .deviceWorkingBytes = 512ULL << 20U}};
    }
    std::uint64_t requestBytes() const {
        const auto delivered = view->load();
        require(delivered && delivered->fresh(), "actual sampled Tasks delivery must be fresh");
        return delivered->previewRequestBytes;
    }
};

const runtime::GpuSceneUploadCommand& upload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands())
        if (const auto* found = std::get_if<runtime::GpuSceneUploadCommand>(&command))
            return *found;
    throw std::runtime_error("actual media preparation must contain an upload");
}

std::shared_ptr<const CompiledCompositionPlan> editPlan(const document::AssetRecord& asset,
                                                        const int edit) {
    auto base = mediaPlan(format(kWidth, kHeight), asset,
                          LayerValues{.position = {kWidth / 2.0 + (edit == 0 ? 0.0 : 0.3),
                                                   kHeight / 2.0 + (edit == 0 ? 0.0 : 0.2)}},
                          2000);
    std::vector<CompiledOperation> operations(base->operations().begin(), base->operations().end());
    operations.resize(2);
    operations.emplace_back(runtime::CompiledText{
        document::NodeId::fromRaw(3000),
        document::ParameterId::fromRaw(3001),
        "SOURCE REUSE " + std::to_string(std::clamp(edit, 1, 3)),
        {document::ParameterId::fromRaw(3002), 48.0},
        {document::ParameterId::fromRaw(3003), Color4d{0.8, 0.4, 0.2, 1.0}},
        runtime::CompiledTextLayout{document::ParameterId::fromRaw(3004),
                                    0,
                                    {document::ParameterId::fromRaw(3005), 1.0},
                                    {document::ParameterId::fromRaw(3006), 0.0}}});
    operations.emplace_back(layerOutput(
        document::NodeId::fromRaw(3010), document::LayerId::fromRaw(3011),
        OperationIndex::fromRaw(2),
        LayerIds{document::ParameterId::fromRaw(3012), document::ParameterId::fromRaw(3013),
                 document::ParameterId::fromRaw(3014), document::ParameterId::fromRaw(3015),
                 document::ParameterId::fromRaw(3016), document::ParameterId::fromRaw(3017)},
        LayerValues{.position = {256.0, 128.0}}));
    operations.emplace_back(CompiledMerge{
        document::NodeId::fromRaw(3020),
        {CompiledMergeInput{document::LayerSlotId::fromRaw(3022), document::LayerId::fromRaw(3011),
                            OperationIndex::fromRaw(3)},
         CompiledMergeInput{document::LayerSlotId::fromRaw(3021), document::LayerId::fromRaw(2012),
                            OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        CompiledCompositionOutput{document::NodeId::fromRaw(3030), OperationIndex::fromRaw(4)});
    return publish(CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, format(kWidth, kHeight),
        std::move(operations), OperationIndex::fromRaw(5)});
}

struct SourceIdentity final {
    std::string key;
    std::weak_ptr<const render::Rgba32fImage> host;
    std::weak_ptr<const render::GpuImage> native;
};
struct StageEvidence final {
    SourceIdentity source;
    bloom::core::Sha256Digest pixels;
};

StageEvidence execute(Root& root, render::GpuDevice& device, runtime::GpuSceneCache& cache,
                      runtime::GpuSceneExecutor& executor, const CpuCompositionEvaluator& cpu,
                      const runtime::GpuSceneMediaContext& context,
                      const runtime::GpuSceneOcioContext& ocio,
                      const std::shared_ptr<const CompiledCompositionPlan>& plan, const int stage,
                      const std::optional<SourceIdentity>& previous) {
    auto request = requestFor(*plan, RationalTime::fromInteger(0), root.requestBytes());
    if (stage == 4) {
        const auto half = render::ImageExtent::create(kWidth / 2U, kHeight / 2U);
        require(half.hasValue(), "Half extent");
        request.resolution = runtime::ProxyResolution{*half.value()};
    }
    const CpuGpuSceneBuilder builder(nullptr, context, ocio, root.phase());
    auto built = builder.build(plan, request);
    require(built.hasValue(), "source build: " + built.diagnostic.message);
    const auto& source = upload(*built.scene);
    const auto media = built.scene->mediaStatistics();
    const auto output = built.scene->outputDescriptor().dataWindow().extent();
    require(output.width() == (stage == 4 ? kWidth / 2 : kWidth) &&
                output.height() == (stage == 4 ? kHeight / 2 : kHeight),
            "exact native B Full/Half output extent");
    if (previous) {
        const auto held = previous->host.lock();
        require(held && held.get() == source.image.get() && previous->key == source.semanticKey,
                "HOST LRU must retain identical raw source across edit/proxy without caller pin");
        require(media.uploadCacheHits == 1 && media.uploadCacheMisses == 0 &&
                    media.imageConversions == 0,
                "warm HOST preparation work deltas");
    } else
        require(media.uploadCacheMisses == 1 && media.imageConversions == 1,
                "cold source preparation must run once");
    require(device.retainProcessingPhase(built.phaseLease), "exact admitted phase retained");
    const auto before = executor.counters();
    auto run = runScene(executor, built.scene, request.pixelStorageByteLimit);
    require(run.ready && run.image, "real native output: " + executor.diagnostic().message);
    const auto after = executor.counters();
    auto native = cache.find(source.semanticKey);
    require(native != nullptr, "native source retention cannot be substituted by a HOST hit");
    if (previous) {
        const auto held = previous->native.lock();
        require(held && held.get() == native.get() && after.uploads == before.uploads,
                "native source pointer and zero added upload across edits/Half");
    } else
        require(after.uploads == before.uploads + 1, "cold actual native upload once");
    require(after.dispatches > before.dispatches && after.outputCacheHits == before.outputCacheHits,
            "changed output dispatch, no finished-frame hit substitute");
    auto oracleRequest = request;
    oracleRequest.pixelStorageByteLimit = kOracleBudget;
    oracleRequest.bypassOperationCache = true;
    const auto oracle = cpu.evaluate(plan, oracleRequest, {});
    require(oracle.frame() != nullptr, "independent CPU output oracle");
    const auto readback = render::readbackResidentImage(*run.image, kOracleBudget);
    require(readback.hasValue() &&
                pixelsClose(readback.pixels, oracle.frame()->processImage().pixels()),
            "every actual edited output pixel matches independent CPU oracle (2e-6)");
    const auto digest = bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(readback.pixels)));
    if (!digest.has_value())
        throw std::runtime_error("pixel digest");
    StageEvidence result{{source.semanticKey, source.image, native}, digest.value()};
    std::cout << "EDIT stage=" << stage << " hostHit=" << media.uploadCacheHits
              << " conversion=" << media.imageConversions
              << " uploadDelta=" << after.uploads - before.uploads
              << " dispatchDelta=" << after.dispatches - before.dispatches
              << " outputHitDelta=" << after.outputCacheHits - before.outputCacheHits
              << " sourceKey=" << source.semanticKey << " hostPointer=" << source.image.get()
              << " nativePointer=" << native.get() << " outputExtent=" << output.width() << 'x'
              << output.height() << " outputPayload=" << readback.pixels.size() * sizeof(Rgba32f)
              << " sourcePayload=" << source.image->pixels().size_bytes()
              << " hostAssigned=" << root.pool->byteBudget()
              << " hostPayload=" << root.pool->preparedPayloadBytes()
              << " hostCombined=" << root.pool->retainedBytes() << '\n';
    // Return only weak source identity: scenes, CPU oracle and native output die per stage.
    run.image.reset();
    native.reset();
    built.scene.reset();
    const auto leaseId = built.phaseLease->id();
    device.retireProcessingPhase(leaseId, true);
    built.phaseLease.reset();
    require(root.policy->snapshot().reservations.admittedPhases == 0,
            "completed source/output must detach phase before immediate max1 next request");
    return result;
}

// Production final route borrows exactly the preview witness root/pool/owner and delivery.
class FinalRoute final {
    runtime::TaskScheduler scheduler_;

  public:
    std::shared_ptr<bloom::host::GpuExportProvider> provider;
    FinalRoute(Root& root, const std::filesystem::path& loader,
               std::shared_ptr<const runtime::GpuSceneOcioContext> ocio) {
        root.sourceContext->operationCache = root.pool;
        root.sourceContext->preparedUploadCache = root.prepared;
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.loaderPath = loader;
        options.processingPhasePolicy = root.policy;
        options.requestByteBudget = root.requestBytes();
        options.readbackByteBudget = kOracleBudget;
        options.ocioContext = std::move(ocio);
        options.mediaContextProvider = [context = root.sourceContext] { return *context; };
        provider = bloom::host::GpuExportProvider::create(std::move(options));
        provider->prepare(scheduler_);
        const auto until = std::chrono::steady_clock::now() + 60s;
        while (!provider->prepared() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        require(provider->prepared() && provider->deviceAvailable(),
                "actual final provider device");
    }
    runtime::GpuProcessFrameOutcome
    evaluate(const std::shared_ptr<const CompiledCompositionPlan>& plan,
             const bool bypass = false) {
        auto request = requestFor(*plan, RationalTime::fromInteger(0), kOracleBudget);
        request.bypassOperationCache = bypass;
        return provider->evaluator()->evaluate(plan, request);
    }
    void close() { require(provider->shutdownAndWait(30s), "actual final provider native exit"); }
    ~FinalRoute() { (void)provider->shutdownAndWait(30s); }
};
void proveFinal(FinalRoute& final, Root& root, const document::AssetRecord& asset,
                const std::filesystem::path& directory, const SourceIdentity& source) {
    root.sourceContext->assetBaseDirectory = directory;
    auto definition = editPlan(asset, 3)->copyDefinition();
    auto& layer = std::get<CompiledLayerOutput>(definition.operations[1]);
    layer.position.source = document::Vec2d{kWidth / 2.0 + 1.3, kHeight / 2.0 + 0.2};
    const auto plan = publish(std::move(definition));
    const auto hits = root.pool->preparedStatistics().hits;
    auto outcome = final.evaluate(plan);
    std::cout << "FINAL status=" << static_cast<int>(outcome.status)
              << " code=" << static_cast<int>(outcome.diagnostic.code)
              << " message=" << outcome.diagnostic.message
              << " dispatches=" << outcome.counters.nativeDispatches
              << " uploads=" << outcome.counters.uploads
              << " readbacks=" << outcome.counters.readbacks << '\n';
    require(outcome.frame &&
                outcome.frame->identity().provider == runtime::EvaluationProvider::GpuResident &&
                outcome.counters.nativeDispatches > 0 && outcome.counters.readbacks == 1,
            "actual final output must execute natively: " + outcome.diagnostic.message);
    require(root.pool->preparedStatistics().hits == hits + 1 &&
                root.prepared->find(source.key).get() == source.host.lock().get(),
            "actual final route must revalidate and reuse same retained HOST source");
    CpuCompositionEvaluator oracle;
    oracle.setAssetBaseDirectory(directory);
    auto request = requestFor(*plan, RationalTime::fromInteger(0), kOracleBudget);
    request.bypassOperationCache = true;
    const auto cpu = oracle.evaluate(plan, request, {});
    require(cpu.frame() && pixelsClose(outcome.frame->processImage().pixels(),
                                       cpu.frame()->processImage().pixels()),
            "actual final every-pixel CPU parity");
}
void generatedControls(const std::filesystem::path& loader,
                       const std::shared_ptr<const runtime::GpuSceneOcioContext>& ocio) {
    const auto directory =
        std::filesystem::temp_directory_path() / "bloom_prepared_source_controls";
    std::filesystem::create_directories(directory);
    const auto path = directory / "mutable.exr";
    writeExrRgba(path, 32, 24, std::vector<ExrPixel>(32ULL * 24, {0.2F, 0.4F, 0.1F, 1.0F}));
    const auto oldAsset = imageAsset(path, "Private generated source", 900);
    const auto makePlan = [](const document::AssetRecord& asset, const double x) {
        return mediaPlan(format(128, 96), asset, LayerValues{.position = {x, 48.0}}, 7000);
    };
    Root root;
    FinalRoute final(root, loader, ocio);
    root.sourceContext->assetBaseDirectory = directory;
    const auto plan = makePlan(oldAsset, 64.0);
    auto first = final.evaluate(plan);
    require(first.frame != nullptr,
            "generated final cold native source: " + first.diagnostic.message);
    const auto context = *root.sourceContext;
    const CpuGpuSceneBuilder builder(nullptr, context, *ocio);
    auto request = requestFor(*plan, RationalTime::fromInteger(0), kOracleBudget);
    auto warm = builder.build(plan, request);
    require(warm && warm.scene->mediaStatistics().uploadCacheHits == 1,
            "generated retained source hit");
    const auto held = upload(*warm.scene).image;
    const auto oldKey = upload(*warm.scene).semanticKey;
    warm.scene.reset();
    const auto before = root.pool->preparedStatistics();
    request.bypassOperationCache = true;
    const auto bypass = builder.build(plan, request);
    require(bypass && bypass.scene->mediaStatistics().uploadCacheHits == 0 &&
                root.pool->preparedStatistics() == before,
            "bypass has no prepared reads/writes");
    request.bypassOperationCache = false;
    root.pool->clearOperations();
    const auto operationPurge = builder.build(plan, request);
    require(operationPurge && operationPurge.scene->mediaStatistics().uploadCacheHits == 1,
            "operation purge preserves prepared source");
    root.pool->clearDecodedMedia();
    require(root.prepared->entryCount() == 0 && held->pixels().front().alpha() == 1.0F,
            "media purge removes prepared entries and external source pins stay valid");
    auto readOnlyDefinition = plan->copyDefinition();
    readOnlyDefinition.bypassOperationCache = true;
    const auto readOnly = builder.build(publish(std::move(readOnlyDefinition)), request);
    require(readOnly && readOnly.scene->mediaStatistics().uploadCacheHits == 0 &&
                root.prepared->entryCount() == 0,
            "interactive read-only miss renders without store");
    first.frame.reset();
    const auto valid = final.evaluate(makePlan(oldAsset, 65.0));
    require(valid.frame != nullptr, "final source repopulates after media purge");
    const auto hits = root.pool->preparedStatistics().hits;
    writeExrRgba(path, 32, 24, std::vector<ExrPixel>(32ULL * 24, {0.7F, 0.1F, 0.5F, 1.0F}));
    const auto stale = final.evaluate(makePlan(oldAsset, 66.0));
    std::cout << "STALE_FINAL status=" << static_cast<int>(stale.status)
              << " code=" << static_cast<int>(stale.diagnostic.code)
              << " message=" << stale.diagnostic.message << '\n';
    require(!stale.frame && root.pool->preparedStatistics().hits == hits,
            "actual final stale digest rejected before retained source lookup");
    const auto freshAsset = imageAsset(path, "Fresh private source", 900);
    const auto freshPlan = makePlan(freshAsset, 67.0);
    const auto fresh = final.evaluate(freshPlan);
    CpuCompositionEvaluator oracle;
    oracle.setAssetBaseDirectory(directory);
    auto cpuRequest = requestFor(*freshPlan, RationalTime::fromInteger(0), kOracleBudget);
    cpuRequest.bypassOperationCache = true;
    const auto cpu = oracle.evaluate(freshPlan, cpuRequest, {});
    require(
        fresh.frame && cpu.frame() &&
            pixelsClose(fresh.frame->processImage().pixels(), cpu.frame()->processImage().pixels()),
        "fresh declared digest recovers actual final native parity");
    const auto freshPrepared = builder.build(freshPlan, cpuRequest);
    require(freshPrepared && upload(*freshPrepared.scene).semanticKey != oldKey,
            "changed source content changes actual source semantic key");
    final.close();
    for (const auto cap : {0ULL, 128ULL}) {
        Root capped(cap);
        FinalRoute cappedFinal(capped, loader, ocio);
        capped.sourceContext->assetBaseDirectory = directory;
        const auto output = cappedFinal.evaluate(freshPlan);
        require(output.frame &&
                    pixelsClose(output.frame->processImage().pixels(),
                                cpu.frame()->processImage().pixels()) &&
                    capped.prepared->entryCount() == 0 && capped.pool->preparedPayloadBytes() == 0,
                "explicit small/zero subset still executes uncached with CPU parity");
        std::cout << "CAPPED payloadCap=" << cap << " assigned=" << capped.pool->byteBudget()
                  << " retained=" << capped.pool->retainedBytes() << '\n';
        cappedFinal.close();
    }
}

void proveMembers(const Options& options, Root& root, render::GpuDevice& device,
                  const std::shared_ptr<const runtime::GpuSceneOcioContext>& ocio) {
    FinalRoute final(root, options.loader, ocio);
    CpuCompositionEvaluator cpu;
    auto context = runtime::GpuSceneMediaContext::fromEvaluator(cpu);
    context.operationCache = root.pool;
    context.preparedUploadCache = root.prepared;
    auto cache = runtime::GpuSceneCache::create(device, {kNativeRetention});
    require(cache.hasValue(), "native scene cache");
    auto budgets = runtime::GpuSceneExecutorBudgets{};
    budgets.maxImageBytes = root.requestBytes();
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache, budgets);
    require(executor.hasValue(), "native executor");
    const std::vector<std::size_t> selected{0, options.members.size() / 2,
                                            options.members.size() - 1};
    for (const auto index : selected) {
        const auto& member = options.members[index];
        std::cout << "MEMBER frame=" << member.frame << " phase=probe" << '\n';
        auto asset = imageAsset(member.path, "source-reuse", 900);
        const auto declaredDigest = bloom::core::Sha256Digest::fromLowercaseHex(member.digest);
        if (!declaredDigest.has_value())
            throw std::runtime_error("invalid manifest digest");
        require(asset.width == kWidth && asset.height == kHeight &&
                    asset.contentDigest == declaredDigest.value(),
                "actual manifest geometry/digest");
        context.assetBaseDirectory = member.path.parent_path();
        cpu.setAssetBaseDirectory(member.path.parent_path());
        std::optional<SourceIdentity> source;
        std::optional<bloom::core::Sha256Digest> previousPixels;
        for (int stage = 0; stage < 5; ++stage) {
            const auto plan = editPlan(asset, stage);
            const auto evidence = execute(root, device, *cache.cache, *executor.executor, cpu,
                                          context, *ocio, plan, stage, source);
            if (previousPixels)
                require(evidence.pixels != *previousPixels,
                        "each edit/proxy must actually change output pixels");
            source = evidence.source;
            previousPixels = evidence.pixels;
        }
        if (!source.has_value())
            throw std::runtime_error("missing source evidence");
        proveFinal(final, root, asset, member.path.parent_path(), source.value());
        std::cout << "MEMBER frame=" << member.frame << " PASS" << '\n';
        // Do not require the explicit 2 GiB native pool to hold all 23 large sources/outputs.
        cache.cache->clear();
    }
    final.close();
    executor.executor.reset();
    cache.cache.reset();
    device.reclaimReleasedImages();
}

void proveRetainedSet(const Options& options, Root& root,
                      const std::shared_ptr<const runtime::GpuSceneOcioContext>& ocio) {
    if (options.members.size() != 23) {
        std::cout << "NOTE:23-member retention pass not requested" << '\n';
        return;
    }
    for (std::size_t i = 0; i < options.members.size(); ++i)
        require(options.members[i].frame == 1001 + static_cast<std::int64_t>(i),
                "exact23 contiguous B members");
    const auto count = root.pool->preparedEntryCount();
    const auto charged = root.pool->retainedBytes(runtime::OperationCacheEntryKind::PreparedUpload);
    const auto payload = root.pool->preparedPayloadBytes();
    require(count != 0 && charged >= payload, "measured source metadata must exist");
    const auto metadata = (charged - payload) / count;
    const auto cost = 23 * (kPayload + metadata);
    const auto other = root.pool->retainedBytes() - charged;
    const auto assigned = root.pool->byteBudget();
    std::cout << "SET23 requestedPayload=" << 23 * kPayload
              << " measuredMetadataPerKey=" << metadata << " requiredCombined=" << cost + other
              << " actualAssigned=" << assigned << '\n';
    if (assigned < cost + other) {
        std::cout << "NOTRUN: measured normal allowance insufficient for retained23" << '\n';
        return;
    }
    std::vector<SourceIdentity> sources;
    sources.reserve(23);
    auto context = *root.sourceContext;
    for (const auto& member : options.members) {
        const auto asset = imageAsset(member.path, "Read-only retained B member", 900);
        const auto declaredDigest = bloom::core::Sha256Digest::fromLowercaseHex(member.digest);
        if (!declaredDigest.has_value())
            throw std::runtime_error("invalid retained manifest digest");
        require(asset.contentDigest == declaredDigest.value(), "retained member declared digest");
        context.assetBaseDirectory = member.path.parent_path();
        const CpuGpuSceneBuilder builder(nullptr, context, *ocio, root.phase());
        const auto plan = editPlan(asset, 0);
        auto built = builder.build(
            plan, requestFor(*plan, RationalTime::fromInteger(0), root.requestBytes()));
        require(built.hasValue(),
                "retained23 actual production preparation: " + built.diagnostic.message);
        const auto& source = upload(*built.scene);
        sources.push_back({source.semanticKey, source.image, {}});
        require(root.pool->byteBudget() >= cost + other,
                "actual assignment remained adequate during source set");
        std::cout << "SET_MEMBER frame=" << member.frame
                  << " hit=" << built.scene->mediaStatistics().uploadCacheHits
                  << " payload=" << root.pool->preparedPayloadBytes()
                  << " charged=" << root.pool->retainedBytes()
                  << " sampleSerial=" << root.ledger.state().sampleSerial << '\n';
    }
    require(root.pool->preparedEntryCount() == 23 &&
                root.pool->preparedPayloadBytes() == 23 * kPayload,
            "all23 actual source entries retained without caller pins");
    for (std::size_t i = 0; i < options.members.size(); ++i) {
        const auto& member = options.members[i];
        const auto asset = imageAsset(member.path, "Read-only edited retained B member", 900);
        context.assetBaseDirectory = member.path.parent_path();
        const CpuGpuSceneBuilder builder(nullptr, context, *ocio, root.phase());
        const auto plan = editPlan(asset, 2);
        const auto built = builder.build(
            plan, requestFor(*plan, RationalTime::fromInteger(0), root.requestBytes()));
        require(built && upload(*built.scene).semanticKey == sources[i].key &&
                    upload(*built.scene).image.get() == sources[i].host.lock().get() &&
                    built.scene->mediaStatistics().uploadCacheHits == 1 &&
                    built.scene->mediaStatistics().imageConversions == 0,
                "all23 edited sources retain original allocation/key without redundant conversion");
    }
    std::cout << "SET23 PASS payload=" << root.pool->preparedPayloadBytes() << " metadata="
              << root.pool->retainedBytes(runtime::OperationCacheEntryKind::PreparedUpload) -
                     root.pool->preparedPayloadBytes()
              << " combined=" << root.pool->retainedBytes()
              << " assigned=" << root.pool->byteBudget()
              << " pendingPreparedBytes=unavailable native23Execution=not-run" << '\n';
}

int run(const Options& options) {

    Root root;
    auto created = render::GpuDevice::create({.loader_path = options.loader});
    if (!created) {
        if (options.requireDevice)
            throw std::runtime_error("required native device unavailable");
        std::cout << "SKIP: native device unavailable\n";
        return 77;
    }
    auto resolver = bloom::host::makePackagedGpuOcioResolver(bloom::host::currentExecutablePath());
    require(resolver != nullptr, "packaged OCIO tools");
    const auto resolved = resolver->resolve();
    require(resolved.hasValue(), "packaged OCIO resolution: " + resolved.diagnostic);
    const auto epoch = created.device->ownershipEpoch();
    require(root.policy->reportDevice(epoch, created.device->availableAllocationBudget()),
            "real device allocation delivery");
    const auto sampled = root.ledger.state();
    std::cout << "RETENTION sourcePayload=" << kPayload << " assigned=" << root.pool->byteBudget()
              << " request=" << root.requestBytes() << " nativeControl=" << kNativeRetention
              << " oracleControl=" << kOracleBudget << " processCeiling="
              << (sampled.processCacheCeilingBytes
                      ? std::to_string(*sampled.processCacheCeilingBytes)
                      : "no-finite-assignment")
              << " processSource=" << static_cast<int>(sampled.process.source)
              << " processStatus=" << static_cast<int>(sampled.process.status)
              << " sampleSerial=" << sampled.sampleSerial
              << " sampleFresh=" << sampled.processSampleFresh << '\n';
    generatedControls(options.loader, resolved.context);
    if (!options.members.empty()) {
        proveMembers(options, root, *created.device, resolved.context);
        proveRetainedSet(options, root, resolved.context);
    } else
        std::cout << "NOTE: private B witness absent; generated controls completed" << '\n';
    created.device.reset();
    require(root.policy->retireDeviceReport(epoch, true), "actual native generation retirement");
    const auto native = root.policy->snapshot().reservations;
    require(native.admittedPhases == 0 && native.ownedNativeImages == 0 &&
                native.ownedNativeBuffers == 0,
            "native/phase covered totals zero");
    root.pool->closePreparedUploads();
    root.retirement.owner->beginShutdown();
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!root.retirement.owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    require(root.retirement.owner->exitAcknowledged() && root.retirement.owner->collect() &&
                root.pool->retainedBytes() == 0 && root.pool->preparedEntryCount() == 0,
            "actual HOST worker reference retirement before combined charge zero");
    std::cout << "PASS: actual edited prepared-source HOST/native reuse\n";
    return 0;
}
} // namespace

int main(const int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
