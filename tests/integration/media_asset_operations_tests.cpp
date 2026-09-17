#include "command_test_support.hpp"
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/strict_json_dom.hpp>

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
    return 0;
}
