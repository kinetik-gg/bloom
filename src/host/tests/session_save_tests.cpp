#include <bloom/host/session_save.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_decode.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

// Drives saveProjectSession() -- the synchronous session-save orchestration composing
// ProjectSession::captureSaveInput(), executeSavePublication(), and ProjectSession::
// acceptSavepoint() -- against REAL coordinators on per-test temporary directories, following
// src/host/tests/save_publication_tests.cpp's pattern one composition layer up (a
// ProjectSession replaces that file's bare CanonicalManifestV1/CanonicalDocumentV1 inputs).

namespace {

using bloom::commands::SetProjectName;
using bloom::commands::Transaction;

using bloom::host::ArtifactTargetKey;
using bloom::host::DecodedProjectEditability;
using bloom::host::DecodedProjectSessionRequest;
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionIdentitySource;
using bloom::host::ProjectSessionSavepointStatus;
using bloom::host::PublicationAdmission;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;
using bloom::host::PublicationIntentId;
using bloom::host::saveProjectSession;
using bloom::host::SavePublicationStage;
using bloom::host::SessionPathIntentAbandonStatus;
using bloom::host::SessionPathIntentKind;
using bloom::host::SessionSaveInputStatus;
using bloom::host::SessionSaveRequest;
using bloom::host::SessionSaveResult;
using bloom::host::SessionSaveSavepointBookkeepingFailure;
using bloom::host::SessionSaveStage;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::ArtifactTargetObservation;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactFaultPoint;
using bloom::platform::StagedArtifactPublicationOutcome;

using bloom::project::buildSaveArchive;
using bloom::project::canonicalDocumentSize;
using bloom::project::CanonicalDocumentV1;
using bloom::project::canonicalManifestSize;
using bloom::project::CanonicalManifestV1;
using bloom::project::decodeDocumentEnvelope;
using bloom::project::decodeManifestEnvelope;
using bloom::project::DocumentClassification;
using bloom::project::DocumentDecodeOutcome;
using bloom::project::DocumentDecodeResult;
using bloom::project::encodeCanonicalDocument;
using bloom::project::encodeCanonicalManifest;
using bloom::project::ManifestRequirement;
using bloom::project::parseStrictJsonDom;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::readZipContainer;
using bloom::project::reconstructDocument;
using bloom::project::RoundTripAttachmentPath;
using bloom::project::RoundTripState;
using bloom::project::SaveArchiveExpectedContent;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveStage;
using bloom::project::verifySaveArchive;
using bloom::project::ZipContainerLimits;

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
        constexpr std::string_view prefix = "/tmp/bloom-session-save-XXXXXX";
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
// Shared plumbing (mirrors save_publication_tests.cpp)
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

[[nodiscard]] std::optional<PublicationAdmission> admitIntent(PublicationCoordinator& coordinator,
                                                              Expectations& expectations,
                                                              const std::string_view context) {
    auto result = coordinator.admit();
    expectations.expect(static_cast<bool>(result), context);
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeAdmission();
}

// ---------------------------------------------------------------------------------------------
// Session-level fixtures
// ---------------------------------------------------------------------------------------------

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

// Independently encodes manifest.json/document.json straight from canonical_manifest.hpp/
// canonical_document.hpp -- never through saveProjectSession()/executeSavePublication()/
// save_archive.hpp -- to serve as an oracle for the byte content saveProjectSession() should
// have published.
struct OracleEntries final {
    std::vector<char> manifestBytes;
    std::vector<char> documentBytes;
};

[[nodiscard]] std::optional<OracleEntries>
buildOracleEntries(Expectations& expectations, const CanonicalManifestV1& manifest,
                   const CanonicalDocumentV1& documentInput) {
    const auto manifestSize = canonicalManifestSize(manifest);
    expectations.expect(static_cast<bool>(manifestSize), "oracle: manifest sizes");
    if (!manifestSize) {
        return std::nullopt;
    }
    std::vector<char> manifestBytes(*manifestSize.value());
    expectations.expect(static_cast<bool>(encodeCanonicalManifest(manifest, manifestBytes)),
                        "oracle: manifest encodes");

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    CanonicalDocumentV1 oracleRequest = documentInput;
    oracleRequest.payloadScratch = payloadScratch;
    oracleRequest.sortScratch = sortScratch;
    const auto documentSize = canonicalDocumentSize(oracleRequest);
    expectations.expect(static_cast<bool>(documentSize), "oracle: document sizes");
    if (!documentSize) {
        return std::nullopt;
    }
    std::vector<char> documentBytes(*documentSize.value());
    expectations.expect(static_cast<bool>(encodeCanonicalDocument(oracleRequest, documentBytes)),
                        "oracle: document encodes");
    return OracleEntries{std::move(manifestBytes), std::move(documentBytes)};
}

// Clones a RoundTripState through its public API only (RoundTripState is deliberately move-only
// and exposes no owning "take" accessor from a decode result -- see document_decode.hpp -- so a
// caller that needs to install an owned copy re-attaches every entry rather than reaching past the
// encapsulation).
[[nodiscard]] RoundTripState cloneRoundTrip(const RoundTripState& source) {
    RoundTripState clone;
    for (const auto& entry : source.entries()) {
        clone.attach(entry.path, entry.members);
    }
    return clone;
}

// ---------------------------------------------------------------------------------------------
// End to end: createDecoded with color settings + empty requirements -> an edit transaction ->
// Save As advance -> saveProjectSession -> Published -> acceptSavepoint Accepted -> session state
// reflects the accepted savepoint -> the published file verifies via verifySaveArchive() against
// independently-encoded entries.
// ---------------------------------------------------------------------------------------------

void testEndToEndSaveAs(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "end to end: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    const auto colorSettings = neutralColorSettings();

    auto session = makeDecodedSession(expectations, identitySource, "End To End Project");
    expectations.expect(session.execute(rename("Renamed", session)).changed(),
                        "end to end: the edit commits");
    const auto editedRevision = currentRevision(session);

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "end to end: Save As advances path authority");
    if (!saveAs) {
        return;
    }

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto result = saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "end to end: saveProjectSession fully succeeds (Published + Accepted)");
    expectations.expect(
        result.stage() == SessionSaveStage::Savepoint && result.publication() != nullptr &&
            result.publication()->outcome == StagedArtifactPublicationOutcome::Published &&
            result.savepointStatus() == ProjectSessionSavepointStatus::Accepted,
        "end to end: the result names Published + Accepted verbatim");

    const auto state = session.stateSnapshot();
    expectations.expect(state.cleanRevision == editedRevision && state.dirty == false &&
                            state.displayPath.has_value() &&
                            state.displayPath->value() == targetPath &&
                            result.intentId().has_value() &&
                            state.newestAcceptedPublicationIntent == *result.intentId(),
                        "end to end: session state reflects the accepted savepoint");

    const auto decodedSnapshot = session.decodedSnapshot();
    expectations.expect(static_cast<bool>(decodedSnapshot),
                        "end to end: a decoded snapshot exists");
    if (!decodedSnapshot) {
        return;
    }
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &decodedSnapshot.snapshot(),
                                            .colorSettings = &colorSettings};
    const auto oracleEntries = buildOracleEntries(expectations, manifest, documentInput);
    if (!oracleEntries.has_value()) {
        return;
    }
    const SaveArchiveExpectedContent expected{
        .manifestBytes = asBytes(oracleEntries->manifestBytes),
        .documentBytes = asBytes(oracleEntries->documentBytes),
        .documentSchemaVersion = manifest.documentSchemaVersion,
    };
    const auto published = readFile(targetPath);
    auto reverified =
        verifySaveArchive(asBytes(published), expected, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reverified),
                        "end to end: verifySaveArchive independently re-verifies the published "
                        "file's bytes as the final oracle");
}

// ---------------------------------------------------------------------------------------------
// Plain save after Save As: a second edit, a plain-save intent capture, save to the SAME path
// (ExistingPath) -> published + accepted, path unchanged.
// ---------------------------------------------------------------------------------------------

void testPlainSaveAfterSaveAs(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "plain save after save as: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    auto session = makeDecodedSession(expectations, identitySource, "Plain Save After Save As");
    expectations.expect(session.execute(rename("First", session)).changed(),
                        "plain save after save as: the first edit commits");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs),
                        "plain save after save as: Save As advances path authority");
    if (!saveAs) {
        return;
    }
    const SessionSaveRequest firstRequest{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto firstResult =
        saveProjectSession(session, *coordinator, *artifacts, firstRequest, makeOperation());
    expectations.expect(static_cast<bool>(firstResult),
                        "plain save after save as: the Save As publish fully succeeds");
    if (!firstResult) {
        return;
    }
    const auto pathAfterSaveAs = session.stateSnapshot().displayPath;

    expectations.expect(session.execute(rename("Second", session)).changed(),
                        "plain save after save as: the second edit commits");
    const auto secondRevision = currentRevision(session);
    const auto plainIntent = session.capturePlainSavePathIntent();
    expectations.expect(plainIntent.isValid() &&
                            plainIntent.kind() == SessionPathIntentKind::ExistingPath,
                        "plain save after save as: a plain-save intent captures existing-path "
                        "authority");

    std::optional<ArtifactTargetObservation> observedFingerprint;
    {
        auto peek =
            artifacts->preflight({.targetPath = targetPath,
                                  .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
                                  .expectedTarget = std::nullopt});
        expectations.expect(peek && peek.target()->observation().exists,
                            "plain save after save as: preflight observes the published "
                            "fingerprint");
        if (!peek) {
            return;
        }
        observedFingerprint = peek.target()->observation();
    }
    const SessionSaveRequest secondRequest{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
        .expectedTarget = observedFingerprint,
        .limits = {},
        .intent = plainIntent,
    };
    auto secondResult =
        saveProjectSession(session, *coordinator, *artifacts, secondRequest, makeOperation());
    expectations.expect(static_cast<bool>(secondResult),
                        "plain save after save as: the plain-save publish fully succeeds");
    const auto state = session.stateSnapshot();
    expectations.expect(state.cleanRevision == secondRevision && state.dirty == false &&
                            state.displayPath == pathAfterSaveAs,
                        "plain save after save as: published + accepted with the path unchanged");
}

// ---------------------------------------------------------------------------------------------
// Round-tripped newer minor: install a RoundTripState (built via the real document decode path
// over a spliced 1.1 fixture, mirroring save_archive_tests.cpp) + schemaMinor 1 at creation ->
// save -> the manifest in the published file declares {1,1} and the unknown member survives
// (reopen + decode assert).
// ---------------------------------------------------------------------------------------------

void testRoundTrippedNewerMinorGreenChain(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "round-tripped newer minor: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    auto newProject = bloom::document::makeNewProject("Untitled Project", "Main Composition",
                                                      bloom::core::RationalTime::fromInteger(10));
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
    expectations.expect(static_cast<bool>(size), "round-tripped newer minor: baseline sizes");
    if (!size) {
        return;
    }
    std::string text(*size.value(), '\0');
    const auto written =
        encodeCanonicalDocument(baselineRequest, std::span<char>(text.data(), text.size()));
    expectations.expect(static_cast<bool>(written), "round-tripped newer minor: baseline encodes");
    if (!written) {
        return;
    }

    const std::string anchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto anchorPos = text.find(anchor);
    expectations.expect(anchorPos != std::string::npos,
                        "round-tripped newer minor: root schemaVersion anchor is located");
    if (anchorPos == std::string::npos) {
        return;
    }
    text.replace(anchorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");
    expectations.expect(text.size() >= 2 && text.back() == '\n' && text[text.size() - 2] == '}',
                        "round-tripped newer minor: baseline ends with the root's closing brace");
    if (text.size() < 2 || text.back() != '\n' || text[text.size() - 2] != '}') {
        return;
    }
    text.resize(text.size() - 2);
    text += R"(,"zzzFutureField":42})";
    text += '\n';

    auto parsed = parseStrictJsonDom(asBytes(text), {}, makeOperation());
    expectations.expect(static_cast<bool>(parsed), "round-tripped newer minor: spliced document "
                                                   "parses");
    if (!parsed) {
        return;
    }
    DocumentDecodeResult decoded = decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(
        decoded.outcome() == DocumentDecodeOutcome::Decoded &&
            decoded.classification() == DocumentClassification::EditableWithRoundTrip &&
            decoded.value() != nullptr && decoded.roundTrip() != nullptr,
        "round-tripped newer minor: spliced document decodes with round-trip state");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        return;
    }

    auto reconstructed = reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed),
                        "round-tripped newer minor: spliced document reconstructs");
    if (!reconstructed) {
        return;
    }
    auto* reconstructedValue = reconstructed.value();
    const auto persistedHighWater = reconstructedValue->document->snapshot().ids().highWater();
    auto project = reconstructedValue->document->snapshot().project();
    auto roundTrip = cloneRoundTrip(*decoded.roundTrip());

    auto sessionResult = ProjectSession::createDecoded(
        identitySource, {.project = std::move(project),
                         .colorSettings = reconstructedValue->colorSettings,
                         .editability = DecodedProjectEditability::Editable,
                         .displayPath = std::nullopt,
                         .persistedAllocatorHighWater = persistedHighWater,
                         .roundTrip = std::move(roundTrip),
                         .schemaMinor = 1,
                         .retainedRequirements = {}});
    expectations.expect(static_cast<bool>(sessionResult),
                        "round-tripped newer minor: the session installs the round-tripped "
                        "content");
    if (!sessionResult) {
        return;
    }
    auto session = std::move(sessionResult).takeSession();

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs),
                        "round-tripped newer minor: Save As advances path authority");
    if (!saveAs) {
        return;
    }
    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto result = saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "round-tripped newer minor: saveProjectSession fully succeeds (the "
                        "editable-reopen boundary tolerates our own schemaMinor > 0 archive)");
    if (!result) {
        return;
    }

    const auto published = readFile(targetPath);
    auto reopened = readZipContainer(asBytes(published), ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reopened),
                        "round-tripped newer minor: the published archive reopens");
    if (!reopened) {
        return;
    }

    auto manifestDom =
        parseStrictJsonDom(reopened.document()->manifestBytes(), {}, makeOperation());
    expectations.expect(static_cast<bool>(manifestDom),
                        "round-tripped newer minor: reopened manifest.json parses");
    if (manifestDom) {
        auto decodedManifest = decodeManifestEnvelope(manifestDom.document()->root());
        expectations.expect(
            static_cast<bool>(decodedManifest) && decodedManifest.value() != nullptr &&
                decodedManifest.value()->documentSchemaVersion ==
                    bloom::document::SchemaVersion{1, 1},
            "round-tripped newer minor: the published manifest declares document schema {1,1}");
    }

    auto documentDom =
        parseStrictJsonDom(reopened.document()->documentBytes(), {}, makeOperation());
    expectations.expect(static_cast<bool>(documentDom),
                        "round-tripped newer minor: reopened document.json parses");
    if (!documentDom) {
        return;
    }
    DocumentDecodeResult redecoded = decodeDocumentEnvelope(documentDom.document()->root());
    expectations.expect(redecoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            redecoded.classification() ==
                                DocumentClassification::EditableWithRoundTrip,
                        "round-tripped newer minor: the reopened archive still classifies "
                        "EditableWithRoundTrip, never PreservedReadOnlyRequired");
    if (redecoded.roundTrip() == nullptr) {
        expectations.expect(false, "round-tripped newer minor: the reopened archive retains "
                                   "RoundTripState");
        return;
    }
    const auto* members = redecoded.roundTrip()->find(RoundTripAttachmentPath{});
    expectations.expect(members != nullptr && members->size() == 1 &&
                            (*members)[0].key() == "zzzFutureField",
                        "round-tripped newer minor: the unknown root member survives the "
                        "save/reopen chain byte-exactly");
}

// ---------------------------------------------------------------------------------------------
// Refusals: preserved-read-only session -> ReadOnly; pathless plain save -> PathRequired; stale
// path intent (advance Save As twice, use the first capture) -> StaleIntent. Capture after
// session replacement has no test: no session-replacement/installation API exists yet in this
// slice (see the task package's non-goals) -- noted rather than fabricated.
// ---------------------------------------------------------------------------------------------

void testRefusals(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "refusals: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    {
        auto preservedResult =
            ProjectSession::createPreservedReadOnly(identitySource, "preserved.bloom");
        expectations.expect(static_cast<bool>(preservedResult),
                            "refusals: the preserved fixture installs");
        if (preservedResult) {
            auto preserved = std::move(preservedResult).takeSession();
            const SessionSaveRequest request{.targetPath = targetPath,
                                             .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                             .expectedTarget = std::nullopt,
                                             .limits = {},
                                             .intent = {}};
            auto result =
                saveProjectSession(preserved, *coordinator, *artifacts, request, makeOperation());
            expectations.expect(!static_cast<bool>(result) &&
                                    result.stage() == SessionSaveStage::Capture,
                                "refusals: preserved content is refused at the Capture stage");
            const auto* outcome = result.captureOutcome();
            const auto* status =
                outcome != nullptr ? std::get_if<SessionSaveInputStatus>(outcome) : nullptr;
            expectations.expect(status != nullptr && *status == SessionSaveInputStatus::ReadOnly,
                                "refusals: the capture outcome names ReadOnly");
        }
    }

    {
        auto session = makeDecodedSession(expectations, identitySource, "Pathless Refusal");
        const auto plainIntent = session.capturePlainSavePathIntent();
        const SessionSaveRequest request{.targetPath = targetPath,
                                         .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                         .expectedTarget = std::nullopt,
                                         .limits = {},
                                         .intent = plainIntent};
        auto result =
            saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
        expectations.expect(!static_cast<bool>(result) &&
                                result.stage() == SessionSaveStage::Capture,
                            "refusals: a pathless plain save is refused at the Capture stage");
        const auto* outcome = result.captureOutcome();
        const auto* status =
            outcome != nullptr ? std::get_if<SessionSaveInputStatus>(outcome) : nullptr;
        expectations.expect(status != nullptr && *status == SessionSaveInputStatus::PathRequired,
                            "refusals: the capture outcome names PathRequired");
    }

    {
        auto session = makeDecodedSession(expectations, identitySource, "Stale Refusal");
        const auto first = session.advancePathIntentForSaveAs();
        const auto second = session.advancePathIntentForSaveAs();
        expectations.expect(static_cast<bool>(first) && static_cast<bool>(second),
                            "refusals: both Save As advances succeed");
        const SessionSaveRequest request{.targetPath = targetPath,
                                         .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                         .expectedTarget = std::nullopt,
                                         .limits = {},
                                         .intent = first.capture()};
        auto result =
            saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
        expectations.expect(!static_cast<bool>(result) &&
                                result.stage() == SessionSaveStage::Capture,
                            "refusals: a superseded Save As capture is refused at the Capture "
                            "stage");
        const auto* outcome = result.captureOutcome();
        const auto* status =
            outcome != nullptr ? std::get_if<SessionSaveInputStatus>(outcome) : nullptr;
        expectations.expect(status != nullptr && *status == SessionSaveInputStatus::StaleIntent,
                            "refusals: the capture outcome names StaleIntent");
    }
}

// ---------------------------------------------------------------------------------------------
// Non-published outcomes leave the session untouched: FailedBeforePublication (a platform fault
// injected at AtomicPublication) and Superseded (a competing higher intent registered via the
// executor's preAdmitted seam, exactly as save_publication_tests.cpp's testSupersession drives
// the coordinator directly). Both leave dirty/clean-revision/path/newestAcceptedPublicationIntent
// unchanged; the Superseded case then abandons its Save As intent, returning path authority to
// ExistingPath without fabricating a path.
// ---------------------------------------------------------------------------------------------

void testFailedBeforePublicationLeavesSessionUntouched(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    StagedArtifactConfig config;
    config.faults = {.point = StagedArtifactFaultPoint::AtomicPublication, .occurrence = 1};
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "failed before publication: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    auto session =
        makeDecodedSession(expectations, identitySource, "Failed Before Publication", targetPath);
    expectations.expect(session.execute(rename("Edited", session)).changed(),
                        "failed before publication: the edit commits");
    const auto beforeState = session.stateSnapshot();
    const auto plainIntent = session.capturePlainSavePathIntent();
    const SessionSaveRequest request{.targetPath = targetPath,
                                     .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                     .expectedTarget = std::nullopt,
                                     .limits = {},
                                     .intent = plainIntent};
    auto result = saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(
        !static_cast<bool>(result) && result.stage() == SessionSaveStage::Savepoint &&
            result.publication() != nullptr &&
            result.publication()->outcome ==
                StagedArtifactPublicationOutcome::FailedBeforePublication &&
            !result.savepointStatus().has_value(),
        "failed before publication: FailedBeforePublication triggers no acceptSavepoint attempt");
    const auto afterState = session.stateSnapshot();
    expectations.expect(
        afterState.dirty == true && afterState.cleanRevision == beforeState.cleanRevision &&
            afterState.displayPath == beforeState.displayPath &&
            afterState.newestAcceptedPublicationIntent ==
                beforeState.newestAcceptedPublicationIntent,
        "failed before publication: session state is untouched by a non-published outcome");
}

void testSupersededLeavesSessionUntouchedThenAbandons(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "supersession: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    auto session = makeDecodedSession(expectations, identitySource, "Supersession Project");
    expectations.expect(session.execute(rename("Edited", session)).changed(),
                        "supersession: the edit commits");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "supersession: Save As advances");
    if (!saveAs) {
        return;
    }
    const auto beforeState = session.stateSnapshot();

    ArtifactTargetKey targetKey;
    {
        auto peek = artifacts->preflight({.targetPath = targetPath,
                                          .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                          .expectedTarget = std::nullopt});
        expectations.expect(static_cast<bool>(peek), "supersession: the peek preflight succeeds");
        if (!peek) {
            return;
        }
        targetKey = peek.target()->targetKey();
    }

    auto lowerAdmission = admitIntent(*coordinator, expectations,
                                      "supersession: the executor's own (lower) intent is admitted "
                                      "first");
    auto competitorAdmission = admitIntent(*coordinator, expectations,
                                           "supersession: the competing (higher) intent is "
                                           "admitted second");
    if (!lowerAdmission.has_value() || !competitorAdmission.has_value()) {
        return;
    }
    auto competitorRegistration =
        coordinator->registerTarget(std::move(*competitorAdmission), targetKey);
    expectations.expect(static_cast<bool>(competitorRegistration),
                        "supersession: the competitor registers for the identical target key");
    if (!competitorRegistration) {
        return;
    }
    std::move(competitorRegistration).takeClaim().reset();

    const SessionSaveRequest request{.targetPath = targetPath,
                                     .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                     .expectedTarget = std::nullopt,
                                     .limits = {},
                                     .intent = saveAs.capture(),
                                     .preAdmitted = &(*lowerAdmission)};
    auto result = saveProjectSession(session, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(
        !static_cast<bool>(result) && result.stage() == SessionSaveStage::Savepoint &&
            result.publication() != nullptr &&
            result.publication()->outcome == StagedArtifactPublicationOutcome::Superseded &&
            !result.savepointStatus().has_value(),
        "supersession: Superseded triggers no acceptSavepoint attempt");

    const auto afterState = session.stateSnapshot();
    expectations.expect(afterState.dirty == true &&
                            afterState.cleanRevision == beforeState.cleanRevision &&
                            !afterState.displayPath.has_value() &&
                            afterState.newestAcceptedPublicationIntent ==
                                beforeState.newestAcceptedPublicationIntent &&
                            afterState.pathIntentKind == SessionPathIntentKind::ReplacementPath,
                        "supersession: session state is untouched by a non-published outcome");

    expectations.expect(
        session.abandonSaveAsIntent(saveAs.capture()) == SessionPathIntentAbandonStatus::Abandoned,
        "supersession: the still-current replacement intent can be abandoned after a "
        "non-published outcome");
    const auto abandonedState = session.stateSnapshot();
    expectations.expect(abandonedState.pathIntentKind == SessionPathIntentKind::ExistingPath &&
                            !abandonedState.displayPath.has_value(),
                        "supersession: abandonment restores existing-path authority without "
                        "fabricating a path");
}

// ---------------------------------------------------------------------------------------------
// Savepoint refusal surfaced: forcing a genuine acceptSavepoint() refusal AFTER a real
// executeSavePublication() publish() requires racing session state against the executor between
// its Guard stage and the caller's own acceptSavepoint() call -- no seam in this synchronous
// slice exposes that window (the frozen non-goals forbid adding one: "acceptSavepoint and the
// path-authority machinery are EXISTING semantics -- wire them, do not redefine them"). Every
// acceptSavepoint() refusal status (ReadOnly, InvalidPublicationIntent, StaleIntent,
// UnknownRevision, PathRequired, PathAuthorityMismatch) is already pinned directly against
// ProjectSession in src/host/tests/project_session_tests.cpp (testSavepointPathAuthority,
// testPublicationCallbackOrdering, testPublicationFrontierScopesToPathGeneration,
// testDirtySavepointBranching, testPreservedReadOnlyState) -- verified present, not duplicated
// here. SessionSaveResult::savepointStatus() forwards whatever acceptSavepoint() returns
// verbatim (see saveProjectSession()'s implementation), so this module adds no additional
// masking risk beyond what those tests already cover.
//
// A DIFFERENT truthfulness risk lives in the same catch block: if the LOCAL acceptSavepoint()
// call itself throws (std::bad_alloc from its own bookkeeping, e.g. a Save As replacement path
// copy) AFTER a real Published/PublishedWithDurabilityWarning outcome, the file has already been
// durably replaced -- reporting that as an unpublished/failed result would be untruthful. This is
// handled by SessionSaveResult::publishedSavepointBookkeepingFailed() (see saveProjectSession()'s
// final catch block). Forcing that exact allocation to fail deterministically has no seam either
// (acceptSavepoint()'s own allocations are tiny -- a filesystem::path copy at most -- and this
// slice adds no fault-injection hook per the frozen non-goals), so the trigger is not exercised
// here. Its RESULT SHAPE is pinned directly below instead, mirroring how the refusal statuses
// above are pinned without needing the exact runtime condition that produces them.
// ---------------------------------------------------------------------------------------------

void testPublishedSavepointBookkeepingFailedShape(Expectations& expectations) {
    const bloom::platform::StagedArtifactPublicationResult publication{
        .outcome = StagedArtifactPublicationOutcome::Published,
        .error = bloom::platform::StagedArtifactError::None,
    };
    const auto intentId = PublicationIntentId::fromRaw(7);

    for (const auto failure : {SessionSaveSavepointBookkeepingFailure::ResourceExhausted,
                               SessionSaveSavepointBookkeepingFailure::UnexpectedFailure}) {
        auto result =
            SessionSaveResult::publishedSavepointBookkeepingFailed(publication, intentId, failure);
        expectations.expect(
            !static_cast<bool>(result) && result.stage() == SessionSaveStage::Savepoint &&
                result.publication() != nullptr &&
                result.publication()->outcome == StagedArtifactPublicationOutcome::Published &&
                result.publication()->targetWasPublished() && result.intentId() == intentId &&
                !result.savepointStatus().has_value() &&
                result.savepointBookkeepingFailure() == failure,
            "publishedSavepointBookkeepingFailed: the real publication and intent survive "
            "verbatim, operator bool() is false, savepointStatus() is absent, and the exact "
            "bookkeeping-failure kind is named");
    }
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion pass-through: a budget just above buildSaveArchive()'s own peak charge for a
// bulk fixture (mirroring save_publication_tests.cpp's withBulkDocumentInput/testBudgetExhaustion
// two-pass technique) still fails somewhere in saveProjectSession()'s composed chain, reported
// verbatim at the Publication stage.
// ---------------------------------------------------------------------------------------------

struct BulkFixture final {
    ProjectSession session;
    bloom::document::ColorSettings colorSettings;
    std::vector<ManifestRequirement> requirements;
};

[[nodiscard]] std::optional<BulkFixture>
makeBulkFixture(Expectations& expectations, ProjectSessionIdentitySource& identitySource) {
    using bloom::document::ExtensionRecord;
    using bloom::document::ExtensionRecordId;
    using bloom::document::NoExtensionReferences;
    using bloom::document::OpaqueExtensionPayload;

    auto newProject = bloom::document::makeNewProject("Bulk Budget Project", "Main Composition",
                                                      bloom::core::RationalTime::fromInteger(10));
    constexpr std::uint64_t kRecordCount = 60;
    constexpr std::size_t kPayloadBytes = 200'000;
    std::uint64_t xorshiftState = 0x9E3779B97F4A7C15ULL;
    const auto nextPseudoRandomByte = [&xorshiftState]() noexcept {
        xorshiftState ^= xorshiftState << 13U;
        xorshiftState ^= xorshiftState >> 7U;
        xorshiftState ^= xorshiftState << 17U;
        return static_cast<std::byte>(xorshiftState & 0xFFU);
    };
    for (std::uint64_t index = 1; index <= kRecordCount; ++index) {
        std::vector<std::byte> payloadBytes(kPayloadBytes);
        std::ranges::generate(payloadBytes, nextPseudoRandomByte);
        ExtensionRecord record{ExtensionRecordId::fromRaw(index),
                               "vendor.bulk",
                               "vendor.bulk.record",
                               {1, 0},
                               std::nullopt,
                               "application/octet-stream",
                               NoExtensionReferences{},
                               OpaqueExtensionPayload{std::move(payloadBytes)}};
        const bool added = newProject.project.addExtensionRecord(std::move(record));
        expectations.expect(added, "bulk fixture extension record adds");
        if (!added) {
            return std::nullopt;
        }
    }
    std::vector<ManifestRequirement> requirements{{.providerId = "vendor.bulk",
                                                   .capabilityId = "vendor.bulk.cap",
                                                   .schemaVersion = {1, 0},
                                                   .providedNodeTypeIds = {}}};
    auto colorSettings = neutralColorSettings();
    auto sessionResult = ProjectSession::createDecoded(
        identitySource, {.project = std::move(newProject.project),
                         .colorSettings = colorSettings,
                         .editability = DecodedProjectEditability::Editable,
                         .displayPath = std::nullopt,
                         .persistedAllocatorHighWater = std::nullopt,
                         .roundTrip = std::nullopt,
                         .schemaMinor = 0,
                         .retainedRequirements = requirements});
    expectations.expect(static_cast<bool>(sessionResult), "the bulk fixture session installs");
    if (!sessionResult) {
        return std::nullopt;
    }
    return BulkFixture{std::move(sessionResult).takeSession(), std::move(colorSettings),
                       std::move(requirements)};
}

void testBudgetExhaustionPassThrough(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "budget exhaustion: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    auto fixture = makeBulkFixture(expectations, identitySource);
    if (!fixture.has_value()) {
        return;
    }
    const auto saveAs = fixture->session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "budget exhaustion: Save As advances");
    if (!saveAs) {
        return;
    }
    const auto decodedSnapshot = fixture->session.decodedSnapshot();
    expectations.expect(static_cast<bool>(decodedSnapshot),
                        "budget exhaustion: a decoded snapshot exists");
    if (!decodedSnapshot) {
        return;
    }

    const CanonicalManifestV1 probeManifest{.documentSchemaVersion = {1, 0},
                                            .requirements = fixture->requirements};
    const CanonicalDocumentV1 probeDocument{.snapshot = &decodedSnapshot.snapshot(),
                                            .colorSettings = &fixture->colorSettings};

    constexpr std::uint64_t kBulkProbeBudget = 256ULL << 20U;
    auto probeMemory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
    expectations.expect(probeMemory.has_value(), "budget exhaustion: probe coordinator constructs");
    if (!probeMemory.has_value()) {
        return;
    }
    auto probeOperation = probeMemory->createOperation(kBulkProbeBudget, kBulkProbeBudget);
    expectations.expect(probeOperation.has_value(),
                        "budget exhaustion: probe operation constructs");
    if (!probeOperation.has_value()) {
        return;
    }
    std::uint64_t buildPeakBytes = 0;
    {
        auto probeBuild =
            buildSaveArchive(probeManifest, probeDocument, SaveArchiveLimits{}, *probeOperation);
        expectations.expect(static_cast<bool>(probeBuild),
                            "budget exhaustion: probe build succeeds");
        if (!probeBuild) {
            return;
        }
        buildPeakBytes = probeOperation->snapshot().peakBytes;
    }

    constexpr std::uint64_t kMargin = 64;
    const auto budget = buildPeakBytes + kMargin;
    auto memory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
    expectations.expect(memory.has_value(), "budget exhaustion: coordinator constructs");
    if (!memory.has_value()) {
        return;
    }
    auto operation = memory->createOperation(budget, budget);
    expectations.expect(operation.has_value(),
                        "budget exhaustion: constrained operation constructs");
    if (!operation.has_value()) {
        return;
    }

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto result = saveProjectSession(fixture->session, *coordinator, *artifacts, request,
                                     std::move(*operation));
    expectations.expect(
        !static_cast<bool>(result) && result.stage() == SessionSaveStage::Publication,
        "budget exhaustion: a budget just above the build's own peak still fails somewhere in "
        "the chain, reported at the Publication stage");
    const auto* failure = result.publicationFailure();
    expectations.expect(failure != nullptr && failure->stage() == SavePublicationStage::StagedSave,
                        "budget exhaustion: the wrapped SavePublicationFailure is scoped to "
                        "StagedSave");

    const auto memorySnapshot = memory->snapshot();
    expectations.expect(memorySnapshot.currentBytes == 0,
                        "budget exhaustion: the constrained coordinator's charge returns to zero");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two independently-built sessions with identical content, each saved once through
// saveProjectSession() to a distinct target, produce byte-identical published files.
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
    const auto targetPathA = directoryA.path() / "project.bloom";
    const auto targetPathB = directoryB.path() / "project.bloom";

    auto sessionA = makeDecodedSession(expectations, identitySource, "Determinism Project");
    const auto saveAsA = sessionA.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAsA), "determinism: A's Save As advances");
    if (saveAsA) {
        const SessionSaveRequest requestA{
            .targetPath = targetPathA,
            .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
            .expectedTarget = std::nullopt,
            .limits = {},
            .intent = saveAsA.capture(),
        };
        auto resultA =
            saveProjectSession(sessionA, *coordinator, *artifacts, requestA, makeOperation());
        expectations.expect(static_cast<bool>(resultA), "determinism: A's publish fully succeeds");
    }

    auto sessionB = makeDecodedSession(expectations, identitySource, "Determinism Project");
    const auto saveAsB = sessionB.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAsB), "determinism: B's Save As advances");
    if (saveAsB) {
        const SessionSaveRequest requestB{
            .targetPath = targetPathB,
            .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
            .expectedTarget = std::nullopt,
            .limits = {},
            .intent = saveAsB.capture(),
        };
        auto resultB =
            saveProjectSession(sessionB, *coordinator, *artifacts, requestB, makeOperation());
        expectations.expect(static_cast<bool>(resultB), "determinism: B's publish fully succeeds");
    }

    const auto bytesA = readFile(targetPathA);
    const auto bytesB = readFile(targetPathB);
    expectations.expect(!bytesA.empty() && bytesA == bytesB,
                        "determinism: two saveProjectSession runs of identical content produce "
                        "byte-identical published files");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testEndToEndSaveAs(expectations);
        testPlainSaveAfterSaveAs(expectations);
        testRoundTrippedNewerMinorGreenChain(expectations);
        testRefusals(expectations);
        testFailedBeforePublicationLeavesSessionUntouched(expectations);
        testSupersededLeavesSessionUntouchedThenAbandons(expectations);
        testPublishedSavepointBookkeepingFailedShape(expectations);
        testBudgetExhaustionPassThrough(expectations);
        testDeterminism(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
