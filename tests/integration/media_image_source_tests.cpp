#include "generated_jpeg.hpp"
#include "image_source.hpp"
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <algorithm>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

namespace {
void writeExr(const std::filesystem::path& path) {
    Imf::Header header(1, 1);
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    const std::array<float, 1> red{0.25F};
    const std::array<float, 1> green{0.125F};
    const std::array<float, 1> blue{0.0625F};
    const std::array<float, 1> alpha{0.5F};
    Imf::FrameBuffer buffer;
    const auto insert = [&](const char* name, const std::array<float, 1>& samples) {
        buffer.insert(name, Imf::Slice::Make(Imf::FLOAT, samples.data(), header.dataWindow(),
                                             sizeof(float), sizeof(float)));
    };
    insert("R", red);
    insert("G", green);
    insert("B", blue);
    insert("A", alpha);
    Imf::OutputFile output(path.string().c_str(), header, 1);
    output.setFrameBuffer(buffer);
    output.writePixels(1);
}

// A real, decodable still large enough that an avoided decode is measurable. Only the CACHE-1
// benchmark uses it; the default integration run does not generate or time it.
void writeExrSized(const std::filesystem::path& path, const int width, const int height) {
    Imf::Header header(width, height);
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> red(pixels), green(pixels), blue(pixels), alpha(pixels, 1.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            red[index] = static_cast<float>(x) / static_cast<float>(width);
            green[index] = static_cast<float>(y) / static_cast<float>(height);
            blue[index] = 0.5F;
        }
    }
    Imf::FrameBuffer buffer;
    const auto insert = [&](const char* name, std::vector<float>& samples) {
        buffer.insert(name, Imf::Slice::Make(Imf::FLOAT, samples.data(), header.dataWindow(),
                                             sizeof(float),
                                             static_cast<std::size_t>(width) * sizeof(float)));
    };
    insert("R", red);
    insert("G", green);
    insert("B", blue);
    insert("A", alpha);
    Imf::OutputFile output(path.string().c_str(), header, 1);
    output.setFrameBuffer(buffer);
    output.writePixels(height);
}

struct CacheOneBenchmarkSample final {
    double medianMilliseconds = 0.0;
    std::uint64_t nativeCacheHits = 0;
    std::uint64_t derivedCacheHits = 0;
    std::uint64_t derivedCacheMisses = 0;
};

// Bounded, repeatable timing for CACHE-1. Compares a warm native decoded still-image read against
// the explicit evaluation bypass over the SAME two interactive override plans, pixels, resolution
// and source. Fixture generation, import, compilation and the native warm-up are excluded from both
// modes; each sample times ONLY the evaluator.evaluate() call (content probe/hash, cache lookup and
// evaluation) -- no compilation, display preparation or presentation is timed, and nothing times a
// cache lookup in isolation.
[[nodiscard]] int runCacheOneBenchmark() {
    namespace doc = bloom::document;
    namespace runtime = bloom::runtime;
    namespace commands = bloom::commands;
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kSamples = 7;
    // Unique per benchmark process: a fixed shared directory could be deleted by a concurrent run
    // while this one still reads its fixture.
    std::random_device random;
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bloom-cache1-benchmark-" + std::to_string(random()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directories(folder) && !std::filesystem::exists(folder))
        return 1;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    writeExrSized(folder / "bench.0001.exr", kWidth, kHeight);

    const auto duration = bloom::core::RationalTime::create(1, 1);
    const auto format = doc::CompositionFormat::create(kWidth, kHeight);
    if (!duration || !format)
        return 1;
    auto seed = doc::makeNewProject("CACHE-1 benchmark", "Main", *duration, *format);
    const auto composition = seed.initialCompositionId;
    doc::Document document(std::move(seed.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    commands::ImportAssets import({folder / "bench.0001.exr"}, folder);
    if (import.apply(draft).status != commands::OperationStatus::Applied)
        return 2;
    const auto asset = draft.project().assets().front().id;
    if (commands::AddImageLayer(composition, asset).apply(draft).status !=
        commands::OperationStatus::Applied)
        return 3;
    if (!document.commit(snapshot.revision(), std::move(draft)).committed())
        return 4;
    runtime::SnapshotCompiler compiler(doc::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), composition}, {});
    if (!compiled.plan || compiled.plan->format().width() != kWidth ||
        compiled.plan->format().height() != kHeight)
        return 5;

    // Real compiler override path (the same seam the UI and the parity test rely on): compilation
    // stays outside both timed modes.
    const runtime::CompiledLayerOutput* baseLayer = nullptr;
    for (const auto& operation : compiled.plan->operations())
        if (const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation))
            baseLayer = layer;
    if (baseLayer == nullptr)
        return 6;
    const auto compileOverride = [&](const doc::Vec2d position) {
        return compiler
            .compile({document.snapshot(),
                      composition,
                      {runtime::SnapshotParameterOverride{document.snapshot().revision(),
                                                          baseLayer->position.id, position}}},
                     {})
            .plan;
    };
    const auto overrideA = compileOverride(doc::Vec2d{200.0, 120.0});
    const auto overrideB = compileOverride(doc::Vec2d{1080.0, 600.0});
    if (!overrideA || !overrideB || !overrideA->bypassOperationCache() ||
        !overrideB->bypassOperationCache())
        return 7;
    const auto requestFor = [](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                               const bool explicitBypass) {
        runtime::EvaluationRequest request{.time = {},
                                           .output = plan->output(),
                                           .resolution = {},
                                           .pixelStorageByteLimit = std::size_t{256} * 1024 * 1024};
        request.bypassOperationCache = explicitBypass;
        return request;
    };

    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(folder);
    // Warm the native decoded entry and the OS page cache once, outside both timed modes.
    if (!evaluator.evaluate(compiled.plan, requestFor(compiled.plan, false), {}).frame())
        return 8;
    // Parity gate before timing: for BOTH transforms, the warm cache-read result and the explicit
    // evaluation-bypass result must agree on pixels and bounds, or the run fails instead of timing
    // two different pictures.
    const auto verifyParity =
        [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan) {
            const auto cached = evaluator.evaluate(plan, requestFor(plan, false), {});
            const auto uncached = evaluator.evaluate(plan, requestFor(plan, true), {});
            return cached.frame() && uncached.frame() &&
                   std::ranges::equal(cached.frame()->processImage().pixels(),
                                      uncached.frame()->processImage().pixels()) &&
                   std::ranges::equal(cached.frame()->evaluatedBounds(),
                                      uncached.frame()->evaluatedBounds());
        };
    if (!verifyParity(overrideA) || !verifyParity(overrideB))
        return 9;

    const auto measure = [&](const bool explicitBypass) {
        CacheOneBenchmarkSample sample;
        std::vector<double> milliseconds;
        milliseconds.reserve(kSamples);
        const auto cacheHitsBefore = evaluator.operationCache()->statistics().hits;
        for (int index = 0; index < kSamples; ++index) {
            const auto& plan = index % 2 == 0 ? overrideA : overrideB;
            runtime::OperationCacheStatistics derived;
            const auto start = std::chrono::steady_clock::now();
            const auto result = evaluator.evaluate(plan, requestFor(plan, explicitBypass), {}, {},
                                                   nullptr, &derived);
            const auto end = std::chrono::steady_clock::now();
            if (!result.frame())
                return std::optional<CacheOneBenchmarkSample>{};
            milliseconds.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            sample.derivedCacheHits += derived.hits;
            sample.derivedCacheMisses += derived.misses;
        }
        sample.nativeCacheHits = evaluator.operationCache()->statistics().hits - cacheHitsBefore;
        std::ranges::sort(milliseconds);
        sample.medianMilliseconds = milliseconds[milliseconds.size() / 2];
        return std::optional<CacheOneBenchmarkSample>{sample};
    };

    const auto warmNative = measure(false);
    const auto uncached = measure(true);
    if (!warmNative || !uncached)
        return 10;
    std::cout << "CACHE-1 benchmark source=" << kWidth << 'x' << kHeight
              << " output=" << compiled.plan->format().width() << 'x'
              << compiled.plan->format().height() << " interactive-override samples=" << kSamples
              << " warm_native_median_ms=" << warmNative->medianMilliseconds
              << " warm_native_read_hits=" << warmNative->nativeCacheHits
              << " uncached_median_ms=" << uncached->medianMilliseconds
              << " uncached_read_hits=" << uncached->nativeCacheHits
              << " derived_hits=" << warmNative->derivedCacheHits + uncached->derivedCacheHits
              << " derived_evaluations="
              << warmNative->derivedCacheMisses + uncached->derivedCacheMisses << '\n';
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--cache1-benchmark")
        return runCacheOneBenchmark();
    namespace doc = bloom::document;
    namespace runtime = bloom::runtime;
    namespace commands = bloom::commands;
    const auto folder = std::filesystem::temp_directory_path() / "bloom-image-source-test";
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto write = [&](const std::string& name) {
        std::ofstream file(folder / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()),
                   static_cast<std::streamsize>(kGeneratedJpeg.size()));
    };
    write("frame.0001.jpg");
    write("frame.0003.jpg");
    writeExr(folder / "exr.0001.exr");
    writeExr(folder / "exr.0003.exr");
    const auto duration = bloom::core::RationalTime::create(1, 1);
    const auto atFour = bloom::core::RationalTime::create(4, 24);
    const auto atTwo = bloom::core::RationalTime::create(2, 24);
    const auto rate = doc::FrameRate::create(24, 1);
    if (!duration || !atFour || !atTwo || !rate)
        return 10;
    auto seed = doc::makeNewProject("Image test", "Main", *duration);
    const auto composition = seed.initialCompositionId;
    doc::Document document(std::move(seed.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    commands::ImportAssets import({folder / "frame.0001.jpg"}, folder);
    if (import.apply(draft).status != commands::OperationStatus::Applied) {
        std::cerr << import.diagnostic();
        return 1;
    }
    const auto asset = draft.project().assets().front().id;
    if (commands::AddImageLayer(composition, asset).apply(draft).status !=
        commands::OperationStatus::Applied)
        return 2;
    if (!document.commit(snapshot.revision(), std::move(draft)).committed())
        return 3;
    runtime::SnapshotCompiler compiler(doc::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), composition}, {});
    if (!compiled.plan) {
        for (const auto& d : compiled.diagnostics)
            std::cerr << d.summary << '\n';
        return 4;
    }
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(folder);
    const runtime::EvaluationRequest request{.time = {},
                                             .output = compiled.plan->output(),
                                             .resolution = {},
                                             .pixelStorageByteLimit =
                                                 std::size_t{128} * 1024 * 1024};
    const auto result = evaluator.evaluate(compiled.plan, request, {});
    if (!result.frame() || result.diagnostics().empty())
        return 5;
    const auto* source =
        std::get_if<runtime::CompiledImageSource>(&compiled.plan->operations().front());
    if (!source)
        return 6;
    auto loop = *source;
    loop.loopMode = 1;
    const auto selected = runtime::detail::selectImageSource(loop, *atFour, *rate, folder, {});
    if (!selected.available || selected.path.filename() != "frame.0001.jpg")
        return 7;
    loop.loopMode = 2;
    const auto ping = runtime::detail::selectImageSource(loop, *atTwo, *rate, folder, {});
    if (!ping.available || ping.path.filename() != "frame.0003.jpg")
        return 8;

    // CACHE-1: a warmed native decoded still-image entry is reusable as a READ while a plan with an
    // interactive transform override bypasses derived operation memoization. The image was already
    // decoded and stored by the first evaluate() above, so its DecodedMedia entry is warm.
    const auto baseRequest =
        [](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan) {
            return runtime::EvaluationRequest{.time = {},
                                              .output = plan->output(),
                                              .resolution = {},
                                              .pixelStorageByteLimit =
                                                  std::size_t{128} * 1024 * 1024};
        };
    const auto findImageSource = [](runtime::CompiledCompositionPlanDefinition& definition)
        -> runtime::CompiledImageSource* {
        for (auto& operation : definition.operations)
            if (auto* source = std::get_if<runtime::CompiledImageSource>(&operation))
                return source;
        return nullptr;
    };
    // Two distinct interactive transforms over the SAME source. The plans come from the real
    // compiler override path, so this proves the seam the UI now relies on: a request carrying
    // parameterOverrides compiles to a plan whose own bypassOperationCache() is set
    // (snapshot_compiler_lowering.ipp), which the evaluator maps to read-only native source access
    // while still bypassing derived operations.
    const runtime::CompiledLayerOutput* baseLayer = nullptr;
    for (const auto& operation : compiled.plan->operations())
        if (const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation))
            baseLayer = layer;
    if (baseLayer == nullptr)
        return 24;
    const auto compileOverride = [&](const doc::Vec2d position) {
        return compiler
            .compile({document.snapshot(),
                      composition,
                      {runtime::SnapshotParameterOverride{document.snapshot().revision(),
                                                          baseLayer->position.id, position}}},
                     {})
            .plan;
    };
    const auto overrideA = compileOverride(doc::Vec2d{8.0, 6.0});
    const auto overrideB = compileOverride(doc::Vec2d{40.0, 30.0});
    if (!overrideA || !overrideB || !overrideA->bypassOperationCache() ||
        !overrideB->bypassOperationCache())
        return 24;
    const auto cachedBytesBefore =
        evaluator.operationCache()->retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia);
    const auto cacheStatsBefore = evaluator.operationCache()->statistics();
    runtime::OperationCacheStatistics overrideStatsA, overrideStatsB;
    const auto overrideResultA =
        evaluator.evaluate(overrideA, baseRequest(overrideA), {}, {}, nullptr, &overrideStatsA);
    const auto overrideResultB =
        evaluator.evaluate(overrideB, baseRequest(overrideB), {}, {}, nullptr, &overrideStatsB);
    if (!overrideResultA.frame() || !overrideResultB.frame())
        return 25;
    const auto cacheStatsAfter = evaluator.operationCache()->statistics();
    if (overrideStatsA.hits != 0 || overrideStatsB.hits != 0 ||
        overrideStatsA.misses != overrideA->operations().size() ||
        overrideStatsB.misses != overrideB->operations().size())
        return 26;
    if (cacheStatsAfter.hits != cacheStatsBefore.hits + 2 ||
        cacheStatsAfter.misses != cacheStatsBefore.misses)
        return 27;
    if (evaluator.operationCache()->retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) !=
        cachedBytesBefore)
        return 28;

    // The interactive result must match the explicit uncached oracle bit-for-bit, geometry
    // included.
    auto uncachedRequest = baseRequest(overrideA);
    uncachedRequest.bypassOperationCache = true;
    const auto oracle = evaluator.evaluate(overrideA, uncachedRequest, {});
    if (!oracle.frame() ||
        !std::ranges::equal(overrideResultA.frame()->processImage().pixels(),
                            oracle.frame()->processImage().pixels()) ||
        !std::ranges::equal(overrideResultA.frame()->evaluatedBounds(),
                            oracle.frame()->evaluatedBounds()))
        return 29;
    // Explicit request bypass stays a fully uncached control for the still-image path: it must not
    // read the warmed native entry either. This is the seam that separates the oracle flag from the
    // plan's interactive-derived bypass.
    const auto oracleHitsBefore = evaluator.operationCache()->statistics().hits;
    const auto oracleAgain = evaluator.evaluate(overrideA, uncachedRequest, {});
    if (!oracleAgain.frame() || evaluator.operationCache()->statistics().hits != oracleHitsBefore)
        return 30;

    // A changed source interpretation and a changed alpha input must both miss the warmed entry,
    // must not insert one, and must still match their own explicit uncached evaluation.
    const auto changedSourceCheck = [&](runtime::CompiledCompositionPlanDefinition& definition,
                                        const int expectedReturn) -> int {
        definition.bypassOperationCache = true;
        const auto plan =
            std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
        const auto hitsBefore = evaluator.operationCache()->statistics().hits;
        const auto missesBefore = evaluator.operationCache()->statistics().misses;
        const auto bytesBefore = evaluator.operationCache()->retainedBytes(
            runtime::OperationCacheEntryKind::DecodedMedia);
        const auto changed = evaluator.evaluate(plan, baseRequest(plan), {});
        if (!changed.frame())
            return expectedReturn;
        if (evaluator.operationCache()->statistics().hits != hitsBefore ||
            evaluator.operationCache()->statistics().misses != missesBefore + 1 ||
            evaluator.operationCache()->retainedBytes(
                runtime::OperationCacheEntryKind::DecodedMedia) != bytesBefore)
            return expectedReturn + 100;
        auto referenceRequest = baseRequest(plan);
        referenceRequest.bypassOperationCache = true;
        const auto reference = evaluator.evaluate(plan, referenceRequest, {});
        if (!reference.frame() || !std::ranges::equal(changed.frame()->processImage().pixels(),
                                                      reference.frame()->processImage().pixels()))
            return expectedReturn + 200;
        return 0;
    };
    auto colorSpaceDefinition = compiled.plan->copyDefinition();
    if (auto* imageSource = findImageSource(colorSpaceDefinition))
        imageSource->colorSpace = 1;
    if (const auto failure = changedSourceCheck(colorSpaceDefinition, 40); failure != 0)
        return failure;
    auto alphaDefinition = compiled.plan->copyDefinition();
    if (auto* imageSource = findImageSource(alphaDefinition))
        imageSource->premultiply = !imageSource->premultiply;
    if (const auto failure = changedSourceCheck(alphaDefinition, 45); failure != 0)
        return failure;

    // A zero-budget evaluator still renders (read-only miss decodes, store is refused) and retains
    // nothing.
    runtime::CpuCompositionEvaluator tinyEvaluator;
    tinyEvaluator.setAssetBaseDirectory(folder);
    tinyEvaluator.operationCache()->setByteBudget(0);
    const auto tiny = tinyEvaluator.evaluate(overrideA, baseRequest(overrideA), {});
    if (!tiny.frame() ||
        !std::ranges::equal(tiny.frame()->processImage().pixels(),
                            oracle.frame()->processImage().pixels()) ||
        tinyEvaluator.operationCache()->retainedBytes() != 0)
        return 50;

    // The original committed revision's derived entries survive the gesture evaluations intact.
    const auto committedAgain = evaluator.evaluate(compiled.plan, request, {});
    if (!committedAgain.frame() ||
        committedAgain.frame()->operationCacheStatistics().hits !=
            compiled.plan->operations().size() ||
        committedAgain.frame()->operationCacheStatistics().misses != 0 ||
        !std::ranges::equal(committedAgain.frame()->processImage().pixels(),
                            result.frame()->processImage().pixels()))
        return 51;

    // CACHE-2 (docs/architecture/media-io.md "Disk cache"): two INDEPENDENT evaluator instances
    // sharing one disk cache -- a fresh evaluator's own in-process memory cache is cold, so a
    // decode served to it without touching `frame.0001.jpg` again proves the disk path rather than
    // the memory one. Deliberately not reusing `evaluator` above: its memory cache already holds
    // this frame from the very first evaluate() call, which would hide the disk cache entirely.
    namespace cache = bloom::media::cache;
    cache::MediaDiskCacheConfig diskCacheConfig;
    diskCacheConfig.rootDirectory = folder / "disk-cache";
    auto diskCache = std::make_shared<cache::MediaDiskCache>(diskCacheConfig);
    runtime::CpuCompositionEvaluator diskWriter;
    diskWriter.setAssetBaseDirectory(folder);
    diskWriter.setMediaDiskCache(diskCache);
    const auto written = diskWriter.evaluate(compiled.plan, request, {});
    if (!written.frame() || written.diagnostics().empty())
        return 11;
    diskCache->flush();
    if (diskCache->statistics().entryCount == 0)
        return 12;
    runtime::CpuCompositionEvaluator diskReader;
    diskReader.setAssetBaseDirectory(folder);
    diskReader.setMediaDiskCache(diskCache);
    const auto read = diskReader.evaluate(compiled.plan, request, {});
    if (!read.frame() || read.diagnostics().empty())
        return 13;
    if (diskCache->statistics().hits == 0)
        return 14;

    // A parent's cached source must include its child's resolved media dependencies. Removing
    // an external file does not change the project revision or either compiled plan.
    auto parentDefinition = compiled.plan->copyDefinition();
    parentDefinition.compositionId = doc::CompositionId::fromRaw(composition.value() + 100);
    parentDefinition.nestedPlans = {compiled.plan};
    parentDefinition.operations = {runtime::CompiledCompositionSource{
        doc::NodeId::fromRaw(10001),
        0,
        {{doc::ParameterId::fromRaw(10002), 0.0}, {doc::ParameterId::fromRaw(10003), 1.0}, 0}}};
    parentDefinition.operations.push_back(runtime::CompiledMerge{
        doc::NodeId::fromRaw(10004),
        {{doc::LayerSlotId::fromRaw(10006), {}, runtime::OperationIndex::fromRaw(0)}}});
    parentDefinition.operations.push_back(runtime::CompiledCompositionOutput{
        doc::NodeId::fromRaw(10005), runtime::OperationIndex::fromRaw(1)});
    parentDefinition.output = runtime::OperationIndex::fromRaw(2);
    auto parent =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(parentDefinition));
    auto parentRequest = request;
    parentRequest.output = parent->output();
    const auto nestedBefore = evaluator.evaluate(parent, parentRequest, {});
    if (!nestedBefore.frame()) {
        for (const auto& diagnostic : nestedBefore.diagnostics())
            std::cerr << diagnostic.summary << '\n';
        return 15;
    }
    std::filesystem::remove(folder / "frame.0001.jpg");
    const auto missing = evaluator.evaluate(compiled.plan, request, {});
    if (!missing.frame() || missing.diagnostics().empty())
        return 9;
    const auto nestedAfter = evaluator.evaluate(parent, parentRequest, {});
    if (!nestedAfter.frame() || nestedAfter.diagnostics().empty())
        return 16;
    const auto beforePixels = nestedBefore.frame()->processImage().pixels();
    const auto afterPixels = nestedAfter.frame()->processImage().pixels();
    if (std::ranges::equal(beforePixels, afterPixels))
        return 17;

    // The same Layer -> image source -> CPU composite path also accepts the in-process EXR
    // backend. This is deliberately evaluated at a non-default scrub time to cover the sequence
    // member selection and the layer composite together.
    auto exrSeed = doc::makeNewProject("EXR image test", "Main", *duration);
    const auto exrComposition = exrSeed.initialCompositionId;
    doc::Document exrDocument(std::move(exrSeed.project));
    auto exrSnapshot = exrDocument.snapshot();
    auto exrDraft = exrDocument.draft(exrSnapshot);
    commands::ImportAssets exrImport({folder / "exr.0001.exr"}, folder);
    if (exrImport.apply(exrDraft).status != commands::OperationStatus::Applied)
        return 18;
    const auto exrAsset = exrDraft.project().assets().front().id;
    if (commands::AddImageLayer(exrComposition, exrAsset).apply(exrDraft).status !=
        commands::OperationStatus::Applied)
        return 19;
    if (!exrDocument.commit(exrSnapshot.revision(), std::move(exrDraft)).committed())
        return 20;
    const auto exrCompiled = compiler.compile({exrDocument.snapshot(), exrComposition}, {});
    if (!exrCompiled.plan)
        return 21;
    runtime::CpuCompositionEvaluator exrEvaluator;
    exrEvaluator.setAssetBaseDirectory(folder);
    const auto scrubTime = bloom::core::RationalTime::create(1, 2);
    if (!scrubTime)
        return 22;
    const auto exrResult =
        exrEvaluator.evaluate(exrCompiled.plan,
                              {.time = *scrubTime,
                               .output = exrCompiled.plan->output(),
                               .resolution = {},
                               .pixelStorageByteLimit = std::size_t{128} * 1024 * 1024},
                              {});
    if (!exrResult.frame() || exrResult.frame()->processImage().pixels().empty())
        return 23;
    return 0;
}
