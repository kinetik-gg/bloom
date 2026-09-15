#include "../../media/image/tests/generated_jpeg.hpp"
#include "../image_source.hpp"
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

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
    std::filesystem::remove(folder / "frame.0001.jpg");
    const auto missing = evaluator.evaluate(compiled.plan, request, {});
    if (!missing.frame() || missing.diagnostics().empty())
        return 9;
    return 0;
}
