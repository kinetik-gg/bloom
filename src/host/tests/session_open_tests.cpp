#include <bloom/host/session_open.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/bloom_neutral_profile.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Drives openSessionArchive() -- the synchronous session-level Open orchestration composing
// ProjectSession::admitOpenIntent(), project::openProjectArchive(), and
// ProjectSession::installDecodedReplacement()/installPreservedReadOnlyReplacement() -- following
// session_save_tests.cpp's pattern one composition layer up. installDecodedReplacement()/
// installPreservedReadOnlyReplacement() are ALSO exercised directly (they are public
// ProjectSession members) for the gate-order scenarios (stale/mismatched/edit-during-open
// captures) that require an OpenIntentCapture openSessionArchive() itself would never hand back,
// since that pipeline always admits its own fresh intent.

namespace {

using bloom::commands::SetProjectName;
using bloom::commands::Transaction;

using bloom::host::ArtifactTargetKey;
using bloom::host::DecodedProjectEditability;
using bloom::host::DecodedProjectSessionRequest;
using bloom::host::DecodedReplacementContent;
using bloom::host::DecodedReplacementReservations;
using bloom::host::OpenIntentCapture;
using bloom::host::openSessionArchive;
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionContentKind;
using bloom::host::ProjectSessionIdentitySource;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;
using bloom::host::saveProjectSession;
using bloom::host::SessionInstallStatus;
using bloom::host::SessionOpenInstallOutcome;
using bloom::host::SessionOpenResult;
using bloom::host::SessionOpenStage;
using bloom::host::SessionPathIntentAbandonStatus;
using bloom::host::SessionPathIntentKind;
using bloom::host::SessionSaveInputStatus;
using bloom::host::SessionSaveRequest;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::ArtifactTargetObservation;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;

using bloom::project::buildVerifiedSaveArchive;
using bloom::project::canonicalDocumentSize;
using bloom::project::CanonicalDocumentV1;
using bloom::project::canonicalManifestSize;
using bloom::project::CanonicalManifestV1;
using bloom::project::decodeDocumentEnvelope;
using bloom::project::DocumentClassification;
using bloom::project::DocumentDecodeOutcome;
using bloom::project::DocumentDecodeResult;
using bloom::project::encodeCanonicalDocument;
using bloom::project::encodeCanonicalManifest;
using bloom::project::ManifestRequirement;
using bloom::project::OpenArchiveOutcome;
using bloom::project::OpenArchivePreservedReadOnlySide;
using bloom::project::openProjectArchive;
using bloom::project::parseStrictJsonDom;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::RoundTripAttachmentPath;
using bloom::project::RoundTripState;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveStage;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-session-open-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------------------------
// Shared plumbing (mirrors session_save_tests.cpp / open_archive_tests.cpp)
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U;

[[nodiscard]] ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] ProjectIoOperationMemory makeOperation() {
    return makeOperation(kGenerousOperationBudget);
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::span<const char> bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::array<std::uint8_t, 32> ascendingDigestBytes() noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::optional<PublicationCoordinator>
makePublicationCoordinator(Expectations& expectations,
                           const PublicationCoordinatorConfig config = {}) {
    auto result = PublicationCoordinator::create(config);
    expectations.expect(result.has_value(), "the publication coordinator is created");
    return result;
}

[[nodiscard]] std::optional<StagedArtifactCoordinator>
makeArtifactsCoordinator(Expectations& expectations, const StagedArtifactConfig config = {}) {
    auto result = StagedArtifactCoordinator::create(config);
    expectations.expect(result.succeeded(), "the staged-artifact coordinator is created");
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeCoordinator();
}

[[nodiscard]] bloom::document::Revision currentRevision(const ProjectSession& session) {
    const auto state = session.stateSnapshot();
    if (!state.currentRevision.has_value()) {
        throw std::logic_error("Expected a decoded revision");
    }
    return *state.currentRevision;
}

[[nodiscard]] Transaction rename(std::string value, const ProjectSession& session,
                                 std::string label = "Rename") {
    Transaction transaction(std::move(label), currentRevision(session));
    transaction.emplace<SetProjectName>(std::move(value));
    return transaction;
}

[[nodiscard]] std::string projectName(const ProjectSession& session) {
    const auto snapshot = session.decodedSnapshot();
    if (!snapshot) {
        throw std::logic_error("Expected a decoded document");
    }
    return std::string(snapshot.snapshot().project().name());
}

[[nodiscard]] ProjectSession
makeDecodedSession(Expectations& expectations, ProjectSessionIdentitySource& identitySource,
                   const std::string& projectName,
                   const std::optional<std::filesystem::path>& displayPath = std::nullopt) {
    auto created = bloom::document::makeNewProject(projectName, "Main Composition",
                                                   bloom::core::RationalTime::fromInteger(10));
    std::optional<ProjectDisplayPath> path;
    if (displayPath.has_value()) {
        path = ProjectDisplayPath::create(*displayPath);
        expectations.expect(path.has_value(), "the display-path fixture is valid");
    }
    auto result = ProjectSession::createDecoded(identitySource,
                                                {.project = std::move(created.project),
                                                 .colorSettings = neutralColorSettings(),
                                                 .editability = DecodedProjectEditability::Editable,
                                                 .displayPath = std::move(path),
                                                 .persistedAllocatorHighWater = std::nullopt});
    expectations.expect(static_cast<bool>(result), "the decoded session fixture must be valid");
    if (!result) {
        throw std::logic_error("Could not create decoded project-session fixture");
    }
    return std::move(result).takeSession();
}

// A minimal, valid, unverified two-entry archive that always classifies Opened: used everywhere
// a test needs to reach installDecodedReplacement()'s gates without caring about the document's
// own content.
[[nodiscard]] std::vector<std::byte>
buildMinimalArchiveBytesOrAbort(const std::string& projectName = "Untitled Project") {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject = bloom::document::makeNewProject(projectName, "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

// Builds the same DecodedReplacementContent openSessionArchive() itself builds from a real
// OpenedArchive, exposing it directly so gate-order tests can call
// ProjectSession::installDecodedReplacement() with a caller-chosen (possibly stale)
// OpenIntentCapture -- openSessionArchive() always admits its own fresh intent, so it cannot
// exercise a capture taken earlier against a session that has since moved on.
[[nodiscard]] std::optional<DecodedReplacementContent>
takeDecodedContent(Expectations& expectations, const std::span<const std::byte> archiveBytes,
                   const std::string_view context) {
    auto opened = openProjectArchive(archiveBytes, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Opened, context);
    if (opened.outcome() != OpenArchiveOutcome::Opened) {
        return std::nullopt;
    }
    auto value = std::move(opened).takeOpened();
    DecodedReplacementReservations reservations(std::move(value.manifestReservation),
                                                std::move(value.decodeReservation),
                                                std::move(value.reconstructionReservation));
    return DecodedReplacementContent(
        std::move(value.document), std::move(value.colorSettings), std::move(value.roundTrip),
        value.schemaMinor, std::move(value.requirements), DecodedProjectEditability::Editable,
        std::nullopt, std::move(reservations));
}

[[nodiscard]] std::vector<std::byte> minimalCanonicalDocumentBytesOrAbort() {
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
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &colorSettings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch};
    const auto documentSize = canonicalDocumentSize(request);
    if (!documentSize) {
        std::abort();
    }
    std::vector<char> documentText(*documentSize.value());
    if (!encodeCanonicalDocument(request, documentText)) {
        std::abort();
    }
    std::vector<std::byte> documentBytes(documentText.size());
    std::memcpy(documentBytes.data(), documentText.data(), documentText.size());
    return documentBytes;
}

// A trimmed, self-contained duplicate of src/project/tests/zip_container_test_support.hpp's
// hand-rolled Constrained ZIP Profile writer (only the conforming-archive path this file needs --
// no deflate, no deliberate deviations). src modules may not reach across a sibling module's
// tests/ directory (architecture_boundaries), and open_archive_tests.cpp's own comment already
// establishes per-test-file fixture duplication as this codebase's precedent (e.g.
// neutralColorSettings() above).
[[nodiscard]] std::uint32_t crc32Of(const std::span<const std::byte> payload) noexcept {
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

[[nodiscard]] EntrySpec makeStoredEntry(const std::string& name,
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

[[nodiscard]] std::vector<std::byte> buildConformingArchive(const EntrySpec& manifest,
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

// A {1,1}-container archive (O1's fixture approach -- mirrors open_archive_tests.cpp's
// testManifestSidePreservedReadOnly exactly): a manifest declaring containerVersion {1,1} over an
// otherwise valid {1,0} document classifies PreservedReadOnlyRequired on the manifest side.
[[nodiscard]] std::vector<std::byte> buildManifestSidePreservedReadOnlyArchiveOrAbort() {
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

[[nodiscard]] const SessionInstallStatus* installedStatus(const SessionOpenResult& result) {
    const auto* outcome = result.installOutcome();
    return outcome != nullptr ? std::get_if<SessionInstallStatus>(outcome) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Full cycle: build session A -> edit -> save -> read published bytes -> openSessionArchive into
// the SAME session -> Installed; fresh generations, clean at new baseline, not dirty, displayPath
// set, undo history empty. Then edit + save again -> reopen -> content identical both hops.
// ---------------------------------------------------------------------------------------------

void testFullCycleSessionFileSession(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "full cycle: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    auto session = makeDecodedSession(expectations, identitySource, "Full Cycle Project");
    expectations.expect(session.execute(rename("First Hop", session)).changed(),
                        "full cycle: the first edit commits");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "full cycle: Save As advances path authority");
    if (!saveAs) {
        return;
    }
    const SessionSaveRequest firstSaveRequest{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto firstSaved =
        saveProjectSession(session, *coordinator, *artifacts, firstSaveRequest, makeOperation());
    expectations.expect(static_cast<bool>(firstSaved), "full cycle: the first save fully succeeds");
    if (!firstSaved) {
        return;
    }

    const auto firstPublishedBytes = readFile(targetPath);
    expectations.expect(!firstPublishedBytes.empty(), "full cycle: the published file has bytes");

    const auto beforeReopen = session.stateSnapshot();
    const auto reopenPath = ProjectDisplayPath::create(targetPath);
    expectations.expect(reopenPath.has_value(), "full cycle: the reopen display path constructs");
    if (!reopenPath.has_value()) {
        return;
    }

    auto reopened = openSessionArchive(session, asBytes(firstPublishedBytes), reopenPath,
                                       SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reopened) &&
                            reopened.stage() == SessionOpenStage::Installation,
                        "full cycle: openSessionArchive installs the just-published bytes into "
                        "the SAME session");
    const auto* status = installedStatus(reopened);
    expectations.expect(status != nullptr && *status == SessionInstallStatus::Installed,
                        "full cycle: the install outcome names Installed verbatim");
    expectations.expect(reopened.attemptedContentKind() ==
                            ProjectSessionContentKind::DecodedDocument,
                        "full cycle: the attempted content kind names DecodedDocument");

    const auto afterReopen = session.stateSnapshot();
    // openIntentGeneration_ advances via admitOpenIntent() -- called unconditionally as
    // openSessionArchive()'s first step -- so it is compared separately from the other
    // generations below rather than folded into the "content untouched otherwise" checks.
    expectations.expect(afterReopen.openIntentGeneration.value() ==
                            beforeReopen.openIntentGeneration.value() + 1,
                        "full cycle: admission advances OpenIntentGeneration exactly once");
    expectations.expect(afterReopen.resultAcceptanceGeneration !=
                                beforeReopen.resultAcceptanceGeneration &&
                            afterReopen.pathIntentGeneration != beforeReopen.pathIntentGeneration,
                        "full cycle: install advances both SessionResultAcceptanceGeneration and "
                        "SessionPathIntentGeneration (fresh generations)");
    expectations.expect(afterReopen.pathIntentKind == SessionPathIntentKind::ExistingPath &&
                            !afterReopen.newestAcceptedPublicationIntent.isValid(),
                        "full cycle: path authority resets to ExistingPath and the publication "
                        "frontier clears");
    expectations.expect(afterReopen.cleanRevision.has_value() &&
                            afterReopen.currentRevision == afterReopen.cleanRevision &&
                            afterReopen.dirty == false,
                        "full cycle: clean at the fresh stack's baseline revision, not dirty");
    expectations.expect(afterReopen.historySize == 0 && !afterReopen.canUndo &&
                            !afterReopen.canRedo,
                        "full cycle: undo history is empty");
    expectations.expect(afterReopen.displayPath == reopenPath, "full cycle: displayPath is set");
    expectations.expect(projectName(session) == "First Hop",
                        "full cycle: the reopened content matches what was actually published "
                        "(first hop)");

    // Edit + save again -> reopen -> content identical both hops.
    expectations.expect(session.execute(rename("Second Hop", session)).changed(),
                        "full cycle: the second edit commits");
    const auto plainIntent = session.capturePlainSavePathIntent();
    expectations.expect(plainIntent.isValid() &&
                            plainIntent.kind() == SessionPathIntentKind::ExistingPath,
                        "full cycle: a plain-save intent captures existing-path authority after "
                        "reopen");
    std::optional<ArtifactTargetObservation> observedFingerprint;
    {
        auto peek =
            artifacts->preflight({.targetPath = targetPath,
                                  .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
                                  .expectedTarget = std::nullopt});
        expectations.expect(peek && peek.target()->observation().exists,
                            "full cycle: preflight observes the published fingerprint");
        if (!peek) {
            return;
        }
        observedFingerprint = peek.target()->observation();
    }
    const SessionSaveRequest secondSaveRequest{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
        .expectedTarget = observedFingerprint,
        .limits = {},
        .intent = plainIntent,
    };
    auto secondSaved =
        saveProjectSession(session, *coordinator, *artifacts, secondSaveRequest, makeOperation());
    expectations.expect(static_cast<bool>(secondSaved),
                        "full cycle: the second save fully succeeds");
    if (!secondSaved) {
        return;
    }
    const auto secondPublishedBytes = readFile(targetPath);

    auto secondReopened = openSessionArchive(session, asBytes(secondPublishedBytes), reopenPath,
                                             SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(secondReopened),
                        "full cycle: the second reopen also installs");
    expectations.expect(projectName(session) == "Second Hop",
                        "full cycle: content is identical both hops -- the second reopen reflects "
                        "exactly what was published the second time");
}

// ---------------------------------------------------------------------------------------------
// New-project color default full cycle (issue #60, the gap it exists to close): createNew() ->
// edit -> Save As -> saveProjectSession() -> reopen the just-published bytes via
// openSessionArchive() -> Installed. The decoded colorSettings must equal the Bloom Neutral v1
// value exactly, proving the locator URI and expected-revision digest survive the full
// save/decode/reconstruct/install cycle, not just in-memory construction.
// ---------------------------------------------------------------------------------------------

void testNewProjectFullCycleInstallsBloomNeutralColor(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "new-project color cycle: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "new-project-color.bloom";
    const auto expectedColorSettings =
        bloom::document::makeBloomNeutralColorSettingsV1(bloom::host::kBloomNeutralV1ConfigDigest);
    expectations.expect(expectedColorSettings.validate().ok(),
                        "new-project color cycle: the expected Bloom Neutral value itself "
                        "passes validate()");

    auto createResult = ProjectSession::createNew(
        identitySource, {.projectName = "New Project Color",
                         .compositionName = "Main",
                         .duration = bloom::core::RationalTime::fromInteger(10),
                         .format = {}});
    expectations.expect(static_cast<bool>(createResult),
                        "new-project color cycle: createNew() succeeds");
    if (!createResult) {
        return;
    }
    auto session = std::move(createResult).takeSession();
    // createNew() installs the Bloom Neutral v1 color settings unconditionally -- never
    // ColorSettingsUnavailable -- and captureSaveInput() carries it from the moment the session
    // exists; see bloom.host.project-session's testColorSettingsGatingAndRoundTripValidation for
    // that pre-save assertion. This test's own job is proving the value survives the full
    // save/decode/reconstruct/install cycle below, not re-proving the in-memory default.

    expectations.expect(session.execute(rename("Renamed New Project", session)).changed(),
                        "new-project color cycle: the edit commits");

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs),
                        "new-project color cycle: Save As advances path authority");
    if (!saveAs) {
        return;
    }
    const SessionSaveRequest saveRequest{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto saved =
        saveProjectSession(session, *coordinator, *artifacts, saveRequest, makeOperation());
    expectations.expect(static_cast<bool>(saved), "new-project color cycle: the save succeeds");
    if (!saved) {
        return;
    }

    const auto publishedBytes = readFile(targetPath);
    expectations.expect(!publishedBytes.empty(),
                        "new-project color cycle: the published file has bytes");

    const auto reopenPath = ProjectDisplayPath::create(targetPath);
    expectations.expect(reopenPath.has_value(),
                        "new-project color cycle: the reopen display path constructs");
    if (!reopenPath.has_value()) {
        return;
    }

    auto reopened = openSessionArchive(session, asBytes(publishedBytes), reopenPath,
                                       SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reopened) &&
                            reopened.stage() == SessionOpenStage::Installation,
                        "new-project color cycle: openSessionArchive installs the just-published "
                        "bytes into the SAME session");
    const auto* status = installedStatus(reopened);
    expectations.expect(status != nullptr && *status == SessionInstallStatus::Installed,
                        "new-project color cycle: the install outcome names Installed verbatim");

    expectations.expect(projectName(session) == "Renamed New Project",
                        "new-project color cycle: the reopened content matches what was "
                        "actually published");

    const auto plainIntent = session.capturePlainSavePathIntent();
    const auto reopenedCaptured = session.captureSaveInput(plainIntent);
    expectations.expect(
        reopenedCaptured.status() == SessionSaveInputStatus::Captured &&
            static_cast<bool>(reopenedCaptured) && reopenedCaptured.value() != nullptr,
        "new-project color cycle: captureSaveInput succeeds on the reopened session");
    if (reopenedCaptured.value() != nullptr) {
        expectations.expect(
            reopenedCaptured.value()->colorSettings() == expectedColorSettings,
            "new-project color cycle: the reopened colorSettings equal "
            "makeBloomNeutralColorSettingsV1(kBloomNeutralV1ConfigDigest) exactly -- locator URI "
            "and expected digest survived the full save/decode/reconstruct/install cycle");
        expectations.expect(reopenedCaptured.value()->colorSettings().validate().ok(),
                            "new-project color cycle: the reopened colorSettings pass validate()");
    }
}

// ---------------------------------------------------------------------------------------------
// Round-tripped newer minor survives session->file->session->file: a spliced {1,1} archive with
// one unknown root member (built directly, mirroring open_archive_tests.cpp's
// testOpenRoundTrippedNewerMinorRoundTrip fixture) is opened into a session, saved, reopened
// again through the SAME pipeline, and saved once more -- the two published files must be
// byte-identical and both must still carry the unknown member.
// ---------------------------------------------------------------------------------------------

void testRoundTrippedNewerMinorSurvivesFullCycle(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "round-tripped cycle: fixture is available");
        return;
    }
    const auto firstTarget = directory.path() / "first.bloom";
    const auto secondTarget = directory.path() / "second.bloom";

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "round-tripped cycle: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document sourceDocument{std::move(newProject.project)};
    auto sourceSnapshot = sourceDocument.snapshot();
    const auto colorSettings = neutralColorSettings();

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    const CanonicalDocumentV1 baselineRequest{.snapshot = &sourceSnapshot,
                                              .colorSettings = &colorSettings,
                                              .payloadScratch = payloadScratch,
                                              .sortScratch = sortScratch};
    const auto size = canonicalDocumentSize(baselineRequest);
    expectations.expect(static_cast<bool>(size), "round-tripped cycle: baseline document sizes");
    if (!size) {
        return;
    }
    std::string text(*size.value(), '\0');
    const auto written =
        encodeCanonicalDocument(baselineRequest, std::span<char>(text.data(), text.size()));
    expectations.expect(static_cast<bool>(written),
                        "round-tripped cycle: baseline document encodes");
    if (!written) {
        return;
    }
    const std::string anchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto anchorPos = text.find(anchor);
    expectations.expect(anchorPos != std::string::npos,
                        "round-tripped cycle: root schemaVersion anchor is located");
    if (anchorPos == std::string::npos) {
        return;
    }
    text.replace(anchorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");
    expectations.expect(text.size() >= 2 && text.back() == '\n' && text[text.size() - 2] == '}',
                        "round-tripped cycle: baseline ends with the root's closing brace");
    if (text.size() < 2 || text.back() != '\n' || text[text.size() - 2] != '}') {
        return;
    }
    text.resize(text.size() - 2);
    text += R"(,"zzzFutureField":42})";
    text += '\n';

    auto parsed = parseStrictJsonDom(asBytes(text), {}, makeOperation());
    expectations.expect(static_cast<bool>(parsed), "round-tripped cycle: spliced document parses");
    if (!parsed) {
        return;
    }
    DocumentDecodeResult decoded = decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            decoded.value() != nullptr && decoded.roundTrip() != nullptr,
                        "round-tripped cycle: spliced document decodes with round-trip state");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        return;
    }
    auto reconstructed = bloom::project::reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed),
                        "round-tripped cycle: spliced document reconstructs");
    if (!reconstructed) {
        return;
    }
    auto reconstructedSnapshot = reconstructed.value()->document->snapshot();
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 1}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &reconstructedSnapshot,
                                            .colorSettings = &reconstructed.value()->colorSettings,
                                            .roundTrip = decoded.roundTrip(),
                                            .schemaMinor = 1};
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built), "round-tripped cycle: fixture archive builds");
    if (!built) {
        return;
    }
    const auto fixtureBytes = built.archive()->bytes();

    // "session" (via Open) -> "file" hop 1.
    ProjectSessionIdentitySource sessionIdentitySource;
    auto session = makeDecodedSession(expectations, sessionIdentitySource, "Round-Trip Host");
    const auto firstPath = ProjectDisplayPath::create(firstTarget);
    expectations.expect(firstPath.has_value(), "round-tripped cycle: first target path constructs");
    if (!firstPath.has_value()) {
        return;
    }
    auto openedFirst =
        openSessionArchive(session, fixtureBytes, firstPath, SaveArchiveLimits{}, makeOperation());
    expectations.expect(
        static_cast<bool>(openedFirst),
        "round-tripped cycle: openSessionArchive installs the round-tripped fixture");
    if (!openedFirst) {
        return;
    }
    const SessionSaveRequest firstSaveRequest{
        .targetPath = firstTarget,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = session.capturePlainSavePathIntent(),
    };
    auto firstSaved =
        saveProjectSession(session, *coordinator, *artifacts, firstSaveRequest, makeOperation());
    expectations.expect(static_cast<bool>(firstSaved),
                        "round-tripped cycle: the first save (session->file hop 1) fully succeeds");
    if (!firstSaved) {
        return;
    }
    const auto firstFileBytes = readFile(firstTarget);

    // "file" -> "session" hop 2 (reopen through the SAME pipeline under test).
    auto openedSecond = openSessionArchive(session, asBytes(firstFileBytes), firstPath,
                                           SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(openedSecond),
                        "round-tripped cycle: the second openSessionArchive reinstalls the "
                        "just-published round-tripped bytes");

    // "session" -> "file" hop 2.
    const SessionSaveRequest secondSaveRequest{
        .targetPath = secondTarget,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = session.capturePlainSavePathIntent(),
    };
    auto secondSaved =
        saveProjectSession(session, *coordinator, *artifacts, secondSaveRequest, makeOperation());
    expectations.expect(
        static_cast<bool>(secondSaved),
        "round-tripped cycle: the second save (session->file hop 2) fully succeeds");
    if (!secondSaved) {
        return;
    }
    const auto secondFileBytes = readFile(secondTarget);

    expectations.expect(firstFileBytes == secondFileBytes,
                        "round-tripped cycle: session->file->session->file reproduces "
                        "byte-identical files");

    // The published archive is a ZIP container, not raw JSON -- reopen it through
    // openProjectArchive() itself to inspect the round-tripped member the same way
    // open_archive_tests.cpp does.
    auto reopenedArchive =
        openProjectArchive(asBytes(secondFileBytes), SaveArchiveLimits{}, makeOperation());
    expectations.expect(reopenedArchive.outcome() == OpenArchiveOutcome::Opened,
                        "round-tripped cycle: the final published file reopens as Opened");
    if (reopenedArchive.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto finalValue = std::move(reopenedArchive).takeOpened();
    expectations.expect(
        finalValue.schemaMinor == 1,
        "round-tripped cycle: the final published file still declares schemaMinor 1");
    expectations.expect(
        finalValue.roundTrip.has_value(),
        "round-tripped cycle: the final published file still carries RoundTripState");
    if (!finalValue.roundTrip.has_value()) {
        return;
    }
    const auto* members = finalValue.roundTrip->find(RoundTripAttachmentPath{});
    expectations.expect(members != nullptr && members->size() == 1 &&
                            (*members)[0].key() == "zzzFutureField",
                        "round-tripped cycle: the unknown root member survives session->file->"
                        "session->file exactly");
}

// ---------------------------------------------------------------------------------------------
// Edit-during-open refusal: admit intent, execute a transaction (and separately: an undo), then
// attempt install -> RevisionChanged; session state (revision, dirty, history) untouched.
// ---------------------------------------------------------------------------------------------

void testEditDuringOpenRefusal(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;

    // A successful edit after admission.
    {
        auto session = makeDecodedSession(expectations, identitySource, "Edit During Open");
        const auto admission = session.admitOpenIntent();
        expectations.expect(static_cast<bool>(admission), "edit during open: admission succeeds");
        if (!admission) {
            return;
        }
        const auto intent = admission.capture();
        expectations.expect(session.execute(rename("Edited While Open", session)).changed(),
                            "edit during open: the edit commits while Open is pending");

        auto content = takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort(),
                                          "edit during open: replacement content builds");
        if (!content.has_value()) {
            return;
        }
        const auto before = session.stateSnapshot();
        const auto status = session.installDecodedReplacement(intent, std::move(*content));
        expectations.expect(
            status == SessionInstallStatus::RevisionChanged,
            "edit during open: a successful edit refuses install as RevisionChanged");
        const auto after = session.stateSnapshot();
        expectations.expect(
            after.currentRevision == before.currentRevision &&
                after.cleanRevision == before.cleanRevision && after.dirty == before.dirty &&
                after.historySize == before.historySize && after.canUndo == before.canUndo &&
                after.canRedo == before.canRedo && after.displayPath == before.displayPath &&
                after.resultAcceptanceGeneration == before.resultAcceptanceGeneration,
            "edit during open: revision, dirty state, and history are untouched by "
            "the refused install");
    }

    // Undo after admission (a newer revision even though content reverts).
    {
        auto session = makeDecodedSession(expectations, identitySource, "Undo During Open");
        expectations.expect(session.execute(rename("Baseline Edit", session)).changed(),
                            "undo during open: the baseline edit commits");
        const auto admission = session.admitOpenIntent();
        expectations.expect(static_cast<bool>(admission), "undo during open: admission succeeds");
        if (!admission) {
            return;
        }
        const auto intent = admission.capture();
        const auto undoResult = session.undo();
        expectations.expect(undoResult.changed(), "undo during open: the undo commits");

        auto content = takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort(),
                                          "undo during open: replacement content builds");
        if (!content.has_value()) {
            return;
        }
        const auto before = session.stateSnapshot();
        const auto status = session.installDecodedReplacement(intent, std::move(*content));
        expectations.expect(status == SessionInstallStatus::RevisionChanged,
                            "undo during open: undo (a newer monotonic revision) also refuses "
                            "install as RevisionChanged");
        const auto after = session.stateSnapshot();
        expectations.expect(after.currentRevision == before.currentRevision &&
                                after.dirty == before.dirty && after.canRedo == before.canRedo,
                            "undo during open: session state is untouched by the refused install");
    }
}

// ---------------------------------------------------------------------------------------------
// Stale intent: admit twice, install with the first capture -> StaleOpenIntent.
// Acceptance mismatch: install a capture taken before a successful prior install ->
// AcceptanceMismatch. Documented gate order (see checkInstallAcceptanceGates() in
// project_session.cpp): the three acceptance gates are orthogonal (generation+contentKind,
// resultAcceptance, decoded revision), so this scenario is deterministically AcceptanceMismatch,
// not StaleOpenIntent, under this implementation's gate order -- pinned here rather than left
// ambiguous.
// ---------------------------------------------------------------------------------------------

void testStaleOpenIntent(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Stale Open Intent");
    const auto first = session.admitOpenIntent();
    const auto second = session.admitOpenIntent();
    expectations.expect(static_cast<bool>(first) && static_cast<bool>(second),
                        "stale intent: both admissions succeed");
    if (!first || !second) {
        return;
    }
    auto content = takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort(),
                                      "stale intent: replacement content builds");
    if (!content.has_value()) {
        return;
    }
    const auto before = session.stateSnapshot();
    const auto status = session.installDecodedReplacement(first.capture(), std::move(*content));
    expectations.expect(status == SessionInstallStatus::StaleOpenIntent,
                        "stale intent: installing with the superseded first capture is refused as "
                        "StaleOpenIntent");
    const auto after = session.stateSnapshot();
    expectations.expect(after.resultAcceptanceGeneration == before.resultAcceptanceGeneration &&
                            after.pathIntentGeneration == before.pathIntentGeneration &&
                            after.contentKind == before.contentKind,
                        "stale intent: the refused install leaves the session's identity/content "
                        "state untouched");
}

void testAcceptanceMismatch(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Acceptance Mismatch");
    const auto firstAdmission = session.admitOpenIntent();
    expectations.expect(static_cast<bool>(firstAdmission),
                        "acceptance mismatch: admission succeeds");
    if (!firstAdmission) {
        return;
    }
    const auto firstCapture = firstAdmission.capture();

    auto priorContent = takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort("Prior"),
                                           "acceptance mismatch: prior replacement content builds");
    if (!priorContent.has_value()) {
        return;
    }
    const auto priorStatus =
        session.installDecodedReplacement(firstCapture, std::move(*priorContent));
    expectations.expect(priorStatus == SessionInstallStatus::Installed,
                        "acceptance mismatch: the prior install (using the still-current first "
                        "capture) succeeds -- installation never advances OpenIntentGeneration");

    auto secondContent =
        takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort("Second"),
                           "acceptance mismatch: second replacement content builds");
    if (!secondContent.has_value()) {
        return;
    }
    const auto before = session.stateSnapshot();
    const auto status = session.installDecodedReplacement(firstCapture, std::move(*secondContent));
    expectations.expect(
        status == SessionInstallStatus::AcceptanceMismatch,
        "acceptance mismatch: reusing the same (now result-acceptance-stale) first "
        "capture after a successful prior install is refused as AcceptanceMismatch, "
        "not StaleOpenIntent");
    const auto after = session.stateSnapshot();
    expectations.expect(
        after.resultAcceptanceGeneration == before.resultAcceptanceGeneration &&
            projectName(session) == "Prior",
        "acceptance mismatch: the refused second install leaves the PRIOR install's "
        "content in place");
}

// ---------------------------------------------------------------------------------------------
// Preserved-read-only install: a {1,1}-container archive -> Installed as preserved-read-only;
// stateSnapshot shows PreservedReadOnly kind; subsequent captureSaveInput -> ReadOnly; execute ->
// ReadOnly.
// ---------------------------------------------------------------------------------------------

void testPreservedReadOnlyInstall(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Preserved RO Host");
    const auto archiveBytes = buildManifestSidePreservedReadOnlyArchiveOrAbort();
    const auto path = ProjectDisplayPath::create("preserved.bloom");
    expectations.expect(path.has_value(), "preserved-ro install: fixture path constructs");
    if (!path.has_value()) {
        return;
    }

    auto result =
        openSessionArchive(session, archiveBytes, path, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "preserved-ro install: openSessionArchive installs the manifest-side "
                        "preserved archive");
    expectations.expect(result.attemptedContentKind() ==
                            ProjectSessionContentKind::PreservedReadOnly,
                        "preserved-ro install: the attempted content kind names PreservedReadOnly");
    const auto* preservation = result.preservedReadOnly();
    expectations.expect(
        preservation != nullptr && preservation->side == OpenArchivePreservedReadOnlySide::Manifest,
        "preserved-ro install: the preservation diagnostics carry the manifest side "
        "verbatim");

    const auto snapshot = session.stateSnapshot();
    expectations.expect(snapshot.contentKind == ProjectSessionContentKind::PreservedReadOnly &&
                            snapshot.displayPath == path,
                        "preserved-ro install: stateSnapshot shows PreservedReadOnly content kind "
                        "and the installed path");

    const auto plainIntent = session.capturePlainSavePathIntent();
    expectations.expect(!plainIntent.isValid(),
                        "preserved-ro install: no native plain-save intent can be captured");
    const auto saveInputResult = session.captureSaveInput(plainIntent);
    expectations.expect(saveInputResult.status() == SessionSaveInputStatus::ReadOnly,
                        "preserved-ro install: captureSaveInput reports ReadOnly");

    Transaction blocked("Blocked Edit");
    blocked.emplace<SetProjectName>("Must not apply");
    expectations.expect(session.execute(std::move(blocked)).status ==
                            bloom::host::ProjectSessionCommandStatus::ReadOnly,
                        "preserved-ro install: execute reports ReadOnly");
}

// A pathless preserved-read-only install is refused as typed InvalidContent
// (createPreservedReadOnly requires a path too): session untouched, no install*() call is ever
// made.
void testPathlessPreservedReadOnlyRefused(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Pathless Preserved");
    const auto before = session.stateSnapshot();
    const auto archiveBytes = buildManifestSidePreservedReadOnlyArchiveOrAbort();

    auto result = openSessionArchive(session, archiveBytes, std::nullopt, SaveArchiveLimits{},
                                     makeOperation());
    expectations.expect(!static_cast<bool>(result) &&
                            result.stage() == SessionOpenStage::Installation,
                        "pathless preserved: refused at the Installation stage");
    const auto* status = installedStatus(result);
    expectations.expect(status != nullptr && *status == SessionInstallStatus::InvalidContent,
                        "pathless preserved: the install outcome names InvalidContent");
    expectations.expect(result.preservedReadOnly() != nullptr,
                        "pathless preserved: preservation diagnostics are still carried verbatim "
                        "for the caller");

    const auto after = session.stateSnapshot();
    expectations.expect(after.contentKind == before.contentKind &&
                            after.resultAcceptanceGeneration == before.resultAcceptanceGeneration &&
                            after.displayPath == before.displayPath,
                        "pathless preserved: the session is untouched (installPreservedReadOnly"
                        "Replacement() was never called)");
}

// ---------------------------------------------------------------------------------------------
// Pending Save As cancelled by install: advance Save As, then install -> path intent back to
// ExistingPath at a new generation; the old Save As capture is stale for both save and
// abandonment.
// ---------------------------------------------------------------------------------------------

void testPendingSaveAsCancelledByInstall(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Pending Save As");
    const auto admission = session.admitOpenIntent();
    expectations.expect(static_cast<bool>(admission), "pending save as: admission succeeds");
    if (!admission) {
        return;
    }
    const auto intent = admission.capture();

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "pending save as: Save As advances");
    if (!saveAs) {
        return;
    }
    const auto pathGenerationBefore = session.stateSnapshot().pathIntentGeneration;

    auto content = takeDecodedContent(expectations, buildMinimalArchiveBytesOrAbort(),
                                      "pending save as: replacement content builds");
    if (!content.has_value()) {
        return;
    }
    const auto status = session.installDecodedReplacement(intent, std::move(*content));
    expectations.expect(status == SessionInstallStatus::Installed,
                        "pending save as: install succeeds despite the pending Save As phase");

    const auto after = session.stateSnapshot();
    expectations.expect(after.pathIntentKind == SessionPathIntentKind::ExistingPath &&
                            after.pathIntentGeneration != pathGenerationBefore,
                        "pending save as: path intent resets to ExistingPath at a new generation");

    const auto saveInputResult = session.captureSaveInput(saveAs.capture());
    expectations.expect(saveInputResult.status() == SessionSaveInputStatus::StaleIntent,
                        "pending save as: the old Save As capture is stale for save");
    expectations.expect(session.abandonSaveAsIntent(saveAs.capture()) ==
                            SessionPathIntentAbandonStatus::StaleIntent,
                        "pending save as: the old Save As capture is stale for abandonment too");
}

// ---------------------------------------------------------------------------------------------
// Open failure leaves session untouched: corrupt bytes -> Opening-stage failure. Content-bearing
// stateSnapshot fields are compared before/after; OpenIntentGeneration is compared SEPARATELY and
// is EXPECTED to advance by exactly one -- admission always consumes a generation for a new
// desired Open, whether or not that Open goes on to succeed (docs/architecture/project-session.md,
// "Open Intent": "A newer Open cancels or supersedes every older queued/running Open").
// ---------------------------------------------------------------------------------------------

void testOpenFailureLeavesSessionUntouched(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Corrupt Open");
    expectations.expect(session.execute(rename("Before Corrupt Open", session)).changed(),
                        "open failure: a baseline edit commits");
    const auto before = session.stateSnapshot();

    auto corrupted = buildMinimalArchiveBytesOrAbort();
    expectations.expect(corrupted.size() > 4, "open failure: fixture archive has bytes to corrupt");
    for (auto& byte : corrupted) {
        byte ^= std::byte{0xFF};
    }

    auto result =
        openSessionArchive(session, corrupted, std::nullopt, SaveArchiveLimits{}, makeOperation());
    expectations.expect(!static_cast<bool>(result) && result.stage() == SessionOpenStage::Opening,
                        "open failure: corrupt bytes fail at the Opening stage");
    expectations.expect(result.openingFailure() != nullptr,
                        "open failure: the wrapped SaveArchiveFailure is present");

    const auto after = session.stateSnapshot();
    expectations.expect(after.openIntentGeneration.value() ==
                            before.openIntentGeneration.value() + 1,
                        "open failure: admission still advances OpenIntentGeneration exactly once");
    expectations.expect(
        after.resultAcceptanceGeneration == before.resultAcceptanceGeneration &&
            after.pathIntentGeneration == before.pathIntentGeneration &&
            after.pathIntentKind == before.pathIntentKind &&
            after.contentKind == before.contentKind &&
            after.currentRevision == before.currentRevision &&
            after.cleanRevision == before.cleanRevision && after.dirty == before.dirty &&
            after.historySize == before.historySize && after.canUndo == before.canUndo &&
            after.canRedo == before.canRedo && after.displayPath == before.displayPath &&
            after.newestAcceptedPublicationIntent == before.newestAcceptedPublicationIntent,
        "open failure: every content-bearing field of stateSnapshot is untouched (no install*() "
        "call is ever made on this path)");
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion pass-through: a budget too small for openProjectArchive()'s own chain still
// fails somewhere in that composed chain, reported verbatim at the Opening stage; the session
// stays untouched.
// ---------------------------------------------------------------------------------------------

void testBudgetExhaustionPassThrough(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = makeDecodedSession(expectations, identitySource, "Budget Exhaustion");
    const auto archiveBytes = buildMinimalArchiveBytesOrAbort();

    constexpr std::uint64_t kTinyBudget = 512;
    auto tinyCoordinator = ProjectIoMemoryCoordinator::create(kTinyBudget);
    expectations.expect(tinyCoordinator.has_value(),
                        "budget exhaustion: tiny coordinator constructs");
    if (!tinyCoordinator.has_value()) {
        return;
    }
    auto tinyOperation = tinyCoordinator->createOperation(kTinyBudget, kTinyBudget);
    expectations.expect(tinyOperation.has_value(), "budget exhaustion: tiny operation constructs");
    if (!tinyOperation.has_value()) {
        return;
    }

    const auto before = session.stateSnapshot();
    auto result = openSessionArchive(session, archiveBytes, std::nullopt, SaveArchiveLimits{},
                                     std::move(*tinyOperation));
    expectations.expect(
        !static_cast<bool>(result) && result.stage() == SessionOpenStage::Opening,
        "budget exhaustion: a too-small operation budget still fails at the Opening "
        "stage, reported verbatim");
    const auto after = session.stateSnapshot();
    expectations.expect(after.resultAcceptanceGeneration == before.resultAcceptanceGeneration &&
                            after.currentRevision == before.currentRevision,
                        "budget exhaustion: the session is untouched");
    expectations.expect(tinyCoordinator->snapshot().currentBytes == 0,
                        "budget exhaustion: the constrained coordinator's charge returns to zero");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two independently-built sessions opening the SAME archive bytes install
// byte-identical content -- proven by re-saving each and comparing the published files.
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    TempDirectory directoryA;
    TempDirectory directoryB;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directoryA.isValid() || !directoryB.isValid() || !coordinator.has_value() ||
        !artifacts.has_value()) {
        expectations.expect(false, "determinism: fixture is available");
        return;
    }
    const auto archiveBytes = buildMinimalArchiveBytesOrAbort("Determinism Source");

    auto sessionA = makeDecodedSession(expectations, identitySource, "Determinism Host A");
    auto sessionB = makeDecodedSession(expectations, identitySource, "Determinism Host B");
    auto resultA = openSessionArchive(sessionA, archiveBytes, std::nullopt, SaveArchiveLimits{},
                                      makeOperation());
    auto resultB = openSessionArchive(sessionB, archiveBytes, std::nullopt, SaveArchiveLimits{},
                                      makeOperation());
    expectations.expect(static_cast<bool>(resultA) && static_cast<bool>(resultB),
                        "determinism: both independent opens install");
    expectations.expect(projectName(sessionA) == projectName(sessionB) &&
                            projectName(sessionA) == "Determinism Source",
                        "determinism: both installed sessions decode identical content");

    const auto targetA = directoryA.path() / "project.bloom";
    const auto targetB = directoryB.path() / "project.bloom";
    const SessionSaveRequest requestA{
        .targetPath = targetA,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = sessionA.capturePlainSavePathIntent().isValid()
                      ? sessionA.capturePlainSavePathIntent()
                      : sessionA.advancePathIntentForSaveAs().capture(),
    };
    const SessionSaveRequest requestB{
        .targetPath = targetB,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = sessionB.capturePlainSavePathIntent().isValid()
                      ? sessionB.capturePlainSavePathIntent()
                      : sessionB.advancePathIntentForSaveAs().capture(),
    };
    auto savedA = saveProjectSession(sessionA, *coordinator, *artifacts, requestA, makeOperation());
    auto savedB = saveProjectSession(sessionB, *coordinator, *artifacts, requestB, makeOperation());
    expectations.expect(static_cast<bool>(savedA) && static_cast<bool>(savedB),
                        "determinism: both re-saves fully succeed");

    const auto bytesA = readFile(targetA);
    const auto bytesB = readFile(targetB);
    expectations.expect(!bytesA.empty() && bytesA == bytesB,
                        "determinism: two independent openSessionArchive+save cycles of identical "
                        "archive bytes produce byte-identical published files");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testFullCycleSessionFileSession(expectations);
        testNewProjectFullCycleInstallsBloomNeutralColor(expectations);
        testRoundTrippedNewerMinorSurvivesFullCycle(expectations);
        testEditDuringOpenRefusal(expectations);
        testStaleOpenIntent(expectations);
        testAcceptanceMismatch(expectations);
        testPreservedReadOnlyInstall(expectations);
        testPathlessPreservedReadOnlyRefused(expectations);
        testPendingSaveAsCancelledByInstall(expectations);
        testOpenFailureLeavesSessionUntouched(expectations);
        testBudgetExhaustionPassThrough(expectations);
        testDeterminism(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
