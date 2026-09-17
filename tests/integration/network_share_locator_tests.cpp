// NETSHARE-1, deliverable 4 (locator robustness): a GVFS network-share mount is just another
// directory outside the project's own tree once ordinary filesystem APIs reach it through the
// FUSE mount (the UI concerns in docs/ux/ui-grammar.md aside, the locator math in
// asset_operations.cpp never learns the difference). This test simulates that shape without a
// real mount: it imports an image from a temporary directory that shares no ancestor with the
// project directory except a common scratch root, then proves the resulting
// bloom::document::AssetLocator round-trips through relink and through a full canonical-document
// encode/decode/reconstruct (save/open) cycle -- the same locator math a real
// `/run/user/<uid>/gvfs/smb-share:...` mount exercises on this machine (verified by hand against
// the real mounts; see the task's progress log).
//
// A self-contained one-pixel PNG writer is used here (rather than reusing another module's own
// test-only PNG fixture helper) so this test stays inside its own module's dependencies. It lives
// under tests/integration/ (mirroring media_asset_operations_tests.cpp) rather than under
// src/commands/tests/, because it deliberately crosses into bloom/project for the encode/decode/
// reconstruct round trip -- a dependency src/commands itself is not allowed to declare.
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/image.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <zlib.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <vector>

namespace {

void appendChunk(std::vector<unsigned char>& out, const char* type,
                 const std::vector<unsigned char>& data) {
    const auto pushU32 = [&out](std::uint32_t value) {
        out.push_back(static_cast<unsigned char>((value >> 24) & 0xFFU));
        out.push_back(static_cast<unsigned char>((value >> 16) & 0xFFU));
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFU));
        out.push_back(static_cast<unsigned char>(value & 0xFFU));
    };
    pushU32(static_cast<std::uint32_t>(data.size()));
    std::vector<unsigned char> crcInput(type, type + 4);
    crcInput.insert(crcInput.end(), data.begin(), data.end());
    out.insert(out.end(), crcInput.begin(), crcInput.begin() + 4);
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc32(0L, crcInput.data(), static_cast<uInt>(crcInput.size()));
    pushU32(static_cast<std::uint32_t>(crc));
}

// A valid, decodable 1x1 RGBA8 PNG -- the same IHDR/raw-scanline shape src/media/image's own
// tests use, reproduced here as raw bytes plus zlib deflate + CRC32 framing.
void writeOnePixelPng(const std::filesystem::path& path) {
    std::vector<unsigned char> bytes{137, 80, 78, 71, 13, 10, 26, 10};
    appendChunk(bytes, "IHDR", {0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0});
    const std::vector<unsigned char> raw{0, 128, 64, 32, 128}; // filter byte + RGBA8
    uLongf boundSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(boundSize);
    if (compress2(compressed.data(), &boundSize, raw.data(), static_cast<uLong>(raw.size()), 6) !=
        Z_OK)
        std::abort();
    compressed.resize(boundSize);
    appendChunk(bytes, "IDAT", compressed);
    appendChunk(bytes, "IEND", {});
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

class TestContext final {
  public:
    void expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok_ = false;
        }
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

  private:
    bool ok_ = true;
};

} // namespace

int main() {
    namespace commands = bloom::commands;
    namespace document = bloom::document;
    namespace media = bloom::media;
    namespace project = bloom::project;

    TestContext context;

    std::random_device randomDevice;
    const auto scratchRoot = std::filesystem::temp_directory_path() /
                             ("bloom-netshare-locator-" + std::to_string(randomDevice()));
    // The project directory and the "mount" directory share only `scratchRoot` as a common
    // ancestor -- exactly the shape a real GVFS mount has relative to a project saved under the
    // artist's home directory. The mount directory's own name matches this machine's real mounts
    // literally (see /run/user/<uid>/gvfs/smb-share:server=10.10.10.20,share=production), colon
    // included: a purely lexical relative path from the project directory can't avoid that colon
    // showing up as one of its own components.
    const auto projectDirectory = scratchRoot / "project";
    const auto mountDirectory =
        scratchRoot / "mount" / "smb-share:server=10.10.10.20,share=production";
    std::filesystem::create_directories(projectDirectory);
    std::filesystem::create_directories(mountDirectory);
    const auto sourcePath = mountDirectory / "network-photo.png";
    writeOnePixelPng(sourcePath);
    const auto relinkPath = mountDirectory / "network-photo-relinked.png";
    writeOnePixelPng(relinkPath);

    // 1. Import from the simulated mount.
    commands::ImportAssets import({sourcePath}, projectDirectory);
    context.expect(import.diagnostic().empty(), "import from simulated mount succeeded");
    context.expect(import.prepared().size() == 1, "import produced exactly one asset");
    if (!context.ok())
        return 1;
    const auto& prepared = import.prepared().front();
    context.expect(prepared.locator.path.find("..") != std::string::npos,
                   "locator path crosses out of the project directory (long ../ chain)");
    context.expect(prepared.locator.relinkHint.starts_with("file://"),
                   "locator carries an absolute file:// relink hint");
    // The GVFS mount directory name carries a literal ':' (smb-share:server=...); the stored
    // "project-relative" path must stay schema-valid (document::AssetRecord::validate() forbids
    // ':' and '\\' anywhere in it) even though the real directory on disk is not renamed.
    context.expect(prepared.locator.path.find(':') == std::string::npos,
                   "the stored project-relative path never carries a raw ':' from the mount name");
    context.expect(prepared.locator.path.find("%3A") != std::string::npos ||
                       prepared.locator.path.find("%3a") != std::string::npos,
                   "the mount's ':' is percent-encoded rather than dropped");
    document::AssetRecord validated = prepared;
    validated.id = document::AssetId::fromRaw(1);
    context.expect(validated.validate().ok(),
                   "the prepared asset record satisfies document::AssetRecord::validate()");

    // The locator must resolve back to the real mounted file: this is exactly what
    // AssetController's thumbnail selection (selectThumbnail() -> media::resolveImagePath()) and
    // Relink both depend on.
    const auto resolvedBeforeSave = media::resolveImagePath(
        prepared.locator.path, prepared.locator.relinkHint, projectDirectory);
    context.expect(std::filesystem::equivalent(resolvedBeforeSave, sourcePath),
                   "locator resolves back to the mounted source file before save");
    const auto probeBeforeSave = media::probeImage(resolvedBeforeSave);
    context.expect(probeBeforeSave.value.has_value() && probeBeforeSave.value->width == 1 &&
                       probeBeforeSave.value->height == 1,
                   "the resolved mounted file decodes (thumbnailing's own first step)");

    auto seed = document::makeNewProject("Network Share", "Main",
                                         bloom::core::RationalTime::fromInteger(24));
    auto projectDoc = std::move(seed.project);
    document::Document doc(std::move(projectDoc));
    auto snapshot = doc.snapshot();
    auto draft = doc.draft(snapshot);
    const auto applied = import.apply(draft);
    context.expect(applied.status == commands::OperationStatus::Applied,
                   "ImportAssets::apply applied a new asset");
    context.expect(applied.outputs.size() == 1, "apply produced one output");
    if (!context.ok())
        return 2;
    const auto* assetIdOutput = std::get_if<document::AssetId>(&applied.outputs.front().id);
    context.expect(assetIdOutput != nullptr, "apply's output names an asset id");
    if (assetIdOutput == nullptr)
        return 2;
    const auto assetId = *assetIdOutput;
    const auto committed = doc.commit(snapshot.revision(), std::move(draft));
    context.expect(committed.committed(), "commit of the imported asset succeeded");
    if (!context.ok())
        return 3;

    // 2. Relink through the same simulated-mount shape, to a second file in the same mount.
    {
        auto relinkSnapshot = doc.snapshot();
        auto relinkDraft = doc.draft(relinkSnapshot);
        commands::RelinkAsset relink(assetId, relinkPath, projectDirectory);
        const auto relinkResult = relink.apply(relinkDraft);
        context.expect(relinkResult.status == commands::OperationStatus::Applied,
                       "relink to a second simulated-mount file applied");
        const auto relinkCommitted = doc.commit(relinkSnapshot.revision(), std::move(relinkDraft));
        context.expect(relinkCommitted.committed(), "commit of the relink succeeded");
    }
    if (!context.ok())
        return 4;
    const auto* relinkedAsset = doc.snapshot().project().findAsset(assetId);
    context.expect(relinkedAsset != nullptr, "relinked asset still present");
    if (relinkedAsset == nullptr)
        return 5;
    const auto resolvedAfterRelink = media::resolveImagePath(
        relinkedAsset->locator.path, relinkedAsset->locator.relinkHint, projectDirectory);
    context.expect(std::filesystem::equivalent(resolvedAfterRelink, relinkPath),
                   "relinked locator resolves to the new mounted file");

    // 3. Round-trip through save/open: encode the canonical document, parse, decode, reconstruct
    // -- exactly the pipeline a real project save/open runs -- and confirm the reconstructed
    // asset's locator still resolves through the same (still-mounted) directory.
    const auto current = doc.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<std::size_t, 64> sorting{};
    const project::CanonicalDocumentV1 request{&current, &settings, {}, sorting};
    const auto size = project::canonicalDocumentSize(request);
    context.expect(static_cast<bool>(size), "canonical document sizing succeeded");
    if (!context.ok())
        return 6;
    std::vector<char> bytes(*size.value());
    context.expect(static_cast<bool>(project::encodeCanonicalDocument(request, bytes)),
                   "canonical document encoding succeeded");
    auto memory = project::ProjectIoMemoryCoordinator::create();
    if (!memory) {
        context.expect(false, "project I/O memory coordinator created");
        return 7;
    }
    auto operation = memory->createOperation();
    if (!operation) {
        context.expect(false, "project I/O operation memory created");
        return 8;
    }
    const auto parsed =
        project::parseStrictJsonDom(std::as_bytes(std::span(bytes)), {}, *operation);
    context.expect(static_cast<bool>(parsed), "reopened document parses");
    if (!context.ok())
        return 9;
    const auto decoded = project::decodeDocumentEnvelope(parsed.document()->root());
    context.expect(static_cast<bool>(decoded), "reopened document decodes");
    if (!context.ok())
        return 10;
    const auto reopened = project::reconstructDocument(*decoded.value());
    context.expect(static_cast<bool>(reopened), "reopened document reconstructs");
    if (!context.ok())
        return 11;
    const auto& reopenedAssets = reopened.value()->document->snapshot().project().assets();
    context.expect(reopenedAssets.size() == 1, "reopened project still has exactly one asset");
    if (!context.ok())
        return 12;
    const auto& reopenedAsset = reopenedAssets.front();
    context.expect(reopenedAsset.locator.path == relinkedAsset->locator.path &&
                       reopenedAsset.locator.relinkHint == relinkedAsset->locator.relinkHint,
                   "the locator survives the save/open round trip byte-for-byte");
    const auto resolvedAfterReopen = media::resolveImagePath(
        reopenedAsset.locator.path, reopenedAsset.locator.relinkHint, projectDirectory);
    context.expect(std::filesystem::equivalent(resolvedAfterReopen, relinkPath),
                   "the reopened locator still resolves through the simulated mount");
    const auto probeAfterReopen = media::probeImage(resolvedAfterReopen);
    context.expect(probeAfterReopen.value.has_value(),
                   "the reopened, relinked, resolved mounted file still decodes (thumbnails)");

    std::error_code cleanupError;
    std::filesystem::remove_all(scratchRoot, cleanupError);

    return context.ok() ? 0 : 1;
}
