#pragma once
// A hand-assembled preserved-read-only .bloom archive (a trimmed duplicate of the fixture in
// main_window_readonly_placeholder_tests.cpp, which tests cannot share across targets): opening it
// leaves the project read-only. Used by mcp_live_ui_binding_tests.cpp for the ReadOnly refusal.
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui::test::readonly_fixture {


// ---------------------------------------------------------------------------------------------
// Archive fixture plumbing (trimmed duplicate of src/host/tests/session_open_tests.cpp -- see the
// file comment above).
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U;

[[nodiscard]] inline bloom::project::ProjectIoOperationMemory makeOperation() {
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(kGenerousOperationBudget);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation =
        coordinator->createOperation(kGenerousOperationBudget, kGenerousOperationBudget);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] inline std::array<std::uint8_t, 32> ascendingDigestBytes() noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

[[nodiscard]] inline bloom::document::ColorSettings neutralColorSettings() {
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));
}

[[nodiscard]] inline std::uint32_t crc32Of(const std::span<const std::byte> payload) noexcept {
    const auto value = crc32_z(0L, reinterpret_cast<const Bytef*>(payload.data()), payload.size());
    return static_cast<std::uint32_t>(value);
}

struct EntrySpec final {
    std::string localName;
    std::string centralName;
    std::uint16_t localFlags = 0x0800;
    std::uint16_t centralFlags = 0x0800;
    std::uint16_t localMethod = 0;
    std::uint16_t centralMethod = 0;
    std::vector<std::byte> data;
    std::uint32_t localCompressedSize = 0;
    std::uint32_t centralCompressedSize = 0;
    std::uint32_t localUncompressedSize = 0;
    std::uint32_t centralUncompressedSize = 0;
    std::uint32_t localCrc = 0;
    std::uint32_t centralCrc = 0;
    std::vector<std::byte> localExtra;
    std::vector<std::byte> centralExtra;
    std::string centralComment;
    std::uint16_t versionMadeBy = 0x0314;
    std::uint32_t externalAttrs = 0100644U << 16U;
    std::uint16_t diskNumberStart = 0;
};

[[nodiscard]] inline EntrySpec makeStoredEntry(const std::string& name,
                                        const std::span<const std::byte> payload) {
    EntrySpec spec;
    spec.localName = name;
    spec.centralName = name;
    spec.data.assign(payload.begin(), payload.end());
    spec.localCompressedSize = spec.centralCompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localUncompressedSize = spec.centralUncompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localCrc = spec.centralCrc = crc32Of(payload);
    return spec;
}

class ArchiveWriter final {
  public:
    std::uint64_t appendLocal(const EntrySpec& entry) {
        const auto offset = bytes_.size();
        appendU32(0x04034b50U);
        appendU16(20);
        appendU16(entry.localFlags);
        appendU16(entry.localMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.localCrc);
        appendU32(entry.localCompressedSize);
        appendU32(entry.localUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.localName.size()));
        appendU16(static_cast<std::uint16_t>(entry.localExtra.size()));
        appendText(entry.localName);
        appendRaw(entry.localExtra);
        appendRaw(entry.data);
        return offset;
    }

    void appendCentral(const EntrySpec& entry, const std::uint32_t localHeaderOffset) {
        appendU32(0x02014b50U);
        appendU16(entry.versionMadeBy);
        appendU16(20);
        appendU16(entry.centralFlags);
        appendU16(entry.centralMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.centralCrc);
        appendU32(entry.centralCompressedSize);
        appendU32(entry.centralUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.centralName.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralExtra.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralComment.size()));
        appendU16(entry.diskNumberStart);
        appendU16(0);
        appendU32(entry.externalAttrs);
        appendU32(localHeaderOffset);
        appendText(entry.centralName);
        appendRaw(entry.centralExtra);
        appendText(entry.centralComment);
    }

    void appendEocd(const std::uint16_t diskNumber, const std::uint16_t diskWithCd,
                    const std::uint16_t entriesThisDisk, const std::uint16_t entriesTotal,
                    const std::uint32_t cdSize, const std::uint32_t cdOffset) {
        appendU32(0x06054b50U);
        appendU16(diskNumber);
        appendU16(diskWithCd);
        appendU16(entriesThisDisk);
        appendU16(entriesTotal);
        appendU32(cdSize);
        appendU32(cdOffset);
        appendU16(0);
    }

    [[nodiscard]] std::uint64_t size() const { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

  private:
    void appendU16(const std::uint16_t value) {
        bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
        bytes_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }
    void appendU32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }
    void appendText(const std::string_view text) {
        for (const char character : text) {
            bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
    }
    void appendRaw(const std::span<const std::byte> raw) {
        bytes_.insert(bytes_.end(), raw.begin(), raw.end());
    }

    std::vector<std::byte> bytes_;
};

[[nodiscard]] inline std::vector<std::byte> buildConformingArchive(const EntrySpec& manifest,
                                                            const EntrySpec& document) {
    ArchiveWriter writer;
    const auto manifestOffset = writer.appendLocal(manifest);
    const auto documentOffset = writer.appendLocal(document);
    const auto centralStart = writer.size();
    writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
    writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
    const auto centralSize = writer.size() - centralStart;
    writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                      static_cast<std::uint32_t>(centralStart));
    return writer.bytes();
}

[[nodiscard]] inline std::vector<std::byte> minimalCanonicalDocumentBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    const bloom::project::CanonicalDocumentV1 request{.snapshot = &snapshot,
                                                      .colorSettings = &colorSettings,
                                                      .payloadScratch = payloadScratch,
                                                      .sortScratch = sortScratch};
    const auto documentSize = bloom::project::canonicalDocumentSize(request);
    if (!documentSize) {
        std::abort();
    }
    std::vector<char> documentText(*documentSize.value());
    if (!bloom::project::encodeCanonicalDocument(request, documentText)) {
        std::abort();
    }
    std::vector<std::byte> documentBytes(documentText.size());
    std::memcpy(documentBytes.data(), documentText.data(), documentText.size());
    return documentBytes;
}

// A {1,1}-container archive (mirrors session_open_tests.cpp's
// testManifestSidePreservedReadOnly/buildManifestSidePreservedReadOnlyArchiveOrAbort exactly): a
// manifest declaring containerVersion {1,1} over an otherwise valid {1,0} document classifies
// PreservedReadOnlyRequired on the manifest side.
[[nodiscard]] inline std::vector<std::byte> buildManifestSidePreservedReadOnlyArchiveOrAbort() {
    const std::string manifestText =
        R"({"format":"org.kinetik.bloom.project","containerVersion":{"major":1,"minor":1},)"
        R"("document":{"path":"document.json","schemaVersion":{"major":1,"minor":0}},)"
        R"("requirements":[]})";
    std::vector<std::byte> manifestBytes(manifestText.size());
    std::memcpy(manifestBytes.data(), manifestText.data(), manifestText.size());
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto documentEntry = makeStoredEntry("document.json", documentBytes);
    return buildConformingArchive(manifestEntry, documentEntry);
}

// A minimal, valid, unverified two-entry archive that classifies Opened -- built through
// bloom::project's own real archive writer (no hand-rolled ZIP entries needed, since this one
// declares the container version this build actually supports).
[[nodiscard]] inline std::vector<std::byte> buildEditableArchiveBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Editable Reopened Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const bloom::project::CanonicalManifestV1 manifest{
        .documentSchemaVersion = bloom::project::kCanonicalManifestDocumentSchemaVersionV1,
        .requirements = {}};
    const bloom::project::CanonicalDocumentV1 documentInput{.snapshot = &snapshot,
                                                            .colorSettings = &colorSettings};
    auto built = bloom::project::buildVerifiedSaveArchive(
        manifest, documentInput, bloom::project::SaveArchiveLimits{}, makeOperation());
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

inline void writeFileOrAbort(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        std::abort();
    }
}

// ---------------------------------------------------------------------------------------------

} // namespace bloom::ui::test::readonly_fixture
