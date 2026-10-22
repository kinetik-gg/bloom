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
#include <string>

namespace {
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
    {
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
        if (!output)
            return false;
    }
    std::size_t probeChecks = 0;
    const bloom::commands::ImportAssets stoppedProbe({path}, path.parent_path(),
                                                     [&] { return ++probeChecks >= 2; });
    if (!stoppedProbe.prepared().empty() ||
        stoppedProbe.diagnostic() != "Audio import failed: Audio operation was cancelled")
        return false;

    std::size_t digestChecks = 0;
    const bloom::commands::ImportAssets stoppedDigest({path}, path.parent_path(),
                                                      [&] { return ++digestChecks > 100; });
    if (digestChecks <= 100 || !stoppedDigest.prepared().empty() ||
        stoppedDigest.diagnostic() != "Audio import failed: Audio operation was cancelled")
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
    return 0;
}
