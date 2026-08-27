#include <bloom/project/document_reconstruct.hpp>

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
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::project::DecodedDocumentEnvelope;
using bloom::project::DocumentDecodeError;
using bloom::project::DocumentDecodeResult;
using bloom::project::JsonValue;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::ReconstructDocumentResult;
using bloom::project::ReconstructionRejected;
using bloom::project::ReconstructionStage;

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

constexpr std::uint64_t kGenerousOperationBudget = 8ULL << 20U; // 8 MiB: ample for every fixture.

[[nodiscard]] ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
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
// Shared round-trip plumbing: encode a live snapshot, parse+decode the bytes back, and expose the
// pieces so each fixture-specific test can drive reconstructDocument() and compare.
// ---------------------------------------------------------------------------------------------

struct EncodedDocument final {
    bool ok = false;
    std::string bytes;
};

[[nodiscard]] EncodedDocument encodeSnapshot(const bloom::document::Snapshot& snapshot,
                                             const bloom::document::ColorSettings& colorSettings) {
    EncodedDocument encoded;
    std::vector<char> payloadScratch(1024, '\0');
    std::vector<std::size_t> sortScratch(1024, 0);
    const bloom::project::CanonicalDocumentV1 request{
        .snapshot = &snapshot,
        .colorSettings = &colorSettings,
        .payloadScratch = payloadScratch,
        .sortScratch = sortScratch,
    };
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        return encoded;
    }
    std::vector<char> output(*size.value());
    const auto written = bloom::project::encodeCanonicalDocument(request, output);
    if (!written) {
        return encoded;
    }
    encoded.ok = true;
    encoded.bytes.assign(output.data(), output.size());
    return encoded;
}

[[nodiscard]] DocumentDecodeResult decodeText(const std::string& text) {
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(text), {},
                                                     makeOperation(kGenerousOperationBudget));
    if (!parsed) {
        std::cerr << "fixture failed to parse as strict JSON (error="
                  << static_cast<int>(parsed.error()) << ")\n";
        std::abort();
    }
    return bloom::project::decodeDocumentEnvelope(parsed.document()->root());
}

// Runs the complete determinism round trip for one already-constructed source document: encode,
// parse, decode, reconstruct, re-encode, and assert byte equality plus high-water equality. Returns
// early (after recording a failure) on the first broken link so later assertions never dereference
// a null/absent value.
void expectDeterminismRoundTrip(Expectations& expectations, const bloom::document::Document& source,
                                const bloom::document::ColorSettings& colorSettings,
                                const std::string_view label) {
    auto sourceSnapshot = source.snapshot();
    const auto firstEncoded = encodeSnapshot(sourceSnapshot, colorSettings);
    expectations.expect(firstEncoded.ok, label);
    if (!firstEncoded.ok) {
        return;
    }

    auto parsed = bloom::project::parseStrictJsonDom(asBytes(firstEncoded.bytes), {},
                                                     makeOperation(kGenerousOperationBudget));
    expectations.expect(static_cast<bool>(parsed), label);
    if (!parsed) {
        return;
    }

    const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr, label);
    if (decoded.value() == nullptr) {
        return;
    }

    auto reconstructed = bloom::project::reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed), label);
    if (!reconstructed) {
        return;
    }

    auto* reconstructedValue = reconstructed.value();
    expectations.expect(reconstructedValue != nullptr && reconstructedValue->document != nullptr,
                        label);
    if (reconstructedValue == nullptr || reconstructedValue->document == nullptr) {
        return;
    }
    auto reconstructedSnapshot = reconstructedValue->document->snapshot();
    expectations.expect(reconstructedSnapshot.ids().highWater() == sourceSnapshot.ids().highWater(),
                        label);

    const auto secondEncoded =
        encodeSnapshot(reconstructedSnapshot, reconstructedValue->colorSettings);
    expectations.expect(secondEncoded.ok, label);
    if (!secondEncoded.ok) {
        return;
    }
    expectations.expect(firstEncoded.bytes == secondEncoded.bytes, label);
}

// ---------------------------------------------------------------------------------------------
// THE determinism round trip -- minimal fixture.
// ---------------------------------------------------------------------------------------------

void testMinimalDeterminismRoundTrip(Expectations& expectations) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        expectations.expect(false, "minimal round trip: fixture duration is constructible");
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    const auto settings = neutralColorSettings();

    expectDeterminismRoundTrip(expectations, document, settings,
                               "minimal fixture: determinism round trip (encode/decode/"
                               "reconstruct/re-encode/highWater) holds at every step");
}

// ---------------------------------------------------------------------------------------------
// THE determinism round trip -- composed fixture (mirrors canonical_document_tests.cpp's
// testComposedGoldenBytes: solid layer, one animated scalar opacity curve, one constant vec2
// position, one constant color4).
// ---------------------------------------------------------------------------------------------

void testComposedDeterminismRoundTrip(Expectations& expectations) {
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
    if (!duration.has_value() || !format.has_value()) {
        expectations.expect(false, "composed round trip: fixture values are constructible");
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
    const bool nodesAdded = graph.addNode(layerOutputNode) && graph.addNode(layerStackNode) &&
                            graph.addNode(compositionOutputNode) && graph.addNode(solidSourceNode);
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
        "composed round trip: fixture graph accepts its parts");

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
                        "composed round trip: fixture parameters insert");

    ScalarAnimationCurve curve;
    curve.id = AnimationCurveId::fromRaw(9);
    curve.keyframes.push_back(
        {KeyframeId::fromRaw(21), RationalTime{}, 0.25, KeyframeInterpolation::Hold});
    curve.keyframes.push_back({KeyframeId::fromRaw(22), RationalTime::fromInteger(48), 1.0,
                               KeyframeInterpolation::Linear});
    expectations.expect(composition.animationCurves().insert(curve),
                        "composed round trip: fixture animation curve inserts");

    Project project{ProjectId::fromRaw(1), "Spot Check"};
    expectations.expect(project.addComposition(std::move(composition)),
                        "composed round trip: fixture composition adds");

    const IdAllocatorHighWater highWater{.composition = 1,
                                         .node = 4,
                                         .edge = 3,
                                         .layer = 1,
                                         .layerSlot = 1,
                                         .parameter = 7,
                                         .animationCurve = 9,
                                         .keyframe = 22,
                                         .driverBinding = 0,
                                         .extensionRecord = 0};
    Document document{std::move(project), highWater};
    const auto settings = neutralColorSettings();

    expectDeterminismRoundTrip(expectations, document, settings,
                               "composed fixture: determinism round trip (encode/decode/"
                               "reconstruct/re-encode/highWater) holds at every step");
}

// ---------------------------------------------------------------------------------------------
// Extension round trip -- all three reference policies plus a typed-subject record (mirrors
// canonical_document_tests.cpp's testExtensionGoldenBytes fixture).
// ---------------------------------------------------------------------------------------------

void testExtensionDeterminismRoundTrip(Expectations& expectations) {
    using namespace bloom::document;
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        expectations.expect(false, "extension round trip: fixture duration is constructible");
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);

    const ExtensionRecord linkTableRecord{
        ExtensionRecordId::fromRaw(9),
        "vendor.module",
        "vendor.module.link-table",
        {1, 0},
        std::nullopt,
        "application/x-vendor-table",
        ExtensionHostReferenceTable{{{std::string("primary-comp"), CompositionId::fromRaw(1)}}},
        OpaqueExtensionPayload{}};
    const ExtensionRecord markerRecord{
        ExtensionRecordId::fromRaw(5), "vendor.module",
        "vendor.module.marker",        {1, 0},
        CompositionId::fromRaw(1),     "application/octet-stream",
        NoExtensionReferences{},       OpaqueExtensionPayload{std::byte{0x00}}};
    const ExtensionRecord remapperRecord{
        ExtensionRecordId::fromRaw(2),
        "vendor.module",
        "vendor.module.record-type",
        {1, 0},
        std::nullopt,
        "text/x-vendor-note",
        ExtensionOwnerRemapper{"vendor.module.record-remapper", {1, 0}},
        OpaqueExtensionPayload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}}};
    expectations.expect(newProject.project.addExtensionRecord(linkTableRecord) &&
                            newProject.project.addExtensionRecord(markerRecord) &&
                            newProject.project.addExtensionRecord(remapperRecord),
                        "extension round trip: fixture records add out of numeric ID order");

    Document document{std::move(newProject.project)};
    const auto settings = neutralColorSettings();

    // Confirm the shape decode/reconstruct must invert before checking the round trip itself:
    // three records, three distinct reference-policy kinds, one typed subject.
    auto sourceSnapshot = document.snapshot();
    const auto encoded = encodeSnapshot(sourceSnapshot, settings);
    expectations.expect(encoded.ok, "extension round trip: source snapshot encodes");
    if (encoded.ok) {
        auto parsed = bloom::project::parseStrictJsonDom(asBytes(encoded.bytes), {},
                                                         makeOperation(kGenerousOperationBudget));
        expectations.expect(static_cast<bool>(parsed), "extension round trip: source bytes parse");
        if (parsed) {
            const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
            expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                                "extension round trip: source snapshot decodes");
            if (decoded.value() != nullptr) {
                const auto& records = decoded.value()->extensionRecords;
                expectations.expect(records.size() == 3,
                                    "extension round trip: three extension records decode");
                expectations.expect(
                    records.size() == 3 &&
                        std::holds_alternative<ExtensionOwnerRemapper>(
                            records[0].referencePolicy) &&
                        !records[0].subject.has_value(),
                    "extension round trip: the owner-remapper record decodes with a null subject");
                bool markerSubjectIsComposition = false;
                if (records.size() == 3) {
                    const std::optional<ExtensionTarget>& markerSubject = records[1].subject;
                    if (markerSubject.has_value()) {
                        markerSubjectIsComposition =
                            std::holds_alternative<CompositionId>(markerSubject.value());
                    }
                }
                expectations.expect(
                    records.size() == 3 &&
                        std::holds_alternative<NoExtensionReferences>(records[1].referencePolicy) &&
                        markerSubjectIsComposition,
                    "extension round trip: the none-policy record decodes with a typed "
                    "composition subject");
                expectations.expect(
                    records.size() == 3 && std::holds_alternative<ExtensionHostReferenceTable>(
                                               records[2].referencePolicy),
                    "extension round trip: the host-table record decodes its reference policy "
                    "kind");
            }
        }
    }

    expectDeterminismRoundTrip(expectations, document, settings,
                               "extension fixture: determinism round trip (encode/decode/"
                               "reconstruct/re-encode/highWater) holds at every step");
}

// ---------------------------------------------------------------------------------------------
// Decode-layer rejections: idAllocation.highestIssued and extensions (R3 additions).
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kValidDigest =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

[[nodiscard]] std::string defaultColorSettingsJson() {
    std::string result =
        R"({"schemaVersion":{"major":1,"minor":0},"processColorSpaceId":"lin_rec709_scene",)"
        R"("ocioConfig":{"schemaVersion":{"major":1,"minor":0},)"
        R"("locator":{"kind":"builtin","uri":"bloom://ocio/neutral-v1/config.ocio"},)"
        R"("expectedRevision":{"algorithm":"sha256","digest":")";
    result += kValidDigest;
    result += R"("},"portability":"builtin","contextVariables":[]}})";
    return result;
}

constexpr std::string_view kMinimalGraphJson =
    R"({"nodes":[{"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
    R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
    R"("compositionOutput":{"nodeId":"1","port":"image"}})";

[[nodiscard]] std::string defaultCompositionJson() {
    std::string result =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"width":1920,"height":1080,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}},"parameters":[],"animationCurves":[],)"
        R"("graph":)";
    result += kMinimalGraphJson;
    result += "}";
    return result;
}

[[nodiscard]] std::string documentJson(const std::string_view idAllocationJson,
                                       const std::string_view extensionsJson) {
    std::string result =
        R"({"schemaVersion":{"major":1,"minor":0},"project":{"id":"1","name":"Untitled",)"
        R"("colorSettings":)";
    result += defaultColorSettingsJson();
    result += R"(,"compositions":[)";
    result += defaultCompositionJson();
    result += R"(]},"idAllocation":)";
    result += idAllocationJson;
    result += R"(,"extensions":)";
    result += extensionsJson;
    result += "}";
    return result;
}

constexpr std::string_view kDefaultHighestIssued =
    R"({"highestIssued":{"composition":"0","node":"0","edge":"0","layer":"0","layerSlot":"0",)"
    R"("parameter":"0","animationCurve":"0","keyframe":"0","driverBinding":"0",)"
    R"("extensionRecord":"0"}})";

void expectDecodeFailure(Expectations& expectations, const std::string& text,
                         const DocumentDecodeError expectedError,
                         const std::string_view expectedPath, const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(
        !decoded && decoded.error() == expectedError && decoded.path() == expectedPath, message);
}

void testHighestIssuedRejections(Expectations& expectations) {
    expectDecodeFailure(
        expectations,
        documentJson(R"({"highestIssued":{"node":"0","composition":"0","edge":"0","layer":"0",)"
                     R"("layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
                     R"("driverBinding":"0","extensionRecord":"0"}})",
                     "[]"),
        DocumentDecodeError::MemberOutOfOrder, "/idAllocation/highestIssued/node",
        "highestIssued members out of order report MemberOutOfOrder at the misplaced key");

    expectDecodeFailure(
        expectations,
        documentJson(R"({"highestIssued":{"composition":"0","node":"0","edge":"0","layer":"0",)"
                     R"("layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
                     R"("driverBinding":"0"}})",
                     "[]"),
        DocumentDecodeError::MissingMember, "/idAllocation/highestIssued/extensionRecord",
        "a highestIssued object missing its trailing member reports MissingMember");

    expectDecodeFailure(
        expectations,
        documentJson(R"({"highestIssued":{"composition":"0","node":"0","edge":"0","layer":"0",)"
                     R"("layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
                     R"("driverBinding":"0","extensionRecord":"0","extra":"0"}})",
                     "[]"),
        DocumentDecodeError::UnknownMember, "/idAllocation/highestIssued/extra",
        "a highestIssued object with an eleventh member reports UnknownMember");

    expectDecodeFailure(
        expectations,
        documentJson(R"({"highestIssued":{"composition":"+1","node":"0","edge":"0","layer":"0",)"
                     R"("layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
                     R"("driverBinding":"0","extensionRecord":"0"}})",
                     "[]"),
        DocumentDecodeError::InvalidAllocatorHighWater, "/idAllocation/highestIssued/composition",
        "a leading-plus highestIssued spelling is rejected as non-canonical");

    expectDecodeFailure(
        expectations,
        documentJson(R"({"highestIssued":{"composition":"01","node":"0","edge":"0","layer":"0",)"
                     R"("layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
                     R"("driverBinding":"0","extensionRecord":"0"}})",
                     "[]"),
        DocumentDecodeError::InvalidAllocatorHighWater, "/idAllocation/highestIssued/composition",
        "a leading-zero highestIssued spelling is rejected as non-canonical");

    const auto acceptedZero = decodeText(documentJson(std::string(kDefaultHighestIssued), "[]"));
    expectations.expect(static_cast<bool>(acceptedZero) && acceptedZero.value() != nullptr,
                        "a highestIssued of all \"0\" is accepted (0 is a valid high-water value)");
    if (acceptedZero.value() != nullptr) {
        expectations.expect(acceptedZero.value()->highWater ==
                                bloom::document::IdAllocatorHighWater{},
                            "an all-zero highestIssued decodes to a default-valued high water");
    }
}

[[nodiscard]] std::string extensionRecordJson(const std::string_view id,
                                              const std::string_view subject,
                                              const std::string_view referencePolicy,
                                              const std::string_view payload) {
    std::string result = R"({"id":")";
    result += id;
    result += R"(","ownerId":"vendor.module","typeId":"vendor.module.record-type",)"
              R"("schemaVersion":{"major":1,"minor":0},"subject":)";
    result += subject;
    result += R"(,"mediaType":"application/octet-stream","referencePolicy":)";
    result += referencePolicy;
    result += R"(,"payload":")";
    result += payload;
    result += "\"}";
    return result;
}

void testExtensionRejections(Expectations& expectations) {
    const auto highestIssued = std::string(kDefaultHighestIssued);
    const auto noneRecord = [](const std::string_view id) {
        return extensionRecordJson(id, "null", R"({"kind":"none"})", "AA==");
    };

    expectDecodeFailure(
        expectations,
        documentJson(highestIssued, "[" + noneRecord("2") + "," + noneRecord("1") + "]"),
        DocumentDecodeError::UnsortedExtensionRecords, "/extensions/1/id",
        "extension records out of numeric ID order report UnsortedExtensionRecords");

    expectDecodeFailure(
        expectations,
        documentJson(highestIssued, "[" + noneRecord("1") + "," + noneRecord("1") + "]"),
        DocumentDecodeError::DuplicateExtensionRecord, "/extensions/1/id",
        "two extension records sharing one numeric ID report DuplicateExtensionRecord");

    expectDecodeFailure(
        expectations,
        documentJson(highestIssued, "[" +
                                        extensionRecordJson("1", R"({"kind":"widget","id":"1"})",
                                                            R"({"kind":"none"})", "AA==") +
                                        "]"),
        DocumentDecodeError::InvalidExtensionTargetKind, "/extensions/0/subject/kind",
        "an unknown typed-subject kind reports InvalidExtensionTargetKind");

    expectDecodeFailure(
        expectations,
        documentJson(highestIssued,
                     "[" + extensionRecordJson("1", "null", R"({"kind":"mystery"})", "AA==") + "]"),
        DocumentDecodeError::InvalidReferencePolicyKind, "/extensions/0/referencePolicy/kind",
        "an unknown referencePolicy kind reports InvalidReferencePolicyKind");

    expectDecodeFailure(
        expectations,
        documentJson(highestIssued,
                     "[" + extensionRecordJson("1", "null", R"({"kind":"none"})", "***") + "]"),
        DocumentDecodeError::InvalidBase64Payload, "/extensions/0/payload",
        "a malformed base64 payload spelling reports InvalidBase64Payload");

    const auto valid = decodeText(documentJson(highestIssued, "[" + noneRecord("1") + "]"));
    expectations.expect(static_cast<bool>(valid) && valid.value() != nullptr &&
                            valid.value()->extensionRecords.size() == 1,
                        "a single well-formed extension record decodes successfully");
}

// ---------------------------------------------------------------------------------------------
// Reconstruction-layer rejections: hand-built decoded envelopes exercise the checked document-model
// adders and the Document constructor's inclusive-watermark check directly (no JSON round trip
// needed -- reconstructDocument() takes an already-decoded envelope).
// ---------------------------------------------------------------------------------------------

// A minimal well-formed envelope: one composition with a Layer Stack node (also the graph's stable
// owner) wired straight to a composition-output node -- mirrors document_decode_tests.cpp's minimal
// shape. Callers mutate the returned envelope to provoke one specific rejection.
[[nodiscard]] DecodedDocumentEnvelope minimalEnvelope() {
    using namespace bloom::document;
    DecodedDocumentEnvelope envelope;
    envelope.projectId = ProjectId::fromRaw(1);
    envelope.projectName = "Untitled";
    envelope.colorSettings = bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));

    const auto duration = bloom::core::RationalTime::create(10, 1);
    const auto format = CompositionFormat::create(1920, 1080);
    if (!duration.has_value() || !format.has_value()) {
        std::abort();
    }

    bloom::project::DecodedComposition composition;
    composition.id = CompositionId::fromRaw(1);
    composition.name = "Comp";
    composition.duration = *duration;
    composition.format = *format;

    composition.graph.layerStack.nodeId = NodeId::fromRaw(1);
    composition.graph.nodes.push_back(
        {NodeId::fromRaw(1), std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion});
    composition.graph.nodes.push_back({NodeId::fromRaw(2),
                                       std::string(kCompositionOutputNodeType),
                                       {},
                                       kCompositionOutputNodeSchemaVersion});
    composition.graph.edges.push_back(
        {EdgeId::fromRaw(1),
         {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
         NodeInputRef{NodeId::fromRaw(2), std::string(kCompositionOutputInputPort)}});
    composition.graph.compositionOutput = {NodeId::fromRaw(2),
                                           std::string(kCompositionOutputOutputPort)};

    envelope.compositions.push_back(std::move(composition));
    envelope.highWater.composition = 1;
    envelope.highWater.node = 2;
    envelope.highWater.edge = 1;
    return envelope;
}

void testGraphStoreRejections(Expectations& expectations) {
    using namespace bloom::document;

    // CanonicalGraph::addNode() rejects a structurally invalid parameter-binding role even though
    // decode itself (which does not apply grammar rules to role text) accepted it.
    {
        auto envelope = minimalEnvelope();
        envelope.compositions.front().graph.nodes.front().parameters.push_back(
            {"", ParameterId::fromRaw(1)});
        auto reconstructed = bloom::project::reconstructDocument(envelope);
        expectations.expect(!reconstructed,
                            "a node with a structurally invalid binding role is rejected");
        if (!reconstructed) {
            expectations.expect(
                reconstructed.rejection().stage == ReconstructionStage::GraphNode &&
                    reconstructed.rejection().compositionId == CompositionId::fromRaw(1) &&
                    reconstructed.rejection().recordId == 1,
                "the rejection names GraphNode, the owning composition, and the offending node id");
        }
    }

    // LayerStack::append() rejects a second entry that duplicates an already-appended slot/layer --
    // decode itself does not check Layer Stack entry duplicates (they are canonical source order,
    // never sorted).
    {
        auto envelope = minimalEnvelope();
        auto& composition = envelope.compositions.front();
        composition.graph.layerStack.entries.push_back(
            {LayerSlotId::fromRaw(1), LayerId::fromRaw(1)});
        composition.graph.layerStack.entries.push_back(
            {LayerSlotId::fromRaw(1), LayerId::fromRaw(1)});
        auto reconstructed = bloom::project::reconstructDocument(envelope);
        expectations.expect(!reconstructed, "a duplicated Layer Stack entry is rejected");
        if (!reconstructed) {
            expectations.expect(
                reconstructed.rejection().stage == ReconstructionStage::LayerStackEntry &&
                    reconstructed.rejection().compositionId == CompositionId::fromRaw(1) &&
                    reconstructed.rejection().recordId == 1,
                "the rejection names LayerStackEntry, the owning composition, and the slot id");
        }
    }
}

void testProjectValidateRejection(Expectations& expectations) {
    // A layer-output node whose "position"/"opacity" bindings are swapped: each parameter is
    // individually valid against its own declared schemaKey (so ParameterStore::insert() and
    // CanonicalGraph::addNode() both accept it), but the wrong parameter is bound to each role,
    // which only bloom::document::CanonicalGraph::validate()'s expected-binding check (reached
    // through Project::validate()) detects.
    using namespace bloom::document;
    auto envelope = minimalEnvelope();
    auto& composition = envelope.compositions.front();

    composition.graph.nodes.front() = {
        NodeId::fromRaw(1), std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion};
    NodeRecord layerOutputNode{
        NodeId::fromRaw(3), std::string(kLayerOutputNodeType), {}, kLayerOutputNodeSchemaVersion};
    // Swapped on purpose: "position" is bound to the opacity-schema parameter and vice versa.
    layerOutputNode.parameters = {{"opacity", ParameterId::fromRaw(5)},
                                  {"position", ParameterId::fromRaw(3)}};
    composition.graph.nodes.push_back(std::move(layerOutputNode));
    composition.graph.compositionOutput = {NodeId::fromRaw(2),
                                           std::string(kCompositionOutputOutputPort)};
    composition.graph.edges.push_back(
        {EdgeId::fromRaw(2),
         {NodeId::fromRaw(3), std::string(kLayerOutputOutputPort)},
         LayerStackInputRef{NodeId::fromRaw(1), LayerSlotId::fromRaw(1),
                            std::string(kLayerStackContentInputRole)}});
    composition.graph.layerOutputs.push_back(
        {NodeId::fromRaw(3), LayerId::fromRaw(1), "Layer", std::string(kLayerOutputOutputPort)});
    composition.graph.layerStack.entries.push_back({LayerSlotId::fromRaw(1), LayerId::fromRaw(1)});

    composition.parameters.push_back({ParameterId::fromRaw(3),
                                      std::string(kOpacityParameterSchemaKey),
                                      ConstantValueSource{0.5}});
    composition.parameters.push_back({ParameterId::fromRaw(5),
                                      std::string(kPositionParameterSchemaKey),
                                      ConstantValueSource{Vec2d{0.0, 0.0}}});

    envelope.highWater.node = 3;
    envelope.highWater.edge = 2;
    envelope.highWater.layer = 1;
    envelope.highWater.layerSlot = 1;
    envelope.highWater.parameter = 5;

    auto reconstructed = bloom::project::reconstructDocument(envelope);
    expectations.expect(!reconstructed,
                        "a layer-output node bound to mismatched-schema parameters is rejected");
    if (!reconstructed) {
        expectations.expect(reconstructed.rejection().stage == ReconstructionStage::ProjectValidate,
                            "the rejection surfaces through the whole-project validate() stage");
    }
}

void testDocumentConstructRejection(Expectations& expectations) {
    // The graph/store/composition/extension admission and Project::validate() all succeed; only the
    // decoded high water is wrong (below the highest declared node id), so the failure can only
    // come from Document's own inclusive-watermark check.
    auto envelope = minimalEnvelope();
    envelope.highWater.node = 1; // node 2 (compositionOutput) is declared but not covered.

    auto reconstructed = bloom::project::reconstructDocument(envelope);
    expectations.expect(!reconstructed,
                        "a decoded id above its namespace's persisted high water is rejected");
    if (!reconstructed) {
        expectations.expect(
            reconstructed.rejection().stage == ReconstructionStage::DocumentConstruct,
            "the rejection surfaces through the Document constructor's provenance check");
    }
}

void testWellFormedEnvelopeReconstructs(Expectations& expectations) {
    auto envelope = minimalEnvelope();
    auto reconstructed = bloom::project::reconstructDocument(envelope);
    expectations.expect(static_cast<bool>(reconstructed),
                        "a well-formed hand-built envelope reconstructs successfully");
    if (reconstructed) {
        expectations.expect(reconstructed.value() != nullptr &&
                                reconstructed.value()->document != nullptr,
                            "a successful reconstruction owns a live Document");
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testMinimalDeterminismRoundTrip(expectations);
        testComposedDeterminismRoundTrip(expectations);
        testExtensionDeterminismRoundTrip(expectations);
        testHighestIssuedRejections(expectations);
        testExtensionRejections(expectations);
        testGraphStoreRejections(expectations);
        testProjectValidateRejection(expectations);
        testDocumentConstructRejection(expectations);
        testWellFormedEnvelopeReconstructs(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
