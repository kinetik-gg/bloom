#include <bloom/project/save_archive.hpp>

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
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include "zip_container_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

using bloom::project::buildVerifiedSaveArchive;
using bloom::project::canonicalDocumentSize;
using bloom::project::CanonicalDocumentV1;
using bloom::project::canonicalManifestSize;
using bloom::project::CanonicalManifestV1;
using bloom::project::decodeDocumentEnvelope;
using bloom::project::DocumentClassification;
using bloom::project::DocumentDecodeOutcome;
using bloom::project::encodeCanonicalDocument;
using bloom::project::encodeCanonicalManifest;
using bloom::project::ManifestRequirement;
using bloom::project::parseStrictJsonDom;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::readZipContainer;
using bloom::project::reconstructDocument;
using bloom::project::SaveArchiveContainerReadFailure;
using bloom::project::SaveArchiveContainerWriteFailure;
using bloom::project::SaveArchiveDocumentDecodeFailure;
using bloom::project::SaveArchiveDocumentEncodingFailure;
using bloom::project::SaveArchiveExpectedContent;
using bloom::project::SaveArchiveJsonParseFailure;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveManifestEncodingFailure;
using bloom::project::SaveArchiveRequirementsFailure;
using bloom::project::SaveArchiveResourceExhausted;
using bloom::project::SaveArchiveResult;
using bloom::project::SaveArchiveStage;
using bloom::project::SaveArchiveVerificationResult;
using bloom::project::SaveArchiveVersionAgreementFailure;
using bloom::project::verifySaveArchive;
using bloom::project::ZipContainerError;
using bloom::project::ZipContainerLimits;
using bloom::project::ZipContainerWriteError;

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
// Shared plumbing
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

// ---------------------------------------------------------------------------------------------
// Green chain -- minimal: an empty-requirements manifest and a minimal valid document. The
// produced archive is independently reopened and byte-compared against canonical bytes derived
// straight from the same inputs (never through save_archive itself), so this test also exercises
// the plain canonical_manifest.hpp/canonical_document.hpp writers as an independent oracle.
// ---------------------------------------------------------------------------------------------

void testMinimalGreenChain(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "minimal green chain: fixture duration constructs");
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
                        "minimal green chain: buildVerifiedSaveArchive succeeds");
    if (!built) {
        return;
    }
    const auto* archive = built.archive();
    expectations.expect(archive != nullptr,
                        "minimal green chain: a successful result exposes its archive");
    if (archive == nullptr) {
        return;
    }

    // Independent reopen: readZipContainer() over the produced bytes, entirely outside
    // save_archive.
    auto reopened = readZipContainer(archive->bytes(), ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reopened),
                        "minimal green chain: the produced archive independently reopens through "
                        "readZipContainer");
    if (!reopened) {
        return;
    }

    // Independent decode: parseStrictJsonDom + decodeManifestEnvelope/decodeDocumentEnvelope +
    // reconstructDocument, entirely outside save_archive's own verification.
    auto manifestDom =
        parseStrictJsonDom(reopened.document()->manifestBytes(), {}, makeOperation());
    auto documentDom =
        parseStrictJsonDom(reopened.document()->documentBytes(), {}, makeOperation());
    expectations.expect(static_cast<bool>(manifestDom) && static_cast<bool>(documentDom),
                        "minimal green chain: reopened entries independently parse as strict JSON");
    if (!manifestDom || !documentDom) {
        return;
    }
    auto decodedManifest = bloom::project::decodeManifestEnvelope(manifestDom.document()->root());
    auto decodedDocument = decodeDocumentEnvelope(documentDom.document()->root());
    expectations.expect(static_cast<bool>(decodedManifest) && static_cast<bool>(decodedDocument),
                        "minimal green chain: reopened entries independently decode");
    if (!decodedDocument.value()) {
        return;
    }
    auto reconstructed = reconstructDocument(*decodedDocument.value());
    expectations.expect(static_cast<bool>(reconstructed),
                        "minimal green chain: the independently decoded document independently "
                        "reconstructs");

    // Byte-exact comparison against canonical bytes derived directly from the same inputs.
    const auto manifestSize = canonicalManifestSize(manifest);
    expectations.expect(static_cast<bool>(manifestSize),
                        "minimal green chain: oracle manifest sizes");
    if (!manifestSize) {
        return;
    }
    std::vector<char> expectedManifest(*manifestSize.value());
    const auto manifestWritten = encodeCanonicalManifest(manifest, expectedManifest);
    expectations.expect(static_cast<bool>(manifestWritten),
                        "minimal green chain: oracle manifest encodes");

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    CanonicalDocumentV1 oracleRequest = documentInput;
    oracleRequest.payloadScratch = payloadScratch;
    oracleRequest.sortScratch = sortScratch;
    const auto documentSize = canonicalDocumentSize(oracleRequest);
    expectations.expect(static_cast<bool>(documentSize),
                        "minimal green chain: oracle document sizes");
    if (!documentSize) {
        return;
    }
    std::vector<char> expectedDocument(*documentSize.value());
    const auto documentWritten = encodeCanonicalDocument(oracleRequest, expectedDocument);
    expectations.expect(static_cast<bool>(documentWritten),
                        "minimal green chain: oracle document encodes");

    const auto manifestOut = reopened.document()->manifestBytes();
    const auto documentOut = reopened.document()->documentBytes();
    expectations.expect(
        manifestOut.size() == expectedManifest.size() &&
            std::memcmp(manifestOut.data(), expectedManifest.data(), manifestOut.size()) == 0,
        "minimal green chain: reopened manifest.json is byte-exact against the independent oracle");
    expectations.expect(
        documentOut.size() == expectedDocument.size() &&
            std::memcmp(documentOut.data(), expectedDocument.data(), documentOut.size()) == 0,
        "minimal green chain: reopened document.json is byte-exact against the independent oracle");
}

// ---------------------------------------------------------------------------------------------
// Green chain -- composed: color settings, parameters, an animation curve, a graph with a layer
// stack, one non-foundation node type, and one extension record -- with manifest requirements
// covering both the custom node type and the extension owner. Mirrors
// document_reconstruct_tests.cpp's testComposedDeterminismRoundTrip fixture (already proven to
// reconstruct) plus the addCustomNode/extension patterns from manifest_requirements_tests.cpp and
// document_reconstruct_tests.cpp's testExtensionDeterminismRoundTrip.
// ---------------------------------------------------------------------------------------------

void testComposedGreenChain(Expectations& expectations) {
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
                        "composed green chain: fixture values are constructible");
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
    // The one non-foundation node type this fixture's manifest requirements must cover -- left
    // structurally unconnected (no edges), exactly like manifest_requirements_tests.cpp's
    // addCustomNode helper: CanonicalGraph::addNode() does not require reachability.
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
        "composed green chain: fixture graph accepts its parts");

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
                        "composed green chain: fixture parameters insert");

    ScalarAnimationCurve curve;
    curve.id = AnimationCurveId::fromRaw(9);
    curve.keyframes.push_back(
        {KeyframeId::fromRaw(21), RationalTime{}, 0.25, KeyframeInterpolation::Hold});
    curve.keyframes.push_back({KeyframeId::fromRaw(22), RationalTime::fromInteger(48), 1.0,
                               KeyframeInterpolation::Linear});
    expectations.expect(composition.animationCurves().insert(curve),
                        "composed green chain: fixture animation curve inserts");

    Project project{ProjectId::fromRaw(1), "Spot Check"};
    const ExtensionRecord markerRecord{
        ExtensionRecordId::fromRaw(1), "vendor.module",
        "vendor.module.marker",        {1, 0},
        CompositionId::fromRaw(1),     "application/octet-stream",
        NoExtensionReferences{},       OpaqueExtensionPayload{std::byte{0x2A}}};
    expectations.expect(project.addComposition(std::move(composition)) &&
                            project.addExtensionRecord(markerRecord),
                        "composed green chain: fixture composition/extension record add");

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
                        "composed green chain: buildVerifiedSaveArchive succeeds for a project "
                        "with color settings, parameters, an animation curve, a layer-stack "
                        "graph, a non-foundation node type, and an extension record");
}

// ---------------------------------------------------------------------------------------------
// Green chain -- round-tripped newer minor: a document decoded from a same-major newer-minor
// (1.1) archive with one captured unknown root member, saved back through the chain with
// schemaMinor = 1 and a manifest declaring {1,1}. Verifies green and the retained-data proof
// (decision 2.6: reopening our own just-written archive through the document side's RT capture
// path must come back editable with RoundTripState, not PreservedReadOnlyRequired).
// ---------------------------------------------------------------------------------------------

void testRoundTrippedNewerMinorGreenChain(Expectations& expectations) {
    using bloom::project::DocumentDecodeResult;
    using bloom::project::RoundTripAttachmentPath;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "RT green chain: fixture duration constructs");
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
    expectations.expect(static_cast<bool>(size), "RT green chain: baseline document sizes");
    if (!size) {
        return;
    }
    std::string text(*size.value(), '\0');
    const auto written =
        encodeCanonicalDocument(baselineRequest, std::span<char>(text.data(), text.size()));
    expectations.expect(static_cast<bool>(written), "RT green chain: baseline document encodes");
    if (!written) {
        return;
    }

    // Bump the root schemaVersion.minor from 0 to 1 (the anchor text uniquely identifies the
    // root's own schemaVersion object, which the canonical writer always emits first, ahead of
    // "project").
    const std::string anchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto anchorPos = text.find(anchor);
    expectations.expect(anchorPos != std::string::npos,
                        "RT green chain: root schemaVersion anchor is located in the baseline");
    if (anchorPos == std::string::npos) {
        return;
    }
    text.replace(anchorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");

    // Splice one unknown trailing root member -- a conforming 1.1 writer could have produced
    // this (strictly after every known root member, in ascending UTF-8 key order; there is only
    // one, so ordering is trivially satisfied).
    expectations.expect(text.size() >= 2 && text.back() == '\n' && text[text.size() - 2] == '}',
                        "RT green chain: baseline ends with the root's closing brace and final LF");
    if (text.size() < 2 || text.back() != '\n' || text[text.size() - 2] != '}') {
        return;
    }
    text.resize(text.size() - 2);
    text += R"(,"zzzFutureField":42})";
    text += '\n';

    auto parsed = parseStrictJsonDom(asBytes(text), {}, makeOperation());
    expectations.expect(static_cast<bool>(parsed), "RT green chain: spliced 1.1 document parses");
    if (!parsed) {
        return;
    }
    DocumentDecodeResult decoded = decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            decoded.value() != nullptr,
                        "RT green chain: spliced 1.1 document decodes");
    expectations.expect(decoded.classification() == DocumentClassification::EditableWithRoundTrip,
                        "RT green chain: spliced document classifies EditableWithRoundTrip");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        return;
    }
    expectations.expect(decoded.roundTrip()->size() == 1,
                        "RT green chain: exactly one attachment point (the root) was captured");

    auto reconstructed = reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed),
                        "RT green chain: spliced document reconstructs");
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
    expectations.expect(static_cast<bool>(built),
                        "RT green chain: buildVerifiedSaveArchive succeeds for a round-tripped "
                        "newer-minor document (the editable-reopen boundary tolerates our own "
                        "schemaMinor > 0 archive, per decision 2.6)");
    if (!built) {
        return;
    }

    // Retained-data proof: the produced archive, reopened completely independently, still
    // classifies EditableWithRoundTrip (not PreservedReadOnlyRequired) and still names the exact
    // retained unknown root member.
    auto reopened =
        readZipContainer(built.archive()->bytes(), ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(reopened),
                        "RT green chain: the produced archive reopens");
    if (!reopened) {
        return;
    }
    auto documentDom =
        parseStrictJsonDom(reopened.document()->documentBytes(), {}, makeOperation());
    expectations.expect(static_cast<bool>(documentDom),
                        "RT green chain: reopened document.json parses");
    if (!documentDom) {
        return;
    }
    DocumentDecodeResult redecoded = decodeDocumentEnvelope(documentDom.document()->root());
    expectations.expect(redecoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            redecoded.classification() ==
                                DocumentClassification::EditableWithRoundTrip,
                        "RT green chain: the reopened archive still classifies "
                        "EditableWithRoundTrip, never PreservedReadOnlyRequired");
    if (redecoded.roundTrip() == nullptr) {
        expectations.expect(false, "RT green chain: the reopened archive retains RoundTripState");
        return;
    }
    const auto* members = redecoded.roundTrip()->find(RoundTripAttachmentPath{});
    expectations.expect(members != nullptr && members->size() == 1 &&
                            (*members)[0].key() == "zzzFutureField",
                        "RT green chain: the unknown root member survives the save/reopen chain "
                        "byte-exactly");
}

// ---------------------------------------------------------------------------------------------
// Version disagreement: manifest {1,0} vs. a document written at minor 1, and vice versa.
// ---------------------------------------------------------------------------------------------

void testVersionDisagreement(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "version disagreement: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    {
        const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
        const CanonicalDocumentV1 documentInput{
            .snapshot = &snapshot, .colorSettings = &colorSettings, .schemaMinor = 1};
        auto built =
            buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
        expectations.expect(!built,
                            "version disagreement: manifest {1,0} vs. document minor 1 fails");
        const auto* failure = built.failure();
        expectations.expect(
            failure != nullptr && failure->stage() == SaveArchiveStage::VersionAgreement,
            "version disagreement: failure is scoped to the VersionAgreement stage");
        if (failure != nullptr) {
            const auto* payload = failure->payloadAs<SaveArchiveVersionAgreementFailure>();
            expectations.expect(
                payload != nullptr &&
                    payload->manifestVersion == bloom::document::SchemaVersion{1, 0} &&
                    payload->documentVersion == bloom::document::SchemaVersion{1, 1},
                "version disagreement: failure names the exact mismatched versions");
        }
    }
    {
        const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 1}, .requirements = {}};
        const CanonicalDocumentV1 documentInput{
            .snapshot = &snapshot, .colorSettings = &colorSettings, .schemaMinor = 0};
        auto built =
            buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
        expectations.expect(!built,
                            "version disagreement (reversed): manifest {1,1} vs. document minor 0 "
                            "fails");
        const auto* failure = built.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == SaveArchiveStage::VersionAgreement,
                            "version disagreement (reversed): failure is scoped to the "
                            "VersionAgreement stage");
    }
}

// ---------------------------------------------------------------------------------------------
// Requirements failure: a non-foundation node type present in the document but uncovered by the
// manifest requirements surfaces the coverage failure through the chain.
// ---------------------------------------------------------------------------------------------

void testRequirementsCoverageFailure(Expectations& expectations) {
    using bloom::document::CompositionId;
    using bloom::document::NodeId;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "requirements failure: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    auto* composition = newProject.project.findComposition(newProject.initialCompositionId);
    expectations.expect(composition != nullptr,
                        "requirements failure: fixture composition is present");
    if (composition == nullptr) {
        return;
    }
    const bool nodeAdded =
        composition->graph().addNode({NodeId::fromRaw(100), "vendor.nodes.blur", {}, 1});
    expectations.expect(nodeAdded, "requirements failure: fixture custom node adds");
    if (!nodeAdded) {
        return;
    }

    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    // No requirement covers "vendor.nodes.blur".
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(!built,
                        "requirements failure: an uncovered non-foundation node type fails the "
                        "chain");
    const auto* failure = built.failure();
    expectations.expect(
        failure != nullptr && failure->stage() == SaveArchiveStage::RequirementsValidation,
        "requirements failure: failure is scoped to the RequirementsValidation stage");
    if (failure != nullptr) {
        const auto* payload = failure->payloadAs<SaveArchiveRequirementsFailure>();
        expectations.expect(payload != nullptr && !payload->validation.ok(),
                            "requirements failure: the validation result carries the coverage "
                            "issue");
    }
}

// ---------------------------------------------------------------------------------------------
// Corruption injection: container-level (CRC), JSON-level (invalid syntax), and semantic
// (valid JSON, wrong value) tampering of an otherwise-valid archive, each pinned to its
// stage-scoped typed failure. Uses verifySaveArchive() directly against hand-tampered bytes --
// exactly the entry point a future real Open will reuse for this purpose.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::pair<std::vector<std::byte>, std::vector<std::byte>>
minimalCanonicalBytesOrAbort() {
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

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
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
    std::vector<std::byte> documentBytes(documentText.size());
    std::memcpy(documentBytes.data(), documentText.data(), documentText.size());
    return {std::move(manifestBytes), std::move(documentBytes)};
}

void testCorruptionInjection(Expectations& expectations) {
    using bloom::project::test::buildConformingArchive;
    using bloom::project::test::makeStoredEntry;
    using bloom::project::test::toBytes;

    const auto [manifestBytes, documentBytes] = minimalCanonicalBytesOrAbort();
    const std::string_view documentText(reinterpret_cast<const char*>(documentBytes.data()),
                                        documentBytes.size());
    const SaveArchiveExpectedContent expected{.manifestBytes = manifestBytes,
                                              .documentBytes = documentBytes,
                                              .documentSchemaVersion = {1, 0}};

    // Container-level tamper: flip the document entry's declared CRC. Structure stays a
    // conforming archive; the reader's own independent CRC-32 recomputation disagrees before any
    // JSON parsing happens.
    {
        auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
        auto docEntry = makeStoredEntry("document.json", documentBytes);
        docEntry.localCrc = docEntry.centralCrc ^= 0xFFFFFFFFU;
        const auto archive = buildConformingArchive(manifestEntry, docEntry);
        auto result = verifySaveArchive(archive, expected, SaveArchiveLimits{}, makeOperation());
        expectations.expect(!result, "corruption: container-level CRC tamper is rejected");
        const auto* failure = result.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == SaveArchiveStage::ContainerRead,
                            "corruption: CRC tamper is pinned to the ContainerRead stage");
        if (failure != nullptr) {
            const auto* payload = failure->payloadAs<SaveArchiveContainerReadFailure>();
            expectations.expect(payload != nullptr &&
                                    payload->error == ZipContainerError::CrcMismatch,
                                "corruption: CRC tamper reports CrcMismatch");
        }
    }

    // JSON-level tamper: corrupt document.json into syntactically invalid JSON, with the entry's
    // CRC recomputed to match the corrupted bytes (so the container layer accepts it and only
    // the JSON parser catches the corruption).
    {
        std::string corrupted(documentText);
        expectations.expect(corrupted.size() >= 2 && corrupted[corrupted.size() - 2] == '}',
                            "corruption: fixture ends with a root closing brace to corrupt");
        corrupted[corrupted.size() - 2] = ']'; // mismatched bracket: invalid JSON syntax.
        auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
        auto docEntry = makeStoredEntry("document.json", toBytes(corrupted));
        const auto archive = buildConformingArchive(manifestEntry, docEntry);
        auto result = verifySaveArchive(archive, expected, SaveArchiveLimits{}, makeOperation());
        expectations.expect(!result, "corruption: JSON-level syntax tamper is rejected");
        const auto* failure = result.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == SaveArchiveStage::DocumentParse,
                            "corruption: JSON syntax tamper is pinned to the DocumentParse stage");
        if (failure != nullptr) {
            const auto* payload = failure->payloadAs<SaveArchiveJsonParseFailure>();
            expectations.expect(payload != nullptr &&
                                    payload->error ==
                                        bloom::project::StrictJsonDomError::InvalidSyntax,
                                "corruption: JSON syntax tamper reports InvalidSyntax");
        }
    }

    // Semantic tamper: valid JSON, but a value that breaks typed decode (the fixed v1 process
    // color space identity).
    {
        std::string corrupted(documentText);
        const std::string_view needle = "lin_rec709_scene";
        const auto pos = corrupted.find(needle);
        expectations.expect(pos != std::string::npos,
                            "corruption: fixture contains the process color space id to corrupt");
        if (pos == std::string::npos) {
            return;
        }
        corrupted.replace(pos, needle.size(), "lin_rec709_scenex");
        auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
        auto docEntry = makeStoredEntry("document.json", toBytes(corrupted));
        const auto archive = buildConformingArchive(manifestEntry, docEntry);
        auto result = verifySaveArchive(archive, expected, SaveArchiveLimits{}, makeOperation());
        expectations.expect(!result, "corruption: semantic tamper is rejected");
        const auto* failure = result.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == SaveArchiveStage::DocumentDecode,
                            "corruption: semantic tamper is pinned to the DocumentDecode stage");
        if (failure != nullptr) {
            const auto* payload = failure->payloadAs<SaveArchiveDocumentDecodeFailure>();
            expectations.expect(payload != nullptr &&
                                    payload->error ==
                                        bloom::project::DocumentDecodeError::DomainViolation,
                                "corruption: semantic tamper reports DomainViolation");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion: operation budgets sized to fail at encode, container write, parse, and
// reconstruct, each reporting typed ResourceExhausted and returning the shared coordinator
// snapshot to zero.
// ---------------------------------------------------------------------------------------------

// A larger fixture (many extension records, all owned by one provider so a single manifest
// requirement covers them) is needed to spread the pipeline's stage-by-stage memory needs across
// a wide enough range of budgets to isolate ManifestEncode, ContainerWrite, DocumentParse, and
// Reconstruction as four *distinct* failure points. A minimal document does not: the container
// writer's and reader's own documented worst-case qualified-dependency working-memory reservation
// (a merged-module constant, 4 MiB each, held only transiently within its own stage -- see
// zip_container_writer.cpp/zip_container.cpp) dominates a minimal fixture's entire budget curve,
// so every stage past ContainerWrite succeeds together the moment that transient 4 MiB clears.
void withBulkDocumentInput(
    Expectations& expectations,
    const std::function<void(const CanonicalManifestV1&, const CanonicalDocumentV1&)>& use) {
    using bloom::document::ExtensionRecord;
    using bloom::document::ExtensionRecordId;
    using bloom::document::NoExtensionReferences;
    using bloom::document::OpaqueExtensionPayload;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "budget exhaustion: fixture duration constructs");
    if (!duration.has_value()) {
        return;
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
        const bool added = newProject.project.addExtensionRecord(std::move(record));
        expectations.expect(added, "budget exhaustion: fixture extension record adds");
        if (!added) {
            return;
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
    use(manifest, documentInput);
}

// Runs one budget-exhaustion probe and checks: the call fails, the failure is scoped to
// `expectedStage`, the failure payload is the specific typed ResourceExhausted shape that stage
// reports (never collapsed to a generic catch-all), and the shared coordinator's snapshot returns
// to zero once `built` (which, on failure, owns no archive) goes out of scope.
template <typename CheckPayload>
void expectResourceExhausted(Expectations& expectations, const std::uint64_t operationBudget,
                             const SaveArchiveStage expectedStage, CheckPayload&& checkPayload,
                             const std::string_view message) {
    withBulkDocumentInput(expectations, [&](const CanonicalManifestV1& manifest,
                                            const CanonicalDocumentV1& documentInput) {
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
            auto built = buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{},
                                                  std::move(*operation));
            expectations.expect(!built, message);
            const auto* failure = built.failure();
            expectations.expect(failure != nullptr && failure->stage() == expectedStage, message);
            if (failure != nullptr) {
                expectations.expect(checkPayload(*failure), message);
            }
        } // `built` (holding no archive on failure) is destroyed before the snapshot check below.
        const auto snapshot = coordinator->snapshot();
        expectations.expect(snapshot.currentBytes == 0, message);
    });
}

void testBudgetExhaustion(Expectations& expectations) {
    expectResourceExhausted(
        expectations, 64, SaveArchiveStage::ManifestEncode,
        [](const auto& failure) {
            return failure.template payloadAs<SaveArchiveResourceExhausted>() != nullptr;
        },
        "budget exhaustion: a budget too small for even the manifest buffer fails at "
        "ManifestEncode with SaveArchiveResourceExhausted and a zeroed snapshot on return");
    expectResourceExhausted(
        expectations, 5'000'000, SaveArchiveStage::ContainerWrite,
        [](const auto& failure) {
            const auto* payload = failure.template payloadAs<SaveArchiveContainerWriteFailure>();
            return payload != nullptr &&
                   payload->error == ZipContainerWriteError::ResourceExhausted;
        },
        "budget exhaustion: a budget large enough to encode but too small to clear the archive "
        "writer's own qualified-compressor working-memory reservation fails at ContainerWrite "
        "with a typed ResourceExhausted and a zeroed snapshot on return");
    expectResourceExhausted(
        expectations, 10'000'000, SaveArchiveStage::DocumentParse,
        [](const auto& failure) {
            const auto* payload = failure.template payloadAs<SaveArchiveJsonParseFailure>();
            return payload != nullptr &&
                   payload->error == bloom::project::StrictJsonDomError::ResourceExhausted;
        },
        "budget exhaustion: a budget large enough to write and reopen the archive but too small "
        "to parse document.json fails at DocumentParse with a typed ResourceExhausted and a "
        "zeroed snapshot on return");
    expectResourceExhausted(
        expectations, 20'000'000, SaveArchiveStage::Reconstruction,
        [](const auto& failure) {
            return failure.template payloadAs<SaveArchiveResourceExhausted>() != nullptr;
        },
        "budget exhaustion: a budget large enough to decode but too small for the composition "
        "layer's own reconstruction-footprint reservation fails at Reconstruction with "
        "SaveArchiveResourceExhausted and a zeroed snapshot on return");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two independent runs over semantically identical input produce byte-identical
// archives.
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "determinism: fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};

    auto newProject1 =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document1{std::move(newProject1.project)};
    auto snapshot1 = document1.snapshot();
    const auto colorSettings1 = neutralColorSettings();
    const CanonicalDocumentV1 documentInput1{.snapshot = &snapshot1,
                                             .colorSettings = &colorSettings1};
    auto built1 =
        buildVerifiedSaveArchive(manifest, documentInput1, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built1), "determinism: first run succeeds");
    if (!built1) {
        return;
    }

    auto newProject2 =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document2{std::move(newProject2.project)};
    auto snapshot2 = document2.snapshot();
    const auto colorSettings2 = neutralColorSettings();
    const CanonicalDocumentV1 documentInput2{.snapshot = &snapshot2,
                                             .colorSettings = &colorSettings2};
    auto built2 =
        buildVerifiedSaveArchive(manifest, documentInput2, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built2), "determinism: second run succeeds");
    if (!built2) {
        return;
    }

    const auto bytes1 = built1.archive()->bytes();
    const auto bytes2 = built2.archive()->bytes();
    expectations.expect(bytes1.size() == bytes2.size() &&
                            std::memcmp(bytes1.data(), bytes2.data(), bytes1.size()) == 0,
                        "determinism: two runs over semantically identical input produce "
                        "byte-identical archives");
}

} // namespace

int main() {
    Expectations expectations;
    testMinimalGreenChain(expectations);
    testComposedGreenChain(expectations);
    testRoundTrippedNewerMinorGreenChain(expectations);
    testVersionDisagreement(expectations);
    testRequirementsCoverageFailure(expectations);
    testCorruptionInjection(expectations);
    testBudgetExhaustion(expectations);
    testDeterminism(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
