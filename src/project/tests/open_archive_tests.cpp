#include <bloom/project/open_archive.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include "zip_container_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::project::buildSaveArchive;
using bloom::project::buildVerifiedSaveArchive;
using bloom::project::canonicalDocumentSize;
using bloom::project::CanonicalDocumentV1;
using bloom::project::canonicalManifestSize;
using bloom::project::CanonicalManifestV1;
using bloom::project::DocumentDecodeResult;
using bloom::project::encodeCanonicalDocument;
using bloom::project::encodeCanonicalManifest;
using bloom::project::ManifestRequirement;
using bloom::project::OpenArchiveOutcome;
using bloom::project::OpenArchivePreservedReadOnlySide;
using bloom::project::OpenedArchive;
using bloom::project::openProjectArchive;
using bloom::project::parseStrictJsonDom;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::RoundTripPreservationReason;
using bloom::project::SaveArchiveContainerReadFailure;
using bloom::project::SaveArchiveDocumentDecodeFailure;
using bloom::project::SaveArchiveJsonParseFailure;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveRequirementsFailure;
using bloom::project::SaveArchiveResourceExhausted;
using bloom::project::SaveArchiveStage;
using bloom::project::SaveArchiveVersionAgreementFailure;
using bloom::project::ZipContainerError;

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

// ---------------------------------------------------------------------------------------------
// Shared plumbing (mirrors save_archive_tests.cpp's fixtures; deliberately not shared across test
// binaries, matching this codebase's existing per-test-file fixture duplication precedent, e.g.
// neutralColorSettings() also appears in session_save_tests.cpp).
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U; // 32 MiB: ample for every fixture.

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

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
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

// Byte-compares two archives (this test file's own "were these two Opens/Saves the same
// content" oracle).
[[nodiscard]] bool sameBytes(const std::span<const std::byte> left,
                             const std::span<const std::byte> right) noexcept {
    return left.size() == right.size() &&
           (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

// ---------------------------------------------------------------------------------------------
// Open-what-save-wrote round trips: build a verified archive through the exact same save
// fixtures save_archive_tests.cpp uses, open it, assert the decoded values, then re-save the
// opened state through the save chain and require the republished bytes equal the originals
// byte-for-byte -- the full open-to-save cycle this task's test plan calls for.
// ---------------------------------------------------------------------------------------------

void testOpenMinimalRoundTrip(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "open minimal: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};

    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built),
                        "open minimal: buildVerifiedSaveArchive succeeds");
    if (!built) {
        return;
    }
    const auto* archive = built.archive();
    expectations.expect(archive != nullptr, "open minimal: a successful build exposes its archive");
    if (archive == nullptr) {
        return;
    }

    auto opened = openProjectArchive(archive->bytes(), SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Opened,
                        "open minimal: openProjectArchive opens the archive save just wrote");
    if (opened.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto openedValue = std::move(opened).takeOpened();
    expectations.expect(openedValue.document != nullptr, "open minimal: Opened owns a Document");
    if (!openedValue.document) {
        return;
    }

    auto openedSnapshot = openedValue.document->snapshot();
    expectations.expect(openedSnapshot.project().name() == "Untitled Project",
                        "open minimal: project name decodes exactly");
    expectations.expect(openedSnapshot.project().compositions().size() == 1,
                        "open minimal: one composition decodes");
    expectations.expect(openedValue.colorSettings == colorSettings,
                        "open minimal: colorSettings decode equal to what was saved");
    expectations.expect(openedValue.schemaMinor == 0, "open minimal: schemaMinor decodes to 0");
    expectations.expect(!openedValue.roundTrip.has_value(),
                        "open minimal: no RoundTripState for an exact {1,0} document");
    expectations.expect(openedValue.requirements.empty(),
                        "open minimal: the empty requirement set decodes empty");
    expectations.expect(openedValue.containerVersion == bloom::document::SchemaVersion{1, 0} &&
                            openedValue.documentSchemaVersion ==
                                bloom::document::SchemaVersion{1, 0},
                        "open minimal: container/document schema versions decode to {1,0}");

    const CanonicalManifestV1 resaveManifest{.documentSchemaVersion =
                                                 openedValue.documentSchemaVersion,
                                             .requirements = openedValue.requirements};
    const CanonicalDocumentV1 resaveDocumentInput{
        .snapshot = &openedSnapshot,
        .colorSettings = &openedValue.colorSettings,
        .roundTrip = openedValue.roundTrip.has_value() ? &*openedValue.roundTrip : nullptr,
        .schemaMinor = openedValue.schemaMinor,
    };
    auto resaved = buildVerifiedSaveArchive(resaveManifest, resaveDocumentInput,
                                            SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(resaved),
                        "open minimal: re-saving the opened state through the save chain succeeds");
    if (!resaved) {
        return;
    }
    expectations.expect(sameBytes(archive->bytes(), resaved.archive()->bytes()),
                        "open minimal: the full open->save cycle reproduces the original archive "
                        "byte-for-byte");
}

void testOpenComposedRoundTrip(Expectations& expectations) {
    using namespace bloom::document;
    using bloom::core::Color4d;
    using bloom::core::PixelAspectRatio;
    using bloom::core::RationalTime;

    const auto duration = RationalTime::create(48, 24);
    const auto frameRate = FrameRate::create(24000, 1001);
    const auto pixelAspect = PixelAspectRatio::create(2, 1);
    const auto format =
        CompositionFormat::create(1280, 720, pixelAspect.value_or(PixelAspectRatio::square()),
                                  frameRate.value_or(FrameRate::framesPerSecond24()));
    expectations.expect(duration.has_value() && format.has_value(),
                        "open composed: fixture values are constructible");
    if (!duration.has_value() || !format.has_value()) {
        return;
    }

    CanonicalGraph graph{NodeId::fromRaw(1)};
    const NodeRecord layerOutputNode{
        NodeId::fromRaw(3),
        std::string(kLayerOutputNodeType),
        {{"opacity", ParameterId::fromRaw(3)}, {"position", ParameterId::fromRaw(5)}},
        kLayerOutputNodeSchemaVersion};
    const NodeRecord layerStackNode{
        NodeId::fromRaw(1), std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion};
    const NodeRecord compositionOutputNode{NodeId::fromRaw(4),
                                           std::string(kCompositionOutputNodeType),
                                           {},
                                           kCompositionOutputNodeSchemaVersion};
    const NodeRecord solidSourceNode{NodeId::fromRaw(2),
                                     std::string(kSolidSourceNodeType),
                                     {{"color", ParameterId::fromRaw(7)}},
                                     kSolidSourceNodeSchemaVersion};
    const NodeRecord customNode{NodeId::fromRaw(5), "vendor.nodes.blur", {}, 1};
    const bool nodesAdded = graph.addNode(layerOutputNode) && graph.addNode(layerStackNode) &&
                            graph.addNode(compositionOutputNode) &&
                            graph.addNode(solidSourceNode) && graph.addNode(customNode);
    const EdgeRecord stackToOutputEdge{
        EdgeId::fromRaw(3),
        {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
        NodeInputRef{NodeId::fromRaw(4), std::string(kCompositionOutputInputPort)}};
    const EdgeRecord solidToLayerEdge{
        EdgeId::fromRaw(1),
        {NodeId::fromRaw(2), std::string(kSolidSourceOutputPort)},
        NodeInputRef{NodeId::fromRaw(3), std::string(kLayerOutputContentInputPort)}};
    const EdgeRecord layerToStackEdge{EdgeId::fromRaw(2),
                                      {NodeId::fromRaw(3), std::string(kLayerOutputOutputPort)},
                                      LayerStackInputRef{NodeId::fromRaw(1),
                                                         LayerSlotId::fromRaw(1),
                                                         std::string(kLayerStackContentInputRole)}};
    const bool edgesAdded = graph.addEdge(stackToOutputEdge) && graph.addEdge(solidToLayerEdge) &&
                            graph.addEdge(layerToStackEdge);
    const bool boundariesAdded =
        graph.addLayerOutput({NodeId::fromRaw(3), LayerId::fromRaw(1), "Hero Plate",
                              std::string(kLayerOutputOutputPort)});
    expectations.expect(
        nodesAdded && edgesAdded && boundariesAdded &&
            graph.layerStack().append({LayerSlotId::fromRaw(1), LayerId::fromRaw(1)}),
        "open composed: fixture graph accepts its parts");

    graph.setCompositionOutput({NodeId::fromRaw(4), std::string(kCompositionOutputOutputPort)});
    Composition composition{CompositionId::fromRaw(1), "Hero Shot", *duration, std::move(graph),
                            *format};
    expectations.expect(composition.parameters().insert(
                            {ParameterId::fromRaw(7), std::string(kSolidColorParameterSchemaKey),
                             ConstantValueSource{Color4d{0.0, 0.5, 1.0, 1.0}}}) &&
                            composition.parameters().insert(
                                {ParameterId::fromRaw(5), std::string(kPositionParameterSchemaKey),
                                 ConstantValueSource{Vec2d{96.0, -48.0}}}) &&
                            composition.parameters().insert(
                                {ParameterId::fromRaw(3), std::string(kOpacityParameterSchemaKey),
                                 AnimationCurveSource{AnimationCurveId::fromRaw(9)}}),
                        "open composed: fixture parameters insert");

    ScalarAnimationCurve curve;
    curve.id = AnimationCurveId::fromRaw(9);
    curve.keyframes.push_back(
        {KeyframeId::fromRaw(21), RationalTime{}, 0.25, KeyframeInterpolation::Hold});
    curve.keyframes.push_back({KeyframeId::fromRaw(22), RationalTime::fromInteger(48), 1.0,
                               KeyframeInterpolation::Linear});
    expectations.expect(composition.animationCurves().insert(curve),
                        "open composed: fixture animation curve inserts");

    Project project{ProjectId::fromRaw(1), "Spot Check"};
    const ExtensionRecord markerRecord{
        ExtensionRecordId::fromRaw(1), "vendor.module",
        "vendor.module.marker",        {1, 0},
        CompositionId::fromRaw(1),     "application/octet-stream",
        NoExtensionReferences{},       OpaqueExtensionPayload{std::byte{0x2A}}};
    expectations.expect(project.addComposition(std::move(composition)) &&
                            project.addExtensionRecord(markerRecord),
                        "open composed: fixture composition/extension record add");

    const IdAllocatorHighWater highWater{.composition = 1,
                                         .node = 5,
                                         .edge = 3,
                                         .layer = 1,
                                         .layerSlot = 1,
                                         .parameter = 7,
                                         .animationCurve = 9,
                                         .keyframe = 22,
                                         .driverBinding = 0,
                                         .extensionRecord = 1};
    Document document{std::move(project), highWater};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const std::vector<ManifestRequirement> requirements{
        {.providerId = "vendor.module",
         .capabilityId = "vendor.module.marker-capability",
         .schemaVersion = {1, 0},
         .providedNodeTypeIds = {}},
        {.providerId = "vendor.nodes",
         .capabilityId = "vendor.nodes.blur-capability",
         .schemaVersion = {1, 0},
         .providedNodeTypeIds = {"vendor.nodes.blur"}},
    };
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0},
                                       .requirements = requirements};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};

    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built),
                        "open composed: buildVerifiedSaveArchive succeeds");
    if (!built) {
        return;
    }
    const auto* archive = built.archive();
    expectations.expect(archive != nullptr,
                        "open composed: a successful build exposes its archive");
    if (archive == nullptr) {
        return;
    }

    auto opened = openProjectArchive(archive->bytes(), SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Opened,
                        "open composed: openProjectArchive opens the archive save just wrote");
    if (opened.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto openedValue = std::move(opened).takeOpened();
    expectations.expect(openedValue.document != nullptr, "open composed: Opened owns a Document");
    if (!openedValue.document) {
        return;
    }
    auto openedSnapshot = openedValue.document->snapshot();
    expectations.expect(openedSnapshot.project().name() == "Spot Check",
                        "open composed: project name decodes exactly");
    expectations.expect(openedSnapshot.project().compositions().size() == 1,
                        "open composed: one composition decodes");
    expectations.expect(openedValue.colorSettings == colorSettings,
                        "open composed: colorSettings decode equal to what was saved");
    expectations.expect(openedValue.requirements.size() == requirements.size(),
                        "open composed: both manifest requirements decode");

    const CanonicalManifestV1 resaveManifest{.documentSchemaVersion =
                                                 openedValue.documentSchemaVersion,
                                             .requirements = openedValue.requirements};
    const CanonicalDocumentV1 resaveDocumentInput{
        .snapshot = &openedSnapshot,
        .colorSettings = &openedValue.colorSettings,
        .roundTrip = openedValue.roundTrip.has_value() ? &*openedValue.roundTrip : nullptr,
        .schemaMinor = openedValue.schemaMinor,
    };
    auto resaved = buildVerifiedSaveArchive(resaveManifest, resaveDocumentInput,
                                            SaveArchiveLimits{}, makeOperation());
    expectations.expect(
        static_cast<bool>(resaved),
        "open composed: re-saving the opened state through the save chain succeeds");
    if (!resaved) {
        return;
    }
    expectations.expect(sameBytes(archive->bytes(), resaved.archive()->bytes()),
                        "open composed: the full open->save cycle reproduces the original archive "
                        "byte-for-byte");
}

// A same-major newer-minor (1.1) document with one captured unknown root member, saved through
// the chain, then opened -- proving Open's RoundTripState round-trips both through decode and
// through a subsequent re-save, exactly mirroring save_archive_tests.cpp's own
// testRoundTrippedNewerMinorGreenChain fixture.
void testOpenRoundTrippedNewerMinorRoundTrip(Expectations& expectations) {
    using bloom::project::RoundTripAttachmentPath;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "open RT: fixture duration constructs");
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
    expectations.expect(static_cast<bool>(size), "open RT: baseline document sizes");
    if (!size) {
        return;
    }
    std::string text(*size.value(), '\0');
    const auto written =
        encodeCanonicalDocument(baselineRequest, std::span<char>(text.data(), text.size()));
    expectations.expect(static_cast<bool>(written), "open RT: baseline document encodes");
    if (!written) {
        return;
    }

    const std::string anchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto anchorPos = text.find(anchor);
    expectations.expect(anchorPos != std::string::npos,
                        "open RT: root schemaVersion anchor is located in the baseline");
    if (anchorPos == std::string::npos) {
        return;
    }
    text.replace(anchorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");

    expectations.expect(text.size() >= 2 && text.back() == '\n' && text[text.size() - 2] == '}',
                        "open RT: baseline ends with the root's closing brace and final LF");
    if (text.size() < 2 || text.back() != '\n' || text[text.size() - 2] != '}') {
        return;
    }
    text.resize(text.size() - 2);
    text += R"(,"zzzFutureField":42})";
    text += '\n';

    auto parsed = parseStrictJsonDom(asBytes(text), {}, makeOperation());
    expectations.expect(static_cast<bool>(parsed), "open RT: spliced 1.1 document parses");
    if (!parsed) {
        return;
    }
    DocumentDecodeResult decoded =
        bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(decoded.outcome() == bloom::project::DocumentDecodeOutcome::Decoded &&
                            decoded.value() != nullptr,
                        "open RT: spliced 1.1 document decodes");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        return;
    }
    auto reconstructed = bloom::project::reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed), "open RT: spliced document reconstructs");
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
    expectations.expect(static_cast<bool>(built), "open RT: buildVerifiedSaveArchive succeeds");
    if (!built) {
        return;
    }
    const auto* archive = built.archive();
    expectations.expect(archive != nullptr, "open RT: a successful build exposes its archive");
    if (archive == nullptr) {
        return;
    }

    auto opened = openProjectArchive(archive->bytes(), SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Opened,
                        "open RT: openProjectArchive opens the round-tripped newer-minor archive");
    if (opened.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto openedValue = std::move(opened).takeOpened();
    expectations.expect(openedValue.schemaMinor == 1, "open RT: schemaMinor decodes to 1");
    expectations.expect(openedValue.roundTrip.has_value(),
                        "open RT: RoundTripState is present for the newer-minor document");
    if (!openedValue.roundTrip.has_value()) {
        return;
    }
    const auto* members = openedValue.roundTrip->find(RoundTripAttachmentPath{});
    expectations.expect(members != nullptr && members->size() == 1 &&
                            (*members)[0].key() == "zzzFutureField",
                        "open RT: the retained unknown root member survives Open exactly");

    auto openedSnapshot = openedValue.document->snapshot();
    const CanonicalManifestV1 resaveManifest{.documentSchemaVersion =
                                                 openedValue.documentSchemaVersion,
                                             .requirements = openedValue.requirements};
    const CanonicalDocumentV1 resaveDocumentInput{
        .snapshot = &openedSnapshot,
        .colorSettings = &openedValue.colorSettings,
        .roundTrip = &*openedValue.roundTrip,
        .schemaMinor = openedValue.schemaMinor,
    };
    auto resaved = buildVerifiedSaveArchive(resaveManifest, resaveDocumentInput,
                                            SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(resaved),
                        "open RT: re-saving the opened round-tripped state through the save chain "
                        "succeeds");
    if (!resaved) {
        return;
    }
    expectations.expect(sameBytes(archive->bytes(), resaved.archive()->bytes()),
                        "open RT: the full open->save cycle reproduces the original archive "
                        "byte-for-byte");
}

// ---------------------------------------------------------------------------------------------
// Shared byte-level fixtures for the preserved-read-only and corruption/version tests below.
// ---------------------------------------------------------------------------------------------

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

[[nodiscard]] std::vector<std::byte>
manifestBytesOrAbort(const bloom::document::SchemaVersion documentSchemaVersion) {
    const CanonicalManifestV1 manifest{.documentSchemaVersion = documentSchemaVersion,
                                       .requirements = {}};
    const auto manifestSize = canonicalManifestSize(manifest);
    if (!manifestSize) {
        std::abort();
    }
    std::vector<char> manifestText(*manifestSize.value());
    if (!encodeCanonicalManifest(manifest, manifestText)) {
        std::abort();
    }
    std::vector<std::byte> manifestBytes(manifestText.size());
    std::memcpy(manifestBytes.data(), manifestText.data(), manifestText.size());
    return manifestBytes;
}

// ---------------------------------------------------------------------------------------------
// Preserved-read-only, both sides: a manifest with containerVersion {1,1} over an otherwise valid
// document classifies on the manifest side; a document with an unknown OCIO locator discriminator
// classifies on the document side. Both are distinct from every Failed outcome.
// ---------------------------------------------------------------------------------------------

void testManifestSidePreservedReadOnly(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;

    // Hand-assembled compact manifest JSON (mirrors manifest_decode_tests.cpp's own fixture
    // technique): strict JSON parsing does not require canonical pretty-printing, only
    // encodeCanonicalManifest()'s writer-side comparisons do.
    const std::string manifestText =
        R"({"format":"org.kinetik.bloom.project","containerVersion":{"major":1,"minor":1},)"
        R"("document":{"path":"document.json","schemaVersion":{"major":1,"minor":0}},)"
        R"("requirements":[]})";
    std::vector<std::byte> manifestBytes(manifestText.size());
    std::memcpy(manifestBytes.data(), manifestText.data(), manifestText.size());
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto documentEntry = makeStoredEntry("document.json", documentBytes);
    const auto archive = buildConformingArchive(manifestEntry, documentEntry);

    auto opened = openProjectArchive(archive, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::PreservedReadOnlyRequired,
                        "manifest-side preservation: a same-major newer-minor containerVersion "
                        "classifies PreservedReadOnlyRequired, not Opened or Failed");
    const auto* preservation = opened.preservedReadOnly();
    expectations.expect(preservation != nullptr &&
                            preservation->side == OpenArchivePreservedReadOnlySide::Manifest &&
                            preservation->path.view() == "/containerVersion/minor",
                        "manifest-side preservation: the result names the manifest side and the "
                        "exact diagnostic path");
}

void testDocumentSidePreservedReadOnly(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;

    auto documentBytes = minimalCanonicalDocumentBytesOrAbort();
    std::string text(reinterpret_cast<const char*>(documentBytes.data()), documentBytes.size());

    const std::string minorAnchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto minorPos = text.find(minorAnchor);
    expectations.expect(minorPos != std::string::npos,
                        "document-side preservation: root schemaVersion anchor is located");
    if (minorPos == std::string::npos) {
        return;
    }
    text.replace(minorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");

    const std::string kindAnchor = R"("kind": "builtin")";
    const auto kindPos = text.find(kindAnchor);
    expectations.expect(kindPos != std::string::npos,
                        "document-side preservation: the OCIO locator kind anchor is located");
    if (kindPos == std::string::npos) {
        return;
    }
    text.replace(kindPos, kindAnchor.size(), R"("kind": "future-locator-kind")");

    std::vector<std::byte> splicedDocumentBytes(text.size());
    std::memcpy(splicedDocumentBytes.data(), text.data(), text.size());
    const auto manifestBytes = manifestBytesOrAbort({1, 1});

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto documentEntry = makeStoredEntry("document.json", splicedDocumentBytes);
    const auto archive = buildConformingArchive(manifestEntry, documentEntry);

    auto opened = openProjectArchive(archive, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::PreservedReadOnlyRequired,
                        "document-side preservation: an unknown OCIO locator kind classifies "
                        "PreservedReadOnlyRequired, not Opened or Failed");
    const auto* preservation = opened.preservedReadOnly();
    expectations.expect(
        preservation != nullptr &&
            preservation->side == OpenArchivePreservedReadOnlySide::Document &&
            preservation->documentReason == RoundTripPreservationReason::UnknownDiscriminatorKind &&
            preservation->path.view() == "/project/colorSettings/ocioConfig/locator/kind",
        "document-side preservation: the result names the document side, the exact reason, and "
        "the exact diagnostic path");
}

// ---------------------------------------------------------------------------------------------
// Failures: a representative subset of the corruption corpus, pinned to the same stages Save's
// own verification uses (save_archive_tests.cpp's testCorruptionInjection/testVersionDisagreement/
// testRequirementsCoverageFailure).
// ---------------------------------------------------------------------------------------------

void testContainerCrcFailure(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;

    const auto manifestBytes = manifestBytesOrAbort({1, 0});
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto docEntry = makeStoredEntry("document.json", documentBytes);
    docEntry.localCrc = docEntry.centralCrc ^= 0xFFFFFFFFU;
    const auto archive = buildConformingArchive(manifestEntry, docEntry);

    auto opened = openProjectArchive(archive, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Failed,
                        "open corruption: container-level CRC tamper is rejected");
    const auto* failure = opened.failure();
    expectations.expect(failure != nullptr && failure->stage() == SaveArchiveStage::ContainerRead,
                        "open corruption: CRC tamper is pinned to the ContainerRead stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveContainerReadFailure>();
        expectations.expect(payload != nullptr && payload->error == ZipContainerError::CrcMismatch,
                            "open corruption: CRC tamper reports CrcMismatch");
    }
}

void testJsonSyntaxFailure(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;
    using bloom::project::test::toBytes;

    const auto manifestBytes = manifestBytesOrAbort({1, 0});
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();
    std::string corrupted(reinterpret_cast<const char*>(documentBytes.data()),
                          documentBytes.size());
    expectations.expect(corrupted.size() >= 2 && corrupted[corrupted.size() - 2] == '}',
                        "open corruption: fixture ends with a root closing brace to corrupt");
    if (corrupted.size() < 2 || corrupted[corrupted.size() - 2] != '}') {
        return;
    }
    corrupted[corrupted.size() - 2] = ']'; // mismatched bracket: invalid JSON syntax.

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto docEntry = makeStoredEntry("document.json", toBytes(corrupted));
    const auto archive = buildConformingArchive(manifestEntry, docEntry);

    auto opened = openProjectArchive(archive, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Failed,
                        "open corruption: JSON-level syntax tamper is rejected");
    const auto* failure = opened.failure();
    expectations.expect(failure != nullptr && failure->stage() == SaveArchiveStage::DocumentParse,
                        "open corruption: JSON syntax tamper is pinned to the DocumentParse stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveJsonParseFailure>();
        expectations.expect(payload != nullptr &&
                                payload->error == bloom::project::StrictJsonDomError::InvalidSyntax,
                            "open corruption: JSON syntax tamper reports InvalidSyntax");
    }
}

void testSemanticDecodeFailure(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;
    using bloom::project::test::toBytes;

    const auto manifestBytes = manifestBytesOrAbort({1, 0});
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();
    std::string corrupted(reinterpret_cast<const char*>(documentBytes.data()),
                          documentBytes.size());
    const std::string_view needle = "lin_rec709_scene";
    const auto pos = corrupted.find(needle);
    expectations.expect(pos != std::string::npos,
                        "open corruption: fixture contains the process color space id to corrupt");
    if (pos == std::string::npos) {
        return;
    }
    corrupted.replace(pos, needle.size(), "lin_rec709_scenex");

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto docEntry = makeStoredEntry("document.json", toBytes(corrupted));
    const auto archive = buildConformingArchive(manifestEntry, docEntry);

    auto opened = openProjectArchive(archive, SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Failed,
                        "open corruption: semantic tamper is rejected");
    const auto* failure = opened.failure();
    expectations.expect(failure != nullptr && failure->stage() == SaveArchiveStage::DocumentDecode,
                        "open corruption: semantic tamper is pinned to the DocumentDecode stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveDocumentDecodeFailure>();
        expectations.expect(payload != nullptr &&
                                payload->error ==
                                    bloom::project::DocumentDecodeError::DomainViolation,
                            "open corruption: semantic tamper reports DomainViolation");
    }
}

void testVersionDisagreementFailure(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(),
                        "open version disagreement: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    // manifest declares {1,0}; the document is written with root schemaVersion.minor = 1. No
    // captured-input leg exists for Open (see docs/architecture/project-format.md, "Versions,
    // Migrations, And Preservation"), so only this manifest-vs-document-root disagreement is
    // exercised here.
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{
        .snapshot = &snapshot, .colorSettings = &colorSettings, .schemaMinor = 1};
    auto built = buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built),
                        "open version disagreement: fixture archive builds (unverified)");
    if (!built) {
        return;
    }

    auto opened =
        openProjectArchive(built.archive()->bytes(), SaveArchiveLimits{}, makeOperation());
    expectations.expect(
        opened.outcome() == OpenArchiveOutcome::Failed,
        "open version disagreement: manifest {1,0} vs. document root minor 1 fails");
    const auto* failure = opened.failure();
    expectations.expect(
        failure != nullptr && failure->stage() == SaveArchiveStage::VersionAgreement,
        "open version disagreement: failure is scoped to the VersionAgreement stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveVersionAgreementFailure>();
        expectations.expect(
            payload != nullptr &&
                payload->manifestVersion == bloom::document::SchemaVersion{1, 0} &&
                payload->documentVersion == bloom::document::SchemaVersion{1, 1},
            "open version disagreement: failure names the exact mismatched versions");
    }
}

void testUncoveredRequirementFailure(Expectations& expectations) {
    using bloom::document::CompositionId;
    using bloom::document::NodeId;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(),
                        "open uncovered requirement: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    auto* composition = newProject.project.findComposition(newProject.initialCompositionId);
    expectations.expect(composition != nullptr,
                        "open uncovered requirement: fixture composition is present");
    if (composition == nullptr) {
        return;
    }
    const bool nodeAdded =
        composition->graph().addNode({NodeId::fromRaw(100), "vendor.nodes.blur", {}, 1});
    expectations.expect(nodeAdded, "open uncovered requirement: fixture custom node adds");
    if (!nodeAdded) {
        return;
    }

    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    auto built = buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built),
                        "open uncovered requirement: fixture archive builds (unverified)");
    if (!built) {
        return;
    }

    auto opened =
        openProjectArchive(built.archive()->bytes(), SaveArchiveLimits{}, makeOperation());
    expectations.expect(opened.outcome() == OpenArchiveOutcome::Failed,
                        "open uncovered requirement: an uncovered non-foundation node type fails");
    const auto* failure = opened.failure();
    expectations.expect(
        failure != nullptr && failure->stage() == SaveArchiveStage::RequirementsValidation,
        "open uncovered requirement: failure is scoped to the RequirementsValidation stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveRequirementsFailure>();
        expectations.expect(payload != nullptr && !payload->validation.ok(),
                            "open uncovered requirement: the validation result carries the "
                            "coverage issue");
    }
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion at parse and reconstruction, with a zeroed snapshot once the failed result
// goes out of scope. Uses the same wide bulk fixture save_archive_tests.cpp's own budget probes
// use, so the pipeline's stage-by-stage memory needs are spread widely enough to isolate each
// stage.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> bulkArchiveBytesOrAbort(Expectations& expectations) {
    using bloom::document::ExtensionRecord;
    using bloom::document::ExtensionRecordId;
    using bloom::document::NoExtensionReferences;
    using bloom::document::OpaqueExtensionPayload;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    constexpr std::uint64_t kRecordCount = 4000;
    for (std::uint64_t index = 1; index <= kRecordCount; ++index) {
        ExtensionRecord record{ExtensionRecordId::fromRaw(index),
                               "vendor.bulk",
                               "vendor.bulk.record",
                               {1, 0},
                               std::nullopt,
                               "application/octet-stream",
                               NoExtensionReferences{},
                               OpaqueExtensionPayload{std::byte{0x01}, std::byte{0x02}}};
        if (!newProject.project.addExtensionRecord(std::move(record))) {
            std::abort();
        }
    }
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();
    const std::vector<ManifestRequirement> requirements{{.providerId = "vendor.bulk",
                                                         .capabilityId = "vendor.bulk.cap",
                                                         .schemaVersion = {1, 0},
                                                         .providedNodeTypeIds = {}}};
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0},
                                       .requirements = requirements};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};

    // Built (and verified) under a generous, independent operation: only the subsequent
    // openProjectArchive() call in each probe below runs under a constrained budget.
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built), "open budget exhaustion: bulk fixture builds");
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

template <typename CheckPayload>
void expectOpenResourceExhausted(Expectations& expectations,
                                 const std::vector<std::byte>& archiveBytes,
                                 const std::uint64_t operationBudget,
                                 const SaveArchiveStage expectedStage, CheckPayload&& checkPayload,
                                 const std::string_view message) {
    auto coordinator = ProjectIoMemoryCoordinator::create(64ULL << 20U);
    expectations.expect(coordinator.has_value(), message);
    if (!coordinator.has_value()) {
        return;
    }
    auto operation = coordinator->createOperation(operationBudget, operationBudget);
    expectations.expect(operation.has_value(), message);
    if (!operation.has_value()) {
        return;
    }
    {
        auto opened = openProjectArchive(archiveBytes, SaveArchiveLimits{}, std::move(*operation));
        expectations.expect(opened.outcome() == OpenArchiveOutcome::Failed, message);
        const auto* failure = opened.failure();
        expectations.expect(failure != nullptr && failure->stage() == expectedStage, message);
        if (failure != nullptr) {
            expectations.expect(checkPayload(*failure), message);
        }
    } // `opened` (holding no resident state on failure) is destroyed before the snapshot check.
    const auto snapshot = coordinator->snapshot();
    expectations.expect(snapshot.currentBytes == 0, message);
}

void testBudgetExhaustion(Expectations& expectations) {
    const auto archiveBytes = bulkArchiveBytesOrAbort(expectations);

    // Budgets calibrated empirically against this exact bulk fixture (4000 extension records, one
    // covering manifest requirement): under ~6 MB the shared chain's own ContainerRead working-set
    // reservation dominates (matches save_archive_tests.cpp's own ContainerWrite/ContainerRead
    // note); 6-13 MB isolates DocumentParse; 18-22 MB isolates Reconstruction.
    expectOpenResourceExhausted(
        expectations, archiveBytes, 8'000'000, SaveArchiveStage::DocumentParse,
        [](const auto& failure) {
            const auto* payload = failure.template payloadAs<SaveArchiveJsonParseFailure>();
            return payload != nullptr &&
                   payload->error == bloom::project::StrictJsonDomError::ResourceExhausted;
        },
        "open budget exhaustion: a budget large enough to read the container and parse "
        "manifest.json but too small to parse document.json fails at DocumentParse with a typed "
        "ResourceExhausted and a zeroed snapshot on return");
    expectOpenResourceExhausted(
        expectations, archiveBytes, 20'000'000, SaveArchiveStage::Reconstruction,
        [](const auto& failure) {
            return failure.template payloadAs<SaveArchiveResourceExhausted>() != nullptr;
        },
        "open budget exhaustion: a budget large enough to decode but too small for the "
        "composition layer's own reconstruction-footprint reservation fails at Reconstruction "
        "with SaveArchiveResourceExhausted and a zeroed snapshot on return");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two independent opens of the same archive bytes decode to the same values (proven
// by re-encoding each Opened result and byte-comparing, mirroring
// save_archive_tests.cpp's own testDeterminism oracle).
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "open determinism: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};

    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built), "open determinism: fixture archive builds");
    if (!built) {
        return;
    }
    const auto archiveBytes = built.archive()->bytes();

    auto openedFirst = openProjectArchive(archiveBytes, SaveArchiveLimits{}, makeOperation());
    auto openedSecond = openProjectArchive(archiveBytes, SaveArchiveLimits{}, makeOperation());
    expectations.expect(openedFirst.outcome() == OpenArchiveOutcome::Opened &&
                            openedSecond.outcome() == OpenArchiveOutcome::Opened,
                        "open determinism: both independent opens succeed");
    if (openedFirst.outcome() != OpenArchiveOutcome::Opened ||
        openedSecond.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto first = std::move(openedFirst).takeOpened();
    auto second = std::move(openedSecond).takeOpened();

    auto firstSnapshot = first.document->snapshot();
    auto secondSnapshot = second.document->snapshot();
    std::vector<char> payloadScratch1(64, '\0');
    std::vector<std::size_t> sortScratch1(64, 0);
    std::vector<char> payloadScratch2(64, '\0');
    std::vector<std::size_t> sortScratch2(64, 0);
    const CanonicalDocumentV1 firstRequest{
        .snapshot = &firstSnapshot,
        .colorSettings = &first.colorSettings,
        .payloadScratch = payloadScratch1,
        .sortScratch = sortScratch1,
        .roundTrip = first.roundTrip.has_value() ? &*first.roundTrip : nullptr,
        .schemaMinor = first.schemaMinor};
    const CanonicalDocumentV1 secondRequest{
        .snapshot = &secondSnapshot,
        .colorSettings = &second.colorSettings,
        .payloadScratch = payloadScratch2,
        .sortScratch = sortScratch2,
        .roundTrip = second.roundTrip.has_value() ? &*second.roundTrip : nullptr,
        .schemaMinor = second.schemaMinor};

    const auto firstSize = canonicalDocumentSize(firstRequest);
    const auto secondSize = canonicalDocumentSize(secondRequest);
    expectations.expect(static_cast<bool>(firstSize) && static_cast<bool>(secondSize),
                        "open determinism: both decoded values re-encode");
    if (!firstSize || !secondSize) {
        return;
    }
    std::vector<char> firstText(*firstSize.value());
    std::vector<char> secondText(*secondSize.value());
    expectations.expect(static_cast<bool>(encodeCanonicalDocument(firstRequest, firstText)) &&
                            static_cast<bool>(encodeCanonicalDocument(secondRequest, secondText)),
                        "open determinism: both decoded values re-encode without error");
    expectations.expect(firstText == secondText,
                        "open determinism: two opens of the same archive decode to identical "
                        "values (re-encoded bytes match)");
    expectations.expect(first.colorSettings == second.colorSettings &&
                            first.schemaMinor == second.schemaMinor &&
                            first.requirements == second.requirements &&
                            first.containerVersion == second.containerVersion &&
                            first.documentSchemaVersion == second.documentSchemaVersion,
                        "open determinism: every other decoded field is also identical");
}

} // namespace

// function-try-block: ColorSettings::operator==() recurses into std::variant::operator==(), whose
// standard-library implementation is conservatively flagged as exception-throwing (a
// valueless-by-exception variant, which this test suite never produces) -- see
// color_settings_tests.cpp's identical precedent.
int main() try {
    Expectations expectations;
    testOpenMinimalRoundTrip(expectations);
    testOpenComposedRoundTrip(expectations);
    testOpenRoundTrippedNewerMinorRoundTrip(expectations);
    testManifestSidePreservedReadOnly(expectations);
    testDocumentSidePreservedReadOnly(expectations);
    testContainerCrcFailure(expectations);
    testJsonSyntaxFailure(expectations);
    testSemanticDecodeFailure(expectations);
    testVersionDisagreementFailure(expectations);
    testUncoveredRequirementFailure(expectations);
    testBudgetExhaustion(expectations);
    testDeterminism(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& exception) {
    std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
    return EXIT_FAILURE;
} catch (...) {
    std::cerr << "FAILED: unexpected non-standard exception\n";
    return EXIT_FAILURE;
}
