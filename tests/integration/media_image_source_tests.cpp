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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

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
} // namespace

int main() {
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
