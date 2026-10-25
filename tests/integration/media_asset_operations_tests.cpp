#include "command_test_support.hpp"
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
// A minimal PCM16 mono WAV with `dataBytes` of silence.
bool writeWav(const std::filesystem::path& path, const std::uint32_t dataBytes) {
    std::ofstream output(path, std::ios::binary);
    const auto u16 = [&](std::uint16_t value) {
        const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8U)};
        output.write(bytes, sizeof(bytes));
    };
    const auto u32 = [&](std::uint32_t value) {
        const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8U),
                              static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
        output.write(bytes, sizeof(bytes));
    };
    output.write("RIFF", 4);
    u32(36U + dataBytes);
    output.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(48'000);
    u32(48'000U * 2U);
    u16(2);
    u16(16);
    output.write("data", 4);
    u32(dataBytes);
    output.seekp(44 + dataBytes - 1);
    output.put('\0');
    return static_cast<bool>(output);
}

// One bad file never aborts a batch: valid files import in one undoable transaction and every
// failure is reported with its own path and diagnostic.
bool mixedBatchImport() {
    namespace doc = bloom::document;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("bloom-mixed-import-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct RemoveDirectory final {
        std::filesystem::path path;
        ~RemoveDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto first = directory / "first.wav", second = directory / "second.wav";
    const auto corrupt = directory / "corrupt.png", missing = directory / "missing.wav";
    if (!writeWav(first, 4800) || !writeWav(second, 9600))
        return false;
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "not an image";
    }
    const std::vector<std::filesystem::path> paths{first, corrupt, missing, second};
    auto import = std::make_unique<bloom::commands::ImportAssets>(paths, directory);
    const auto& failures = import->failures();
    if (!import->diagnostic().empty() || import->prepared().size() != 2 || failures.size() != 2 ||
        failures[0].path != corrupt || failures[1].path != missing ||
        failures[0].diagnostic.empty() || failures[1].diagnostic.empty())
        return false;

    auto seed = doc::makeNewProject("Import", "Main", bloom::core::RationalTime::fromInteger(2));
    doc::Document document(std::move(seed.project));
    bloom::commands::CommandStack stack(document);
    bloom::commands::Transaction transaction("Import Assets", document.snapshot().revision());
    if (!transaction.add(std::move(import)))
        return false;
    const auto executed = stack.execute(std::move(transaction));
    if (executed.status != bloom::commands::CommandStatus::Succeeded ||
        document.snapshot().project().assets().size() != 2)
        return false;
    const auto undone = stack.undo();
    if (undone.status != bloom::commands::CommandStatus::Succeeded ||
        !document.snapshot().project().assets().empty())
        return false;

    // Only invalid files: the batch is refused and names each failure.
    const bloom::commands::ImportAssets refused({corrupt, missing}, directory);
    if (refused.diagnostic().empty() || !refused.prepared().empty() ||
        refused.failures().size() != 2 ||
        refused.diagnostic().find("2 files could not be imported") == std::string::npos)
        return false;
    // Cancellation keeps what finished (#377): stopping while the second item is prepared keeps
    // the first, abandons the rest without reporting them as faults, and still applies.
    bool stop = false;
    std::vector<bloom::commands::ImportProgress> reports;
    const bloom::commands::ImportAssets cancelled(
        paths, directory, [&] { return stop; },
        [&](const bloom::commands::ImportProgress& state) {
            reports.push_back(state);
            stop = stop || (state.completed >= 1 && !state.current.empty());
        });
    if (!cancelled.cancelled() || cancelled.prepared().size() != 1 ||
        !cancelled.failures().empty() || cancelled.notImported() != 3 ||
        !cancelled.diagnostic().empty() || reports.size() < 2 || reports.front().total != 4 ||
        reports.front().current != first)
        return false;
    // Cancelling before anything was prepared refuses the batch.
    const bloom::commands::ImportAssets refusedCancel(paths, directory, [] { return true; });
    return refusedCancel.cancelled() && refusedCancel.prepared().empty() &&
           refusedCancel.notImported() == 4 && refusedCancel.diagnostic() == "Import cancelled";
}

// A chosen folder contributes the media files below it, sorted and without hidden entries; a
// folder with no media is reported as one skipped item with a plain reason.
bool folderImport() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("bloom-folder-import-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct RemoveDirectory final {
        std::filesystem::path path;
        ~RemoveDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto footage = directory / "Footage";
    const auto empty = directory / "Notes";
    std::filesystem::create_directories(footage / "Sub");
    std::filesystem::create_directories(footage / ".cache");
    std::filesystem::create_directories(empty);
    if (!writeWav(footage / "b.wav", 4800) || !writeWav(footage / "Sub" / "a.wav", 4800) ||
        !writeWav(footage / ".hidden.wav", 4800) || !writeWav(footage / ".cache" / "c.wav", 4800))
        return false;
    {
        std::ofstream notes(footage / "readme.txt");
        notes << "not media";
        std::ofstream other(empty / "readme.txt");
        other << "not media";
    }
    std::size_t total = 0;
    const bloom::commands::ImportAssets imported(
        {footage, empty, footage / "b.wav"}, directory, {},
        [&](const bloom::commands::ImportProgress& state) { total = state.total; });
    if (!imported.diagnostic().empty() || imported.prepared().size() != 2 || total != 2 ||
        imported.failures().size() != 1 || imported.failures().front().path != empty ||
        imported.failures().front().diagnostic !=
            "The folder holds no media files Bloom can import")
        return false;
    // Sorted: Footage/Sub/a.wav before Footage/b.wav (directory order is not portable).
    const auto& first = imported.prepared().front().locator.path;
    return first.ends_with("Sub/a.wav") && bloom::commands::isImportableMediaFile("X.EXR") &&
           !bloom::commands::isImportableMediaFile("readme.txt");
}

bool audioImportCancellationAndRecovery() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-audio-import-cancel-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct RemoveFile final {
        std::filesystem::path path;
        ~RemoveFile() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};
    constexpr std::uint32_t dataBytes = 8U * 1024U * 1024U;
    if (!writeWav(path, dataBytes))
        return false;
    std::size_t probeChecks = 0;
    const bloom::commands::ImportAssets stoppedProbe({path}, path.parent_path(),
                                                     [&] { return ++probeChecks >= 2; });
    if (!stoppedProbe.prepared().empty() || !stoppedProbe.cancelled() ||
        !stoppedProbe.failures().empty() || stoppedProbe.diagnostic() != "Import cancelled")
        return false;

    std::size_t digestChecks = 0;
    const bloom::commands::ImportAssets stoppedDigest({path}, path.parent_path(),
                                                      [&] { return ++digestChecks > 100; });
    if (digestChecks <= 100 || !stoppedDigest.prepared().empty() || !stoppedDigest.cancelled() ||
        stoppedDigest.diagnostic() != "Import cancelled")
        return false;

    const bloom::commands::ImportAssets recovered({path}, path.parent_path());
    return recovered.diagnostic().empty() && recovered.prepared().size() == 1 &&
           recovered.prepared().front().frames == dataBytes / 2U;
}
} // namespace

int main() {
    namespace doc = bloom::document;
    auto seed = doc::makeNewProject("Images", "Main", bloom::core::RationalTime::fromInteger(24));
    const auto compositionId = seed.initialCompositionId;
    auto project = std::move(seed.project);
    const bloom::core::Color4d background{0.2, 0.4, 0.6, 1.0};
    project.findComposition(compositionId)->setBackgroundColor(background);
    doc::AssetRecord asset;
    asset.id = doc::AssetId::fromRaw(4);
    asset.locator = {"file", "project-relative", "images/test.png", "file:///test.png"};
    asset.name = "test";
    asset.width = 2;
    asset.height = 3;
    if (!project.addAsset(asset))
        return 1;
    doc::Document document(std::move(project));
    auto snapshot = document.snapshot();
    if (snapshot.ids().highWater().asset != 4)
        return 2;
    auto draft = document.draft(snapshot);
    bloom::commands::RemoveAsset remove(asset.id);
    if (remove.apply(draft).status != bloom::commands::OperationStatus::Applied)
        return 3;
    const auto committed = document.commit(snapshot.revision(), std::move(draft));
    if (!committed.committed() || !document.snapshot().project().assets().empty())
        return 4;
    const auto restored = document.restore(document.snapshot().revision(), snapshot);
    if (!restored.committed() || document.snapshot().project().assets().size() != 1)
        return 5;
    if (document.snapshot().ids().highWater().asset != 4)
        return 6;
    const auto current = document.snapshot();
    const auto settings = doc::makeBloomNeutralColorSettingsV1({});
    std::array<std::size_t, 64> sorting{};
    const bloom::project::CanonicalDocumentV1 request{&current, &settings, {}, sorting};
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size)
        return 8;
    std::vector<char> bytes(*size.value());
    if (!bloom::project::encodeCanonicalDocument(request, bytes))
        return 9;
    auto memory = bloom::project::ProjectIoMemoryCoordinator::create();
    if (!memory)
        return 10;
    auto operation = memory->createOperation();
    if (!operation)
        return 11;
    const auto parsed =
        bloom::project::parseStrictJsonDom(std::as_bytes(std::span(bytes)), {}, *operation);
    if (!parsed)
        return 12;
    const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    if (!decoded)
        return 13;
    const auto reopened = bloom::project::reconstructDocument(*decoded.value());
    if (!reopened || reopened.value()->document->snapshot().project().assets().size() != 1 ||
        reopened.value()->document->snapshot().project().assets()[0] != asset)
        return 14;
    if (reopened.value()
            ->document->snapshot()
            .project()
            .findComposition(compositionId)
            ->backgroundColor() != background)
        return 15;
    asset.manifest.members.push_back({});
    if (asset.validate().ok())
        return 7;
    if (!audioImportCancellationAndRecovery())
        return 16;
    if (!mixedBatchImport())
        return 17;
    if (!folderImport())
        return 18;
    return 0;
}
