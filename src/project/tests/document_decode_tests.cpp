#include <bloom/project/document_decode.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/unknown_json_number.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numeric>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::project::DecodedDocumentEnvelope;
using bloom::project::DocumentClassification;
using bloom::project::DocumentDecodeError;
using bloom::project::DocumentDecodeOutcome;
using bloom::project::DocumentDecodeResult;
using bloom::project::JsonValue;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::RetainedJsonValue;
using bloom::project::RetainedJsonValueKind;
using bloom::project::RoundTripAttachmentPath;
using bloom::project::RoundTripCollectionKind;
using bloom::project::RoundTripPathSegment;
using bloom::project::RoundTripPreservationReason;
using bloom::project::StrictJsonDomResult;
using bloom::project::UnknownJsonNumberKind;

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

// ---------------------------------------------------------------------------------------------
// Minimal hand-assembled document.json fixtures. parseStrictJsonDom accepts any insignificant
// whitespace and member order (canonical layout is only a writer requirement), so these compact,
// non-canonically-ordered literals are valid input; document_decode.cpp's own order checks are
// exercised deliberately by the "wrong member order" fixtures below. Composition objects below
// carry only id/name/duration/format: the decoder matches composition members as a prefix (see
// document_decode.hpp's scope comment), so parameters/animationCurves/graph need not be present.
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kSchemaVersion10 = R"({"major":1,"minor":0})";
constexpr std::string_view kValidDigest =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

// A minimal closed composition interior: exactly one node (used both as the required Layer Stack
// owner and the required compositionOutput target, which R2 permits -- CanonicalGraph's stricter
// node-type/role invariants are a later document-construction concern). Used by every fixture
// below that is not itself exercising parameters/animationCurves/graph.
constexpr std::string_view kMinimalGraphJson =
    R"({"nodes":[{"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
    R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
    R"("compositionOutput":{"nodeId":"1","port":"image"}})";

[[nodiscard]] std::string colorSettingsJson(const std::string_view digest,
                                            const std::string_view portability,
                                            const std::string_view contextVariablesJson) {
    std::string result =
        R"({"schemaVersion":{"major":1,"minor":0},"processColorSpaceId":"lin_rec709_scene",)"
        R"("ocioConfig":{"schemaVersion":{"major":1,"minor":0},)"
        R"("locator":{"kind":"builtin","uri":"bloom://ocio/neutral-v1/config.ocio"},)"
        R"("expectedRevision":{"algorithm":"sha256","digest":")";
    result += digest;
    result += R"("},"portability":")";
    result += portability;
    result += R"(","contextVariables":[)";
    result += contextVariablesJson;
    result += "]}}";
    return result;
}

[[nodiscard]] std::string defaultColorSettingsJson() {
    return colorSettingsJson(kValidDigest, "builtin", "");
}

[[nodiscard]] std::string compositionJson(
    const std::string_view idJson, const std::string_view name,
    const std::string_view durationNumerator, const std::string_view durationDenominator,
    const std::string_view widthJson, const std::string_view heightJson,
    const std::string_view pixelAspectNumerator, const std::string_view pixelAspectDenominator,
    const std::string_view frameRateNumerator, const std::string_view frameRateDenominator) {
    std::string result = "{\"id\":";
    result += idJson;
    result += ",\"name\":\"";
    result += name;
    result += "\",\"duration\":{\"numerator\":\"";
    result += durationNumerator;
    result += "\",\"denominator\":\"";
    result += durationDenominator;
    result += "\"},\"format\":{\"width\":";
    result += widthJson;
    result += ",\"height\":";
    result += heightJson;
    result += ",\"pixelAspect\":{\"numerator\":\"";
    result += pixelAspectNumerator;
    result += "\",\"denominator\":\"";
    result += pixelAspectDenominator;
    result += "\"},\"frameRate\":{\"numerator\":\"";
    result += frameRateNumerator;
    result += "\",\"denominator\":\"";
    result += frameRateDenominator;
    result += "\"}},\"parameters\":[],\"animationCurves\":[],\"graph\":";
    result += kMinimalGraphJson;
    result += "}";
    return result;
}

[[nodiscard]] std::string defaultCompositionJson(const std::string_view idJson = R"("1")") {
    return compositionJson(idJson, "Comp", "10", "1", "1920", "1080", "1", "1", "24", "1");
}

// Builds a full composition object with id "1"/name "Comp"/a fixed valid duration and format, and
// caller-supplied parameters/animationCurves/graph bodies -- used by every R2 composition-interior
// rejection and acceptance test below.
[[nodiscard]] std::string compositionWithInterior(const std::string_view parametersJson,
                                                  const std::string_view animationCurvesJson,
                                                  const std::string_view graphJson) {
    std::string result =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"width":1920,"height":1080,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}},"parameters":)";
    result += parametersJson;
    result += R"(,"animationCurves":)";
    result += animationCurvesJson;
    result += R"(,"graph":)";
    result += graphJson;
    result += "}";
    return result;
}

[[nodiscard]] std::string documentJson(const std::string_view schemaVersionJson,
                                       const std::string_view projectIdJson,
                                       const std::string_view projectNameJson,
                                       const std::string_view colorSettingsJsonText,
                                       const std::string_view compositionsArrayBody) {
    std::string result = "{\"schemaVersion\":";
    result += schemaVersionJson;
    result += ",\"project\":{\"id\":";
    result += projectIdJson;
    result += ",\"name\":";
    result += projectNameJson;
    result += ",\"colorSettings\":";
    result += colorSettingsJsonText;
    result += ",\"compositions\":[";
    result += compositionsArrayBody;
    result += R"(]},"idAllocation":{"highestIssued":{"composition":"0","node":"0","edge":"0",)"
              R"("layer":"0","layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
              R"("driverBinding":"0","extensionRecord":"0"}},"extensions":[]})";
    return result;
}

[[nodiscard]] std::string baselineDocument() {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", defaultColorSettingsJson(),
                        defaultCompositionJson());
}

[[nodiscard]] std::string documentWithComposition(const std::string_view compositionJsonText) {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", defaultColorSettingsJson(),
                        compositionJsonText);
}

[[nodiscard]] std::string documentWithColorSettings(const std::string_view colorSettingsJsonText) {
    return documentJson(kSchemaVersion10, R"("1")", R"("Untitled")", colorSettingsJsonText,
                        defaultCompositionJson());
}

// ---------------------------------------------------------------------------------------------
// RT1 fixtures: schemaVersion {1, minor > 0} documents (see docs/architecture/project-format.md,
// "Versions, Migrations, And Preservation"). documentJson() above always writes an exact-{1,0}
// extensions-free skeleton; RT1's own fixtures additionally need a caller-chosen minor and,
// sometimes, a non-empty extensions array, so this module adds its own skeleton builder rather
// than complicating every existing R2/R3 call site above.
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kSchemaVersion11 = R"({"major":1,"minor":1})";

[[nodiscard]] std::string
documentWithCompositionMinor1(const std::string_view compositionJsonText) {
    return documentJson(kSchemaVersion11, R"("1")", R"("Untitled")", defaultColorSettingsJson(),
                        compositionJsonText);
}

[[nodiscard]] std::string
documentWithColorSettingsMinor1(const std::string_view colorSettingsJsonText) {
    return documentJson(kSchemaVersion11, R"("1")", R"("Untitled")", colorSettingsJsonText,
                        defaultCompositionJson());
}

// Like documentJson() above, but also lets the caller supply the extensions array body and its
// matching idAllocation.highestIssued.extensionRecord high-water (documentJson() always writes an
// empty extensions array with an all-zero high-water).
[[nodiscard]] std::string documentWithExtensions(const std::string_view schemaVersionJson,
                                                 const std::string_view extensionsArrayBody,
                                                 const std::string_view extensionRecordHighWater) {
    std::string result = "{\"schemaVersion\":";
    result += schemaVersionJson;
    result += R"(,"project":{"id":"1","name":"Untitled","colorSettings":)";
    result += defaultColorSettingsJson();
    result += R"(,"compositions":[)";
    result += defaultCompositionJson();
    result += R"(]},"idAllocation":{"highestIssued":{"composition":"0","node":"0","edge":"0",)"
              R"("layer":"0","layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
              R"("driverBinding":"0","extensionRecord":")";
    result += extensionRecordHighWater;
    result += R"("}},"extensions":[)";
    result += extensionsArrayBody;
    result += "]}";
    return result;
}

// ---------------------------------------------------------------------------------------------
// Decode helpers
// ---------------------------------------------------------------------------------------------

[[nodiscard]] DocumentDecodeResult decodeText(const std::string& text) {
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(text), {},
                                                     makeOperation(kGenerousOperationBudget));
    if (!parsed) {
        // Every fixture below is constructed to be syntactically valid strict JSON; a parse
        // failure means the fixture itself is broken, not the decoder under test. Fail loudly
        // rather than reporting a misleading decode mismatch.
        std::cerr << "fixture failed to parse as strict JSON (error="
                  << static_cast<int>(parsed.error()) << ")\n";
        std::abort();
    }
    return bloom::project::decodeDocumentEnvelope(parsed.document()->root());
}

void expectDecodeFailure(Expectations& expectations, const std::string& text,
                         const DocumentDecodeError expectedError,
                         const std::string_view expectedPath, const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(!decoded && decoded.outcome() == DocumentDecodeOutcome::Failed &&
                            decoded.error() == expectedError && decoded.path() == expectedPath,
                        message);
}

// RT1: asserts a same-major newer-minor document classifies PreservedReadOnlyRequired (an unknown
// core discriminator or an unknown number outside the lossless subset -- see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "unknown core
// discriminators are never guessed") rather than decoding or hard-failing. No
// DecodedDocumentEnvelope or RoundTripState is produced for this outcome.
void expectPreservedReadOnly(Expectations& expectations, const std::string& text,
                             const RoundTripPreservationReason expectedReason,
                             const std::string_view expectedPath, const std::string_view message) {
    const auto decoded = decodeText(text);
    expectations.expect(
        !decoded && decoded.outcome() == DocumentDecodeOutcome::PreservedReadOnlyRequired &&
            decoded.preservationReason() == expectedReason && decoded.path() == expectedPath &&
            decoded.value() == nullptr && decoded.roundTrip() == nullptr,
        message);
}

// ---------------------------------------------------------------------------------------------
// Success path sanity and full round trip
// ---------------------------------------------------------------------------------------------

void testBaselineDecodesSuccessfully(Expectations& expectations) {
    const auto decoded = decodeText(baselineDocument());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the hand-assembled baseline fixture decodes without error");
    if (decoded.value() != nullptr) {
        const auto& envelope = *decoded.value();
        expectations.expect(envelope.projectId.value() == 1,
                            "the baseline project id decodes to 1");
        expectations.expect(envelope.projectName == "Untitled",
                            "the baseline project name decodes exactly");
        expectations.expect(envelope.compositions.size() == 1,
                            "the baseline fixture decodes exactly one composition");
    }
}

void testFullRoundTrip(Expectations& expectations) {
    using bloom::core::RationalTime;
    using bloom::document::Document;

    const auto duration = RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<std::uint8_t, 32> digestBytes{};
    std::iota(digestBytes.begin(), digestBytes.end(), std::uint8_t{0});
    const auto settings = bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(digestBytes));

    std::array<char, 256> payloadScratch{};
    std::array<std::size_t, 256> sortScratch{};
    const bloom::project::CanonicalDocumentV1 request{
        .snapshot = &snapshot,
        .colorSettings = &settings,
        .payloadScratch = payloadScratch,
        .sortScratch = sortScratch,
    };
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        expectations.expect(false, "the round-trip snapshot sizes successfully");
        return;
    }
    std::vector<char> output(*size.value());
    const auto written = bloom::project::encodeCanonicalDocument(request, output);
    if (!written) {
        expectations.expect(false, "the round-trip snapshot encodes successfully");
        return;
    }

    const std::string_view encodedText(output.data(), output.size());
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(encodedText), {},
                                                     makeOperation(kGenerousOperationBudget));
    expectations.expect(static_cast<bool>(parsed),
                        "the canonical writer's own bytes parse as strict JSON");
    if (!parsed) {
        return;
    }

    const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the canonical writer's own bytes decode without error");
    if (decoded.value() == nullptr) {
        return;
    }

    const auto& envelope = *decoded.value();
    const auto& sourceProject = snapshot.project();
    expectations.expect(envelope.projectId == sourceProject.id(),
                        "round trip: decoded project id equals the source project id");
    expectations.expect(envelope.projectName == sourceProject.name(),
                        "round trip: decoded project name equals the source project name");
    expectations.expect(envelope.colorSettings == settings,
                        "round trip: decoded color settings equal the source color settings");
    expectations.expect(
        envelope.compositions.size() == sourceProject.compositions().size(),
        "round trip: decoded composition count equals the source composition count");
    if (envelope.compositions.size() == sourceProject.compositions().size()) {
        for (std::size_t index = 0; index < envelope.compositions.size(); ++index) {
            const auto& decodedComposition = envelope.compositions[index];
            const auto& sourceComposition = sourceProject.compositions()[index];
            expectations.expect(
                decodedComposition.id == sourceComposition.id(),
                "round trip: decoded composition id equals the source composition id");
            expectations.expect(
                decodedComposition.name == sourceComposition.name(),
                "round trip: decoded composition name equals the source composition name");
            expectations.expect(
                decodedComposition.duration == sourceComposition.duration(),
                "round trip: decoded composition duration equals the source duration");
            expectations.expect(decodedComposition.format == sourceComposition.format(),
                                "round trip: decoded composition format equals the source format");
            expectations.expect(
                decodedComposition.parameters.empty() &&
                    sourceComposition.parameters().records().empty(),
                "round trip: the minimal new-project composition has no parameters on either side");
            expectations.expect(
                decodedComposition.animationCurves.empty() &&
                    sourceComposition.animationCurves().records().empty(),
                "round trip: the minimal new-project composition has no animation curves on "
                "either side");
            expectations.expect(
                decodedComposition.graph.nodes.size() == 2 &&
                    decodedComposition.graph.edges.size() == 1 &&
                    decodedComposition.graph.layerOutputs.empty(),
                "round trip: the minimal new-project graph decodes its Layer Stack node, "
                "composition-output node, and connecting edge");
            expectations.expect(
                decodedComposition.graph.layerStack.nodeId ==
                        sourceComposition.graph().layerStack().nodeId() &&
                    decodedComposition.graph.layerStack.entries.empty(),
                "round trip: decoded Layer Stack node id matches and has no entries");
            expectations.expect(
                sourceComposition.graph().compositionOutput().has_value() &&
                    decodedComposition.graph.compositionOutput ==
                        *sourceComposition.graph().compositionOutput(),
                "round trip: decoded compositionOutput matches the source graph's output");
        }
    }
}

void testComposedRoundTrip(Expectations& expectations) {
    using namespace bloom::document;
    using bloom::core::Color4d;
    using bloom::core::PixelAspectRatio;
    using bloom::core::RationalTime;

    // Mirrors canonical_document_tests.cpp's testComposedGoldenBytes fixture (solid layer, one
    // animated scalar opacity curve, one constant vec2 position, one constant color4) so this test
    // exercises the exact composed shape the writer emits and this decoder must invert.
    const auto duration = RationalTime::create(48, 24);
    const auto frameRate = FrameRate::create(24000, 1001);
    const auto pixelAspect = PixelAspectRatio::create(2, 1);
    const auto format =
        CompositionFormat::create(1280, 720, pixelAspect.value_or(PixelAspectRatio::square()),
                                  frameRate.value_or(FrameRate::framesPerSecond24()));
    if (!duration.has_value() || !format.has_value()) {
        expectations.expect(false, "the composed fixture values are constructible");
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
        "the composed fixture graph accepts its deliberately scrambled parts");

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
                        "the composed fixture parameters insert out of numeric ID order");

    ScalarAnimationCurve curve;
    curve.id = AnimationCurveId::fromRaw(9);
    curve.keyframes.push_back(
        {KeyframeId::fromRaw(21), RationalTime{}, 0.25, KeyframeInterpolation::Hold});
    curve.keyframes.push_back({KeyframeId::fromRaw(22), RationalTime::fromInteger(48), 1.0,
                               KeyframeInterpolation::Linear});
    expectations.expect(composition.animationCurves().insert(curve),
                        "the composed fixture animation curve inserts");

    Project project{ProjectId::fromRaw(1), "Spot Check"};
    expectations.expect(project.addComposition(std::move(composition)),
                        "the composed fixture composition adds");

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
    auto snapshot = document.snapshot();

    std::array<char, 256> payloadScratch{};
    std::array<std::size_t, 256> sortScratch{};
    const auto settings =
        bloom::document::makeBloomNeutralColorSettingsV1(bloom::core::Sha256Digest::fromBytes([] {
            std::array<std::uint8_t, 32> bytes{};
            std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
            return bytes;
        }()));
    const bloom::project::CanonicalDocumentV1 request{
        .snapshot = &snapshot,
        .colorSettings = &settings,
        .payloadScratch = payloadScratch,
        .sortScratch = sortScratch,
    };
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        expectations.expect(false, "the composed fixture sizes successfully");
        return;
    }
    std::vector<char> output(*size.value());
    const auto written = bloom::project::encodeCanonicalDocument(request, output);
    if (!written) {
        expectations.expect(false, "the composed fixture encodes successfully");
        return;
    }

    const std::string_view encodedText(output.data(), output.size());
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(encodedText), {},
                                                     makeOperation(kGenerousOperationBudget));
    expectations.expect(static_cast<bool>(parsed), "the composed fixture parses as strict JSON");
    if (!parsed) {
        return;
    }

    const auto decoded = bloom::project::decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the composed fixture decodes without error");
    if (decoded.value() == nullptr) {
        return;
    }

    expectations.expect(decoded.value()->compositions.size() == 1,
                        "the composed fixture decodes exactly one composition");
    if (decoded.value()->compositions.empty()) {
        return;
    }
    const auto& decodedComposition = decoded.value()->compositions.front();
    const auto& sourceComposition = snapshot.project().compositions().front();

    // Parameters: source insertion order is 7,5,3 (out-of-order on purpose); decoded order is
    // strictly ascending numeric ParameterId.
    std::vector<ParameterRecord> sourceParameters(sourceComposition.parameters().records().begin(),
                                                  sourceComposition.parameters().records().end());
    std::ranges::sort(sourceParameters,
                      [](const auto& left, const auto& right) { return left.id < right.id; });
    expectations.expect(decodedComposition.parameters == sourceParameters,
                        "composed round trip: decoded parameters equal the source parameters, "
                        "sorted by numeric id");

    // Animation curves: AnimationCurveStore already keeps sorted order.
    const std::vector<AnimationCurveRecord> sourceCurves(
        sourceComposition.animationCurves().records().begin(),
        sourceComposition.animationCurves().records().end());
    expectations.expect(decodedComposition.animationCurves == sourceCurves,
                        "composed round trip: decoded animation curves equal the source curves");

    // Graph: nodes/edges/layerOutputs were inserted out of id order on purpose; sort copies to
    // match the decoder's canonical numeric order before comparing.
    std::vector<NodeRecord> sourceNodes(sourceComposition.graph().nodes().begin(),
                                        sourceComposition.graph().nodes().end());
    std::ranges::sort(sourceNodes,
                      [](const auto& left, const auto& right) { return left.id < right.id; });
    expectations.expect(decodedComposition.graph.nodes == sourceNodes,
                        "composed round trip: decoded nodes equal the source nodes, sorted by id");

    std::vector<EdgeRecord> sourceEdges(sourceComposition.graph().edges().begin(),
                                        sourceComposition.graph().edges().end());
    std::ranges::sort(sourceEdges,
                      [](const auto& left, const auto& right) { return left.id < right.id; });
    expectations.expect(decodedComposition.graph.edges == sourceEdges,
                        "composed round trip: decoded edges equal the source edges, sorted by id");

    std::vector<LayerOutputBoundary> sourceLayerOutputs(
        sourceComposition.graph().layerOutputs().begin(),
        sourceComposition.graph().layerOutputs().end());
    std::ranges::sort(sourceLayerOutputs, [](const auto& left, const auto& right) {
        return std::pair{left.layerId.value(), left.nodeId.value()} <
               std::pair{right.layerId.value(), right.nodeId.value()};
    });
    expectations.expect(decodedComposition.graph.layerOutputs == sourceLayerOutputs,
                        "composed round trip: decoded layer outputs equal the source layer "
                        "outputs, sorted by (layerId, nodeId)");

    expectations.expect(decodedComposition.graph.layerStack.nodeId ==
                            sourceComposition.graph().layerStack().nodeId(),
                        "composed round trip: decoded Layer Stack node id equals the source");
    const std::vector<LayerStackEntry> sourceEntries(
        sourceComposition.graph().layerStack().entries().begin(),
        sourceComposition.graph().layerStack().entries().end());
    expectations.expect(decodedComposition.graph.layerStack.entries == sourceEntries,
                        "composed round trip: decoded Layer Stack entries equal the source "
                        "entries in source order");

    expectations.expect(sourceComposition.graph().compositionOutput().has_value() &&
                            decodedComposition.graph.compositionOutput ==
                                *sourceComposition.graph().compositionOutput(),
                        "composed round trip: decoded compositionOutput equals the source");
}

// ---------------------------------------------------------------------------------------------
// Structural rejection: exact member order and unknown members
// ---------------------------------------------------------------------------------------------

void testRejectsRootMemberOrder(Expectations& expectations) {
    const std::string project =
        std::string("{\"id\":\"1\",\"name\":\"Untitled\",\"colorSettings\":") +
        defaultColorSettingsJson() + ",\"compositions\":[" + defaultCompositionJson() + "]}";
    const std::string json = std::string("{\"schemaVersion\":") + std::string(kSchemaVersion10) +
                             ",\"idAllocation\":{},\"project\":" + project + ",\"extensions\":[]}";
    expectDecodeFailure(
        expectations, json, DocumentDecodeError::MemberOutOfOrder, "/idAllocation",
        "a root object with idAllocation before project reports member-out-of-order "
        "at the exact offending key");
}

void testRejectsRootUnknownMember(Expectations& expectations) {
    const std::string json =
        baselineDocument().substr(0, baselineDocument().size() - 1) + ",\"bogus\":null}";
    expectDecodeFailure(expectations, json, DocumentDecodeError::UnknownMember, "/bogus",
                        "an unrecognized trailing root member is rejected with its exact path");
}

void testRejectsProjectMemberOrder(Expectations& expectations) {
    const std::string project =
        std::string("{\"name\":\"Untitled\",\"id\":\"1\",\"colorSettings\":") +
        defaultColorSettingsJson() + ",\"compositions\":[" + defaultCompositionJson() + "]}";
    const std::string json = std::string("{\"schemaVersion\":") + std::string(kSchemaVersion10) +
                             ",\"project\":" + project + ",\"idAllocation\":{},\"extensions\":[]}";
    expectDecodeFailure(expectations, json, DocumentDecodeError::MemberOutOfOrder, "/project/name",
                        "a project object with name before id reports member-out-of-order at the "
                        "exact offending key");
}

void testRejectsCompositionMemberOrder(Expectations& expectations) {
    const std::string composition =
        R"({"name":"Comp","id":"1","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"width":1920,"height":1080,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}}})";
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::MemberOutOfOrder,
        "/project/compositions/0/name",
        "a composition object with name before id reports member-out-of-order at the "
        "exact offending key");
}

void testRejectsFormatMemberOrder(Expectations& expectations) {
    // Unlike testRejectsCompositionMissingFormat/testRejectsCompositionMemberOrder above (whose
    // failure is detected while matching the composition object's own seven-key shape, before
    // format's content is ever inspected), this test exercises decodeFormat's own internal member
    // order, so the composition must otherwise be fully closed (parameters/animationCurves/graph
    // present) for control to reach format's content decode at all.
    const std::string composition =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"},)"
        R"("format":{"height":1080,"width":1920,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"}},"parameters":[],)"
        R"("animationCurves":[],"graph":)" +
        std::string(kMinimalGraphJson) + "}";
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::MemberOutOfOrder,
                        "/project/compositions/0/format/height",
                        "a format object with height before width reports member-out-of-order at "
                        "the exact offending key");
}

void testRejectsCompositionMissingFormat(Expectations& expectations) {
    const std::string composition =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"}})";
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::MissingMember, "/project/compositions/0/format",
                        "a composition missing its format member reports the exact missing path");
}

void testRejectsProjectIdWrongKind(Expectations& expectations) {
    const std::string json = documentJson(kSchemaVersion10, "1", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    expectDecodeFailure(expectations, json, DocumentDecodeError::WrongValueKind, "/project/id",
                        "a project id encoded as a JSON number instead of a string is rejected");
}

// ---------------------------------------------------------------------------------------------
// Schema version
// ---------------------------------------------------------------------------------------------

// RT1: a same-major newer-minor document with no unknown additive members at all now decodes --
// the pre-RT1 exact-{1,0}-only rejection is gone -- and classifies EditableWithRoundTrip with an
// empty RoundTripState (see docs/architecture/project-format.md, "Versions, Migrations, And
// Preservation": "a newer minor opens editable only when every unknown construct is additive,
// bounded, and provably preservable" -- trivially true here since there is none).
void testAcceptsSchemaVersionMinor11WithoutUnknownMembers(Expectations& expectations) {
    const std::string json = documentJson(R"({"major":1,"minor":1})", R"("1")", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    const auto decoded = decodeText(json);
    expectations.expect(decoded.outcome() == bloom::project::DocumentDecodeOutcome::Decoded &&
                            static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "document schemaVersion 1.1 with no unknown members decodes");
    expectations.expect(decoded.classification() ==
                            bloom::project::DocumentClassification::EditableWithRoundTrip,
                        "document schemaVersion 1.1 classifies EditableWithRoundTrip");
    expectations.expect(decoded.roundTrip() != nullptr && decoded.roundTrip()->empty(),
                        "document schemaVersion 1.1 with no unknown members has an empty "
                        "RoundTripState");
}

// Exact {1,0} keeps the pre-RT1 behavior exactly: value() is populated but classification() is
// ExactSchemaV1_0 and roundTrip() stays nullptr -- no RoundTripState is ever produced for an
// exact-1.0 document (see docs/architecture/project-format.md, "Versions, Migrations, And
// Preservation": "a document with schemaVersion {1, minor>0} may contain unknown additive
// members; exactly {1,0} must NOT").
void testExactSchemaV1_0ProducesNoRoundTripState(Expectations& expectations) {
    const auto decoded = decodeText(baselineDocument());
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the exact-1.0 baseline document decodes");
    expectations.expect(decoded.classification() ==
                            bloom::project::DocumentClassification::ExactSchemaV1_0,
                        "the exact-1.0 baseline document classifies ExactSchemaV1_0");
    expectations.expect(decoded.roundTrip() == nullptr,
                        "the exact-1.0 baseline document produces no RoundTripState");
}

void testRejectsSchemaVersionMajor2(Expectations& expectations) {
    const std::string json = documentJson(R"({"major":2,"minor":0})", R"("1")", R"("Untitled")",
                                          defaultColorSettingsJson(), defaultCompositionJson());
    expectDecodeFailure(expectations, json, DocumentDecodeError::DomainViolation, "/schemaVersion",
                        "document schemaVersion 2.0 is rejected as not exactly 1.0");
}

// ---------------------------------------------------------------------------------------------
// Object id canonical spelling
// ---------------------------------------------------------------------------------------------

void testRejectsProjectIdSpellings(Expectations& expectations) {
    for (const auto& [spelling, label] :
         {std::pair<std::string_view, std::string_view>{"0", "zero"},
          std::pair<std::string_view, std::string_view>{"01", "leading-zero"},
          std::pair<std::string_view, std::string_view>{"+1", "leading-plus"}}) {
        const std::string idJson = std::string("\"") + std::string(spelling) + "\"";
        const std::string json = documentJson(kSchemaVersion10, idJson, R"("Untitled")",
                                              defaultColorSettingsJson(), defaultCompositionJson());
        expectDecodeFailure(expectations, json, DocumentDecodeError::InvalidId, "/project/id",
                            std::string("a project id spelled '") + std::string(spelling) + "' (" +
                                std::string(label) + ") is rejected as non-canonical");
    }
}

// ---------------------------------------------------------------------------------------------
// Duration
// ---------------------------------------------------------------------------------------------

void testRejectsUnreducedDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "20", "2", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::UnreducedRational, "/project/compositions/0/duration",
                        "an unreduced duration 20/2 is rejected");
}

void testRejectsZeroDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "0", "1", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::NonPositiveDuration,
                        "/project/compositions/0/duration",
                        "a zero duration is rejected as non-positive");
}

void testRejectsNegativeDuration(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "-5", "1", "1920", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::NonPositiveDuration,
                        "/project/compositions/0/duration",
                        "a negative duration is rejected as non-positive");
}

// ---------------------------------------------------------------------------------------------
// Pixel aspect
// ---------------------------------------------------------------------------------------------

void testRejectsUnreducedPixelAspect(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "1080", "2", "2", "24", "1");
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::UnreducedRational,
        "/project/compositions/0/format/pixelAspect", "an unreduced pixel aspect 2/2 is rejected");
}

// ---------------------------------------------------------------------------------------------
// Composition format domain: width/height/pixel product
// ---------------------------------------------------------------------------------------------

void testRejectsZeroWidth(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/width", "a zero width is rejected");
}

void testRejectsWidthOverCeiling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1048577", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/width",
                        "a width above the frozen 1048576 ceiling is rejected");
}

void testRejectsZeroHeight(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "0", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/height", "a zero height is rejected");
}

void testRejectsHeightOverCeiling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920", "1048577", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::DomainViolation,
                        "/project/compositions/0/format/height",
                        "a height above the frozen 1048576 ceiling is rejected");
}

void testRejectsPixelProductOverCeiling(Expectations& expectations) {
    // Both terms individually satisfy 1..1048576, but 65536 * 65537 = 4295032832 > 2^32.
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "65536", "65537", "1", "1", "24", "1");
    expectDecodeFailure(
        expectations, documentWithComposition(composition), DocumentDecodeError::DomainViolation,
        "/project/compositions/0/format",
        "a checked pixel product over 2^32 is rejected even though width and height "
        "each satisfy the per-dimension ceiling");
}

// ---------------------------------------------------------------------------------------------
// JSON-number uint32 spelling
// ---------------------------------------------------------------------------------------------

void testRejectsWidthFractionSpelling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "1920.0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled with a fraction (1920.0) is rejected as non-canonical");
}

void testRejectsWidthExponentSpelling(Expectations& expectations) {
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "192e1", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled with an exponent (192e1) is rejected as non-canonical");
}

void testRejectsWidthNegativeZeroSpelling(Expectations& expectations) {
    // "-0" is valid RFC 8259 JSON number syntax (unlike a genuine multi-digit leading zero such as
    // "01920", which the mandatory strict JSON preflight layer rejects before typed decode ever
    // sees it -- see strict_json_preflight_tests.cpp's InvalidNumber{"01", ...} case). Negative
    // zero is the reachable member of the same non-canonical-sign spelling family the contract
    // calls out explicitly: "JSON negative zero is invalid for an integer field."
    const auto composition =
        compositionJson(R"("1")", "Comp", "10", "1", "-0", "1080", "1", "1", "24", "1");
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::InvalidJsonUInt32,
                        "/project/compositions/0/format/width",
                        "a width spelled as negative zero (-0) is rejected as non-canonical");
}

// ---------------------------------------------------------------------------------------------
// OCIO digest spelling
// ---------------------------------------------------------------------------------------------

void testRejectsDigestWrongLength(Expectations& expectations) {
    const std::string shortDigest(kValidDigest.substr(0, kValidDigest.size() - 1));
    const std::string colorSettings = colorSettingsJson(shortDigest, "builtin", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::InvalidDigestSpelling,
                        "/project/colorSettings/ocioConfig/expectedRevision/digest",
                        "a 63-character digest is rejected for its wrong length");
}

void testRejectsDigestUppercaseHex(Expectations& expectations) {
    std::string upperDigest(kValidDigest.substr(0, kValidDigest.size() - 2));
    upperDigest += "1F";
    const std::string colorSettings = colorSettingsJson(upperDigest, "builtin", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::InvalidDigestSpelling,
                        "/project/colorSettings/ocioConfig/expectedRevision/digest",
                        "a digest containing an uppercase hex character is rejected");
}

// ---------------------------------------------------------------------------------------------
// OCIO portability/locator agreement
// ---------------------------------------------------------------------------------------------

void testRejectsPortabilityLocatorMismatch(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(kValidDigest, "external", "");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/portability",
                        "portability 'external' disagreeing with a builtin locator is rejected");
}

// ---------------------------------------------------------------------------------------------
// OCIO context variables
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedContextVariables(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(
        kValidDigest, "builtin", R"({"name":"b","value":"1"},{"name":"a","value":"2"})");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/contextVariables/1/name",
                        "context variables out of UTF-8 name order are rejected");
}

void testRejectsDuplicateContextVariables(Expectations& expectations) {
    const std::string colorSettings = colorSettingsJson(
        kValidDigest, "builtin", R"({"name":"a","value":"1"},{"name":"a","value":"2"})");
    expectDecodeFailure(expectations, documentWithColorSettings(colorSettings),
                        DocumentDecodeError::DomainViolation,
                        "/project/colorSettings/ocioConfig/contextVariables/1/name",
                        "a duplicate context variable name is rejected");
}

// ---------------------------------------------------------------------------------------------
// Compositions collection ordering
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedCompositions(Expectations& expectations) {
    const std::string compositions =
        defaultCompositionJson(R"("2")") + "," + defaultCompositionJson(R"("1")");
    expectDecodeFailure(expectations, documentWithComposition(compositions),
                        DocumentDecodeError::UnsortedCompositions, "/project/compositions/1/id",
                        "compositions out of ascending numeric id order are rejected");
}

void testRejectsDuplicateCompositions(Expectations& expectations) {
    const std::string compositions =
        defaultCompositionJson(R"("1")") + "," + defaultCompositionJson(R"("1")");
    expectDecodeFailure(expectations, documentWithComposition(compositions),
                        DocumentDecodeError::DuplicateComposition, "/project/compositions/1/id",
                        "two compositions declaring the same numeric id are rejected");
}

// ---------------------------------------------------------------------------------------------
// R2: the composition object is now closed (the R1 staging gap)
// ---------------------------------------------------------------------------------------------

void testRejectsClosedCompositionTrailingMember(Expectations& expectations) {
    const std::string valid = compositionWithInterior("[]", "[]", std::string(kMinimalGraphJson));
    const std::string composition = valid.substr(0, valid.size() - 1) + R"(,"bogus":null})";
    expectDecodeFailure(expectations, documentWithComposition(composition),
                        DocumentDecodeError::UnknownMember, "/project/compositions/0/bogus",
                        "a fully-formed composition with a trailing unrecognized member is now "
                        "rejected -- R1 accepted trailing members unvalidated");
}

// ---------------------------------------------------------------------------------------------
// Parameters: collection ordering and source discriminator
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::string boolConstantParameterJson(const std::string_view idJson) {
    std::string result = R"({"id":)";
    result += idJson;
    result += R"(,"schemaKey":"bloom.test","source":{"kind":"constant",)"
              R"("value":{"kind":"bool","value":true}}})";
    return result;
}

void testRejectsUnsortedParameters(Expectations& expectations) {
    const std::string parameters =
        "[" + boolConstantParameterJson(R"("2")") + "," + boolConstantParameterJson(R"("1")") + "]";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior(
                            parameters, "[]", std::string(kMinimalGraphJson))),
                        DocumentDecodeError::UnsortedParameters,
                        "/project/compositions/0/parameters/1/id",
                        "parameters out of ascending numeric id order are rejected");
}

void testRejectsDuplicateParameters(Expectations& expectations) {
    const std::string parameters =
        "[" + boolConstantParameterJson(R"("1")") + "," + boolConstantParameterJson(R"("1")") + "]";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior(
                            parameters, "[]", std::string(kMinimalGraphJson))),
                        DocumentDecodeError::DuplicateParameter,
                        "/project/compositions/0/parameters/1/id",
                        "two parameters declaring the same numeric id are rejected");
}

void testRejectsUnsupportedParameterSourceKind(Expectations& expectations) {
    const std::string parameters =
        R"([{"id":"1","schemaKey":"bloom.test","source":{"kind":"driver-binding",)"
        R"("driverId":"1"}}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior(
                            parameters, "[]", std::string(kMinimalGraphJson))),
                        DocumentDecodeError::UnsupportedParameterSource,
                        "/project/compositions/0/parameters/0/source/kind",
                        "a spelled-out 'driver-binding' parameter source kind is the deferred wire "
                        "vocabulary, reported as an unsupported source rather than fabricated");
}

// ---------------------------------------------------------------------------------------------
// Constant values: exact member order and kind
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::string parameterWithConstantValueJson(const std::string_view valueJson) {
    std::string result = R"({"id":"1","schemaKey":"bloom.test","source":{"kind":"constant",)"
                         R"("value":)";
    result += valueJson;
    result += "}}";
    return result;
}

void expectConstantValueRejected(Expectations& expectations, const std::string_view valueJson,
                                 const DocumentDecodeError expectedError,
                                 const std::string_view expectedPath,
                                 const std::string_view message) {
    const std::string parameters = "[" + parameterWithConstantValueJson(valueJson) + "]";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior(
                            parameters, "[]", std::string(kMinimalGraphJson))),
                        expectedError, expectedPath, message);
}

void testRejectsBoolValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"value":true,"kind":"bool"})", DocumentDecodeError::UnknownMember,
        "/project/compositions/0/parameters/0/source/value/value",
        "a bool constant value spelled with 'value' before 'kind' is rejected -- 'kind' must "
        "be literally first");
}

void testRejectsInt64ValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"value":"1","kind":"int64"})", DocumentDecodeError::UnknownMember,
        "/project/compositions/0/parameters/0/source/value/value",
        "an int64 constant value spelled with 'value' before 'kind' is rejected");
}

void testRejectsFloat64ValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"value":1.0,"kind":"float64"})", DocumentDecodeError::UnknownMember,
        "/project/compositions/0/parameters/0/source/value/value",
        "a float64 constant value spelled with 'value' before 'kind' is rejected");
}

void testRejectsStringValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"value":"text","kind":"string"})", DocumentDecodeError::UnknownMember,
        "/project/compositions/0/parameters/0/source/value/value",
        "a string constant value spelled with 'value' before 'kind' is rejected");
}

void testRejectsVec2ValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(expectations, R"({"kind":"vec2","y":0.0,"x":0.0})",
                                DocumentDecodeError::MemberOutOfOrder,
                                "/project/compositions/0/parameters/0/source/value/y",
                                "a vec2 constant value spelled with 'y' before 'x' is rejected");
}

void testRejectsColor4ValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"kind":"color4","green":0.0,"red":0.0,"blue":0.0,"alpha":1.0})",
        DocumentDecodeError::MemberOutOfOrder,
        "/project/compositions/0/parameters/0/source/value/green",
        "a color4 constant value spelled with 'green' before 'red' is rejected");
}

void testRejectsRationalValueWrongOrder(Expectations& expectations) {
    expectConstantValueRejected(
        expectations, R"({"kind":"rational","denominator":"1","numerator":"0"})",
        DocumentDecodeError::MemberOutOfOrder,
        "/project/compositions/0/parameters/0/source/value/denominator",
        "a rational constant value spelled with 'denominator' before 'numerator' is rejected");
}

void testRejectsUnknownConstantValueKind(Expectations& expectations) {
    expectConstantValueRejected(expectations, R"({"kind":"bogus","value":true})",
                                DocumentDecodeError::InvalidConstantValueKind,
                                "/project/compositions/0/parameters/0/source/value/kind",
                                "an unrecognized constant value kind is rejected");
}

void testRejectsNonFiniteFloat64(Expectations& expectations) {
    expectConstantValueRejected(expectations, R"({"kind":"float64","value":1e309})",
                                DocumentDecodeError::InvalidFloat64,
                                "/project/compositions/0/parameters/0/source/value/value",
                                "a float64 constant value that overflows to infinity is rejected");
}

// ---------------------------------------------------------------------------------------------
// Animation curves: collection ordering, kind, and keyframe shape
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedAnimationCurves(Expectations& expectations) {
    const std::string curves = R"([{"id":"2","kind":"scalar","keyframes":[{"id":"1",)"
                               R"("time":{"numerator":"0","denominator":"1"},"value":0.0,)"
                               R"("outgoingInterpolation":"linear"}]},)"
                               R"({"id":"1","kind":"scalar","keyframes":[{"id":"2",)"
                               R"("time":{"numerator":"0","denominator":"1"},"value":0.0,)"
                               R"("outgoingInterpolation":"linear"}]}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::UnsortedAnimationCurves,
                        "/project/compositions/0/animationCurves/1/id",
                        "animation curves out of ascending numeric id order are rejected");
}

void testRejectsDuplicateAnimationCurves(Expectations& expectations) {
    const std::string curveJson = R"({"id":"1","kind":"scalar","keyframes":[{"id":"1",)"
                                  R"("time":{"numerator":"0","denominator":"1"},"value":0.0,)"
                                  R"("outgoingInterpolation":"linear"}]})";
    const std::string curves = "[" + curveJson + "," + curveJson + "]";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::DuplicateAnimationCurve,
                        "/project/compositions/0/animationCurves/1/id",
                        "two animation curves declaring the same numeric id are rejected");
}

void testRejectsUnknownAnimationCurveKind(Expectations& expectations) {
    const std::string curves = R"([{"id":"1","kind":"bogus","keyframes":[]}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::InvalidAnimationCurveKind,
                        "/project/compositions/0/animationCurves/0/kind",
                        "an unrecognized animation curve kind is rejected");
}

void testRejectsEmptyKeyframes(Expectations& expectations) {
    const std::string curves = R"([{"id":"1","kind":"scalar","keyframes":[]}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::EmptyKeyframes,
                        "/project/compositions/0/animationCurves/0/keyframes",
                        "an animation curve with no keyframes is rejected");
}

void testRejectsEqualKeyframeTimes(Expectations& expectations) {
    const std::string curves =
        R"([{"id":"1","kind":"scalar","keyframes":[)"
        R"({"id":"1","time":{"numerator":"0","denominator":"1"},"value":0.0,)"
        R"("outgoingInterpolation":"hold"},)"
        R"({"id":"2","time":{"numerator":"0","denominator":"1"},"value":1.0,)"
        R"("outgoingInterpolation":"linear"}]}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::NonIncreasingKeyframeTime,
                        "/project/compositions/0/animationCurves/0/keyframes/1/time",
                        "two keyframes with an equal exact rational time are rejected");
}

void testRejectsDecreasingKeyframeTimes(Expectations& expectations) {
    const std::string curves =
        R"([{"id":"1","kind":"scalar","keyframes":[)"
        R"({"id":"1","time":{"numerator":"10","denominator":"1"},"value":0.0,)"
        R"("outgoingInterpolation":"hold"},)"
        R"({"id":"2","time":{"numerator":"5","denominator":"1"},"value":1.0,)"
        R"("outgoingInterpolation":"linear"}]}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(
                            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
                        DocumentDecodeError::NonIncreasingKeyframeTime,
                        "/project/compositions/0/animationCurves/0/keyframes/1/time",
                        "a keyframe time that decreases from the previous keyframe is rejected");
}

void testRejectsFinalKeyframeNotLinear(Expectations& expectations) {
    const std::string curves =
        R"([{"id":"1","kind":"scalar","keyframes":[)"
        R"({"id":"1","time":{"numerator":"0","denominator":"1"},"value":0.0,)"
        R"("outgoingInterpolation":"linear"},)"
        R"({"id":"2","time":{"numerator":"10","denominator":"1"},"value":1.0,)"
        R"("outgoingInterpolation":"hold"}]}])";
    expectDecodeFailure(
        expectations,
        documentWithComposition(
            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
        DocumentDecodeError::FinalKeyframeNotLinear,
        "/project/compositions/0/animationCurves/0/keyframes/1/outgoingInterpolation",
        "a final keyframe interpolation other than canonical linear is rejected");
}

void testRejectsUnknownInterpolation(Expectations& expectations) {
    const std::string curves =
        R"([{"id":"1","kind":"scalar","keyframes":[)"
        R"({"id":"1","time":{"numerator":"0","denominator":"1"},"value":0.0,)"
        R"("outgoingInterpolation":"bogus"}]}])";
    expectDecodeFailure(
        expectations,
        documentWithComposition(
            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
        DocumentDecodeError::InvalidInterpolation,
        "/project/compositions/0/animationCurves/0/keyframes/0/outgoingInterpolation",
        "an unrecognized interpolation spelling is rejected");
}

void testAcceptsVec2AnimationCurve(Expectations& expectations) {
    // An unreferenced vec2 curve is valid wire shape at this decode layer: curve ownership by
    // exactly one parameter is a later document-construction invariant, not checked here.
    const std::string curves = R"([{"id":"1","kind":"vec2","keyframes":[)"
                               R"({"id":"1","time":{"numerator":"0","denominator":"1"},)"
                               R"("value":{"x":0.0,"y":0.0},"outgoingInterpolation":"linear"}]}])";
    const auto decoded = decodeText(documentWithComposition(
        compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))));
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "a well-formed vec2 animation curve decodes successfully");
    if (decoded.value() != nullptr) {
        expectations.expect(decoded.value()->compositions.front().animationCurves.size() == 1,
                            "the decoded composition has exactly the one vec2 curve");
    }
}

// ---------------------------------------------------------------------------------------------
// Graph: node/edge/binding/layerOutput ordering, discriminators, and schemaVersion domain
// ---------------------------------------------------------------------------------------------

void testRejectsUnsortedNodes(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"2","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::UnsortedNodes, "/project/compositions/0/graph/nodes/1/id",
        "graph nodes out of ascending numeric id order are rejected");
}

void testRejectsDuplicateNodes(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::DuplicateNode, "/project/compositions/0/graph/nodes/1/id",
        "two graph nodes declaring the same numeric id are rejected");
}

void testRejectsSchemaVersionZero(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":0,"parameters":[]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::DomainViolation, "/project/compositions/0/graph/nodes/0/schemaVersion",
        "a node schemaVersion of zero is rejected");
}

void testRejectsUnsortedBindings(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[)"
        R"({"role":"position","parameterId":"1"},{"role":"color","parameterId":"2"}]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::UnsortedBindings,
                        "/project/compositions/0/graph/nodes/0/parameters/1/role",
                        "parameter bindings out of UTF-8 role order are rejected");
}

void testRejectsDuplicateBindings(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[)"
        R"({"role":"color","parameterId":"1"},{"role":"color","parameterId":"2"}]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DuplicateBinding,
                        "/project/compositions/0/graph/nodes/0/parameters/1/role",
                        "two parameter bindings on the same node declaring the same role are "
                        "rejected");
}

void testRejectsUnsortedEdges(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"2","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"2","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"node-input","nodeId":"2","port":"image"}},)"
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"node-input","nodeId":"2","port":"image"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::UnsortedEdges, "/project/compositions/0/graph/edges/1/id",
        "graph edges out of ascending numeric id order are rejected");
}

void testRejectsDuplicateEdges(Expectations& expectations) {
    const std::string edgeJson =
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"node-input","nodeId":"2","port":"image"}})";
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"2","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)" +
        edgeJson + "," + edgeJson +
        R"(],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::DuplicateEdge, "/project/compositions/0/graph/edges/1/id",
        "two graph edges declaring the same numeric id are rejected");
}

void testRejectsUnknownEdgeDestinationKind(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"2","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"bogus","nodeId":"2","port":"image"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::InvalidEdgeDestinationKind,
                        "/project/compositions/0/graph/edges/0/destination/kind",
                        "an unrecognized edge destination kind is rejected");
}

void testRejectsUnsortedLayerOutputs(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.layer-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],)"
        R"("layerOutputs":[)"
        R"({"nodeId":"1","layerId":"2","name":"B","outputPort":"image"},)"
        R"({"nodeId":"1","layerId":"1","name":"A","outputPort":"image"}],)"
        R"("layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::UnsortedLayerOutputs,
                        "/project/compositions/0/graph/layerOutputs/1/layerId",
                        "Layer Output boundaries out of (layerId, nodeId) order are rejected");
}

void testRejectsDuplicateLayerOutputs(Expectations& expectations) {
    const std::string boundaryJson =
        R"({"nodeId":"1","layerId":"1","name":"A","outputPort":"image"})";
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.layer-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],)"
        R"("layerOutputs":[)" +
        boundaryJson + "," + boundaryJson +
        R"(],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DuplicateLayerOutput,
                        "/project/compositions/0/graph/layerOutputs/1/layerId",
                        "two Layer Output boundaries declaring the same (layerId, nodeId) pair "
                        "are rejected");
}

void testAcceptsUnsortedLayerStackEntries(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.layer-stack","schemaVersion":1,"parameters":[]},)"
        R"({"id":"2","typeId":"bloom.layer-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"3","typeId":"bloom.layer-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"4","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],)"
        R"("layerOutputs":[)"
        R"({"nodeId":"2","layerId":"1","name":"A","outputPort":"image"},)"
        R"({"nodeId":"3","layerId":"2","name":"B","outputPort":"image"}],)"
        R"("layerStack":{"nodeId":"1","entries":[)"
        R"({"slotId":"2","layerId":"2"},{"slotId":"1","layerId":"1"}]},)"
        R"("compositionOutput":{"nodeId":"4","port":"image"}})";
    const auto decoded =
        decodeText(documentWithComposition(compositionWithInterior("[]", "[]", graph)));
    expectations.expect(static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "Layer Stack entries in reversed (non-ascending) slot order decode "
                        "successfully -- entries are never sorted");
    if (decoded.value() != nullptr) {
        const auto& entries = decoded.value()->compositions.front().graph.layerStack.entries;
        expectations.expect(entries.size() == 2 && entries[0].slotId.value() == 2 &&
                                entries[1].slotId.value() == 1,
                            "the decoded Layer Stack entries preserve exact source order, not "
                            "ascending slot order");
    }
}

// ---------------------------------------------------------------------------------------------
// Cross-reference checks within one decoded composition
// ---------------------------------------------------------------------------------------------

void testRejectsDanglingCurveId(Expectations& expectations) {
    const std::string parameters =
        R"([{"id":"1","schemaKey":"bloom.test","source":{"kind":"animation-curve",)"
        R"("curveId":"99"}}])";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior(
                            parameters, "[]", std::string(kMinimalGraphJson))),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/parameters/0/source/curveId",
                        "a parameter's animation-curve source naming a curve id absent from "
                        "animationCurves is rejected");
}

void testRejectsDanglingParameterId(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[)"
        R"({"role":"color","parameterId":"99"}]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/nodes/0/parameters/0/parameterId",
                        "a node parameter binding naming a parameter id absent from parameters "
                        "is rejected");
}

void testRejectsDanglingEdgeSourceNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"1","source":{"nodeId":"99","port":"image"},)"
        R"("destination":{"kind":"node-input","nodeId":"1","port":"image"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/edges/0/source/nodeId",
                        "an edge source naming a node id absent from graph.nodes is rejected");
}

void testRejectsDanglingEdgeDestinationNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"node-input","nodeId":"99","port":"image"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/edges/0/destination/nodeId",
                        "a node-input edge destination naming a node id absent from graph.nodes "
                        "is rejected");
}

void testRejectsDanglingEdgeDestinationStackNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"layer-stack-input","stackNodeId":"99","slotId":"1",)"
        R"("role":"content"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/edges/0/destination/stackNodeId",
                        "a layer-stack-input edge destination naming a stack node id absent from "
                        "graph.nodes is rejected");
}

void testRejectsDanglingLayerOutputNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],)"
        R"("layerOutputs":[)"
        R"({"nodeId":"99","layerId":"1","name":"A","outputPort":"image"}],)"
        R"("layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/layerOutputs/0/nodeId",
                        "a Layer Output boundary naming a node id absent from graph.nodes is "
                        "rejected");
}

void testRejectsDanglingLayerStackNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"99","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectDecodeFailure(
        expectations, documentWithComposition(compositionWithInterior("[]", "[]", graph)),
        DocumentDecodeError::DanglingReference, "/project/compositions/0/graph/layerStack/nodeId",
        "a Layer Stack naming an owning node id absent from graph.nodes is "
        "rejected");
}

void testRejectsDanglingCompositionOutputNode(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[],"layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"99","port":"image"}})";
    expectDecodeFailure(expectations,
                        documentWithComposition(compositionWithInterior("[]", "[]", graph)),
                        DocumentDecodeError::DanglingReference,
                        "/project/compositions/0/graph/compositionOutput/nodeId",
                        "a compositionOutput naming a node id absent from graph.nodes is "
                        "rejected");
}

// ---------------------------------------------------------------------------------------------
// RT1: unknown-additive-member capture for a same-major newer-minor document (see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation" and
// src/project/include/bloom/project/round_trip_state.hpp).
// ---------------------------------------------------------------------------------------------

// Builds a schemaVersion-{schemaVersionJson} document with an unknown trailing member on the
// root, project, a composition, that composition's format, a graph node, a layer-stack entry, and
// (via a non-empty extensions array) an extension record -- one member captured/rejected at every
// attachment kind the task calls out by name. The extension record's high-water is set to match.
[[nodiscard]] std::string everywhereUnknownsDocument(const std::string_view schemaVersionJson) {
    const std::string extensionRecordJson =
        R"({"id":"1","ownerId":"vendor.module","typeId":"vendor.module.record-type",)"
        R"("schemaVersion":{"major":1,"minor":0},"subject":null,)"
        R"("mediaType":"application/octet-stream","referencePolicy":{"kind":"none"},)"
        R"("payload":"AA==","zzzExtExtra":1.5})";

    const std::string graphJson =
        R"({"nodes":[{"id":"1","typeId":"bloom.composition-output","schemaVersion":1,)"
        R"("parameters":[],"zzzNodeExtra":null}],)"
        R"("edges":[],"layerOutputs":[],)"
        R"("layerStack":{"nodeId":"1","entries":[)"
        R"({"slotId":"1","layerId":"1","zzzEntryExtra":"slot-note"}]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";

    const std::string formatJson =
        R"({"width":1920,"height":1080,"pixelAspect":{"numerator":"1","denominator":"1"},)"
        R"("frameRate":{"numerator":"24","denominator":"1"},)"
        R"("zzzFormatExtra":{"list":[1,2,"three",[4,5]],"note":"deep"}})";

    const std::string compositionJsonText =
        R"({"id":"1","name":"Comp","duration":{"numerator":"10","denominator":"1"},"format":)" +
        formatJson + R"(,"parameters":[],"animationCurves":[],"graph":)" + graphJson +
        R"(,"zzzCompExtra":true})";

    std::string document = "{\"schemaVersion\":";
    document += schemaVersionJson;
    document += R"(,"project":{"id":"1","name":"Untitled","colorSettings":)";
    document += defaultColorSettingsJson();
    document += R"(,"compositions":[)";
    document += compositionJsonText;
    document +=
        R"(],"zzzProjectExtra":"hello world"},)"
        R"("idAllocation":{"highestIssued":{"composition":"0","node":"0","edge":"0",)"
        R"("layer":"0","layerSlot":"0","parameter":"0","animationCurve":"0","keyframe":"0",)"
        R"("driverBinding":"0","extensionRecord":"1"}},)"
        R"("extensions":[)";
    document += extensionRecordJson;
    document += R"(],"zzzFutureField":42})";
    return document;
}

void testAcceptsMinor1WithUnknownMembersEverywhere(Expectations& expectations) {
    const auto decoded = decodeText(everywhereUnknownsDocument(kSchemaVersion11));
    expectations.expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "a 1.1 document with unknown trailing members everywhere still decodes");
    expectations.expect(
        decoded.classification() == DocumentClassification::EditableWithRoundTrip,
        "a 1.1 document with unknown trailing members classifies EditableWithRoundTrip");
    if (decoded.roundTrip() == nullptr) {
        expectations.expect(false, "RoundTripState is present for the everywhere-unknowns fixture");
        return;
    }
    const auto& roundTrip = *decoded.roundTrip();
    expectations.expect(roundTrip.size() == 7,
                        "exactly seven attachment points were captured (root, project, "
                        "composition, format, node, extension record, layer-stack entry)");

    // root: a schema path is the empty attachment path.
    {
        const auto* members = roundTrip.find({});
        expectations.expect(members != nullptr && members->size() == 1,
                            "the root attachment point retained exactly one member");
        if (members != nullptr && members->size() == 1) {
            expectations.expect((*members)[0].key() == "zzzFutureField",
                                "the root retained member's key is exact");
            const auto& value = (*members)[0].value();
            expectations.expect(value.kind() == RetainedJsonValueKind::Number &&
                                    value.asNumber().kind() == UnknownJsonNumberKind::Integer &&
                                    value.asNumber().integerValue() == 42,
                                "the root retained member's lossless integer value decodes "
                                "exactly");
        }
    }

    // project
    {
        const RoundTripAttachmentPath path{RoundTripPathSegment::named("project")};
        const auto* members = roundTrip.find(path);
        expectations.expect(members != nullptr && members->size() == 1 &&
                                (*members)[0].key() == "zzzProjectExtra" &&
                                (*members)[0].value().kind() == RetainedJsonValueKind::String &&
                                (*members)[0].value().asString() == "hello world",
                            "the project attachment point retained its exact string member");
    }

    // composition: keyed by numeric CompositionId, never array position.
    {
        const RoundTripAttachmentPath path{
            RoundTripPathSegment::named("project"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1")};
        const auto* members = roundTrip.find(path);
        expectations.expect(members != nullptr && members->size() == 1 &&
                                (*members)[0].key() == "zzzCompExtra" &&
                                (*members)[0].value().kind() == RetainedJsonValueKind::Boolean &&
                                (*members)[0].value().asBoolean(),
                            "the composition attachment point retained its exact boolean member");
    }

    // format: a singleton nested under its owning composition's identity. Its retained value is
    // also this suite's deep-nested-subtree case: an object containing arrays, including a
    // doubly-nested array.
    {
        const RoundTripAttachmentPath path{
            RoundTripPathSegment::named("project"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
            RoundTripPathSegment::named("format")};
        const auto* members = roundTrip.find(path);
        expectations.expect(members != nullptr && members->size() == 1 &&
                                (*members)[0].key() == "zzzFormatExtra",
                            "the format attachment point retained its exact member key");
        if (members != nullptr && members->size() == 1) {
            const auto& value = (*members)[0].value();
            expectations.expect(value.kind() == RetainedJsonValueKind::Object,
                                "the format retained value is a nested object");
            const auto* list = value.findMember("list");
            const auto* note = value.findMember("note");
            expectations.expect(list != nullptr && list->kind() == RetainedJsonValueKind::Array &&
                                    list->elements().size() == 4,
                                "the nested object's array member retained all four elements");
            if (list != nullptr && list->elements().size() == 4) {
                expectations.expect(
                    list->elements()[0].kind() == RetainedJsonValueKind::Number &&
                        list->elements()[0].asNumber().integerValue() == 1 &&
                        list->elements()[1].kind() == RetainedJsonValueKind::Number &&
                        list->elements()[1].asNumber().integerValue() == 2,
                    "the nested array's leading integers retain their exact values");
                expectations.expect(list->elements()[2].kind() == RetainedJsonValueKind::String &&
                                        list->elements()[2].asString() == "three",
                                    "the nested array's string element retains exactly");
                const auto& innerArray = list->elements()[3];
                expectations.expect(innerArray.kind() == RetainedJsonValueKind::Array &&
                                        innerArray.elements().size() == 2 &&
                                        innerArray.elements()[0].asNumber().integerValue() == 4 &&
                                        innerArray.elements()[1].asNumber().integerValue() == 5,
                                    "the doubly-nested array retains exactly");
            }
            expectations.expect(note != nullptr && note->kind() == RetainedJsonValueKind::String &&
                                    note->asString() == "deep",
                                "the nested object's string member retains exactly");
        }
    }

    // node: keyed by numeric NodeId, nested under composition/graph.
    {
        const RoundTripAttachmentPath path{
            RoundTripPathSegment::named("project"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
            RoundTripPathSegment::named("graph"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Node, "1")};
        const auto* members = roundTrip.find(path);
        expectations.expect(members != nullptr && members->size() == 1 &&
                                (*members)[0].key() == "zzzNodeExtra" &&
                                (*members)[0].value().kind() == RetainedJsonValueKind::Null,
                            "the node attachment point retained its exact null member");
    }

    // extension record: keyed by numeric ExtensionRecordId, directly off the document root (no
    // owning composition).
    {
        const RoundTripAttachmentPath path{
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::ExtensionRecord, "1")};
        const auto* members = roundTrip.find(path);
        expectations.expect(
            members != nullptr && members->size() == 1 && (*members)[0].key() == "zzzExtExtra" &&
                (*members)[0].value().kind() == RetainedJsonValueKind::Number &&
                (*members)[0].value().asNumber().kind() == UnknownJsonNumberKind::Float64,
            "the extension record attachment point retained its exact float64 "
            "member");
        if (members != nullptr && members->size() == 1) {
            const auto bits = (*members)[0].value().asNumber().float64Bits();
            expectations.expect(bits.has_value() && *bits == std::bit_cast<std::uint64_t>(1.5),
                                "the extension record's retained float64 bits are exact");
        }
    }

    // layer-stack entry: keyed by LayerSlotId, nested under composition/graph/layerStack.
    {
        const RoundTripAttachmentPath path{
            RoundTripPathSegment::named("project"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
            RoundTripPathSegment::named("graph"), RoundTripPathSegment::named("layerStack"),
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::LayerStackEntry, "1")};
        const auto* members = roundTrip.find(path);
        expectations.expect(members != nullptr && members->size() == 1 &&
                                (*members)[0].key() == "zzzEntryExtra" &&
                                (*members)[0].value().kind() == RetainedJsonValueKind::String &&
                                (*members)[0].value().asString() == "slot-note",
                            "the layer-stack entry attachment point retained its exact string "
                            "member");
    }
}

// The exact same unknown members, unchanged, at exact schemaVersion {1,0}: the 1.0 writer never
// emits an unknown additive member, so this still hard-rejects at the first one reached (the
// root's own "zzzFutureField", since the root's own closed-shape check runs before project is
// ever descended into).
void testRejects1_0WithSameUnknownMembersEverywhere(Expectations& expectations) {
    expectDecodeFailure(expectations, everywhereUnknownsDocument(kSchemaVersion10),
                        DocumentDecodeError::UnknownMember, "/zzzFutureField",
                        "the same everywhere-unknowns fixture at exact schemaVersion 1.0 is "
                        "rejected at its first unknown member");
}

// An unknown major version is rejected without mutation regardless of minor or how many unknown
// additive members the document also carries (see docs/architecture/project-format.md, "Versions,
// Migrations, And Preservation": "unknown major versions are rejected without mutation").
void testRejectsMajor2WithUnknownMembersEverywhere(Expectations& expectations) {
    const auto decoded = decodeText(everywhereUnknownsDocument(R"({"major":2,"minor":1})"));
    expectations.expect(!decoded && decoded.outcome() == DocumentDecodeOutcome::Failed &&
                            decoded.error() == DocumentDecodeError::DomainViolation &&
                            decoded.path() == "/schemaVersion" && decoded.value() == nullptr &&
                            decoded.roundTrip() == nullptr,
                        "an unrecognized major version 2 is rejected without constructing an "
                        "envelope or RoundTripState even with unknown members present");
}

// An unknown member strictly before, or between, the known members of a closed object remains a
// hard decode error at every minor version -- a conforming {1, minor>0} writer could never
// produce one (see docs/architecture/project-format.md, "Canonical Document Shape": "Retained
// unknown additive members follow all known members of their object").
void testRejectsUnknownMemberBeforeOrBetweenKnownMembersIn1_1(Expectations& expectations) {
    // before every known member of a composition object
    {
        const std::string composition = R"({"aaa":true,"id":"1","name":"Comp",)"
                                        R"("duration":{"numerator":"10","denominator":"1"},)"
                                        R"("format":{"width":1920,"height":1080,)"
                                        R"("pixelAspect":{"numerator":"1","denominator":"1"},)"
                                        R"("frameRate":{"numerator":"24","denominator":"1"}},)"
                                        R"("parameters":[],"animationCurves":[],"graph":)" +
                                        std::string(kMinimalGraphJson) + "}";
        expectDecodeFailure(expectations, documentWithCompositionMinor1(composition),
                            DocumentDecodeError::UnknownMember, "/project/compositions/0/aaa",
                            "an unknown member before every known composition member is "
                            "rejected in a 1.1 document");
    }
    // between two known root members (project, then idAllocation)
    {
        const std::string project =
            std::string("{\"id\":\"1\",\"name\":\"Untitled\",\"colorSettings\":") +
            defaultColorSettingsJson() + ",\"compositions\":[" + defaultCompositionJson() + "]}";
        const std::string json = std::string("{\"schemaVersion\":") +
                                 std::string(kSchemaVersion11) + ",\"project\":" + project +
                                 R"(,"unknownMid":null,"idAllocation":{"highestIssued":)"
                                 R"({"composition":"0","node":"0","edge":"0","layer":"0",)"
                                 R"("layerSlot":"0","parameter":"0","animationCurve":"0",)"
                                 R"("keyframe":"0","driverBinding":"0","extensionRecord":"0"}},)"
                                 R"("extensions":[]})";
        expectDecodeFailure(expectations, json, DocumentDecodeError::UnknownMember, "/unknownMid",
                            "an unknown member between two known root members is rejected in a "
                            "1.1 document");
    }
}

// Trailing unknown members must themselves be in strictly ascending UTF-8 key order; a conforming
// writer always emits them that way (see docs/architecture/project-format.md, "Canonical Document
// Shape"). A literal duplicate trailing key cannot reach this module at all -- the strict JSON
// reader already rejects a duplicate decoded object key structurally, before decodeDocumentEnvelope
// ever runs -- so this exercises the reachable half of the contract's "reject unsorted or
// duplicate" rule; the strict-ascending comparison this module performs also covers an equal
// adjacent key, were one ever reachable.
void testRejectsUnsortedTrailingUnknownMembers(Expectations& expectations) {
    const std::string valid = compositionWithInterior("[]", "[]", std::string(kMinimalGraphJson));
    const std::string composition = valid.substr(0, valid.size() - 1) + R"(,"zzzB":1,"zzzA":2})";
    expectDecodeFailure(expectations, documentWithCompositionMinor1(composition),
                        DocumentDecodeError::UnsortedUnknownMember, "/project/compositions/0/zzzA",
                        "two trailing unknown composition members out of ascending key order "
                        "are rejected in a 1.1 document");
}

// A context-variable array element has no member in the contract's fixed collection identity
// list, so it never captures a trailing unknown member -- even at documentMinor > 0 (see
// docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "An array
// without a declared identity cannot retain unknown elements through an edit").
void testRejectsContextVariableTrailingMemberEvenAtMinor1(Expectations& expectations) {
    const std::string colorSettings =
        colorSettingsJson(kValidDigest, "builtin", R"({"name":"a","value":"1","extra":true})");
    expectDecodeFailure(expectations, documentWithColorSettingsMinor1(colorSettings),
                        DocumentDecodeError::UnknownMember,
                        "/project/colorSettings/ocioConfig/contextVariables/0/extra",
                        "a trailing unknown member on a context-variable entry is rejected even "
                        "in a 1.1 document, because context variables have no declared identity");
}

// ---------------------------------------------------------------------------------------------
// RT1: unknown core discriminators classify PreservedReadOnlyRequired rather than hard-failing or
// being captured -- the discriminator string itself is core vocabulary, never a retained value
// (see docs/architecture/project-format.md, "Versions, Migrations, And Preservation": "unknown
// core discriminators are never guessed").
// ---------------------------------------------------------------------------------------------

void testUnknownOcioLocatorKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string colorSettings =
        R"({"schemaVersion":{"major":1,"minor":0},"processColorSpaceId":"lin_rec709_scene",)"
        R"("ocioConfig":{"schemaVersion":{"major":1,"minor":0},)"
        R"("locator":{"kind":"bogus-locator","uri":"whatever"},)"
        R"("expectedRevision":{"algorithm":"sha256","digest":")" +
        std::string(kValidDigest) + R"("},"portability":"builtin","contextVariables":[]}})";
    expectPreservedReadOnly(expectations, documentWithColorSettingsMinor1(colorSettings),
                            RoundTripPreservationReason::UnknownDiscriminatorKind,
                            "/project/colorSettings/ocioConfig/locator/kind",
                            "an unrecognized OCIO locator kind in a 1.1 document is "
                            "PreservedReadOnlyRequired rather than a hard error");
}

void testUnknownParameterSourceKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string parameters =
        R"([{"id":"1","schemaKey":"bloom.test","source":{"kind":"driver-binding",)"
        R"("driverId":"1"}}])";
    expectPreservedReadOnly(
        expectations,
        documentWithCompositionMinor1(
            compositionWithInterior(parameters, "[]", std::string(kMinimalGraphJson))),
        RoundTripPreservationReason::UnknownDiscriminatorKind,
        "/project/compositions/0/parameters/0/source/kind",
        "the deferred 'driver-binding' parameter source kind in a 1.1 document is "
        "PreservedReadOnlyRequired rather than a hard error");
}

void testUnknownConstantValueKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string parameters =
        R"([{"id":"1","schemaKey":"bloom.test","source":{"kind":"constant",)"
        R"("value":{"kind":"bogus-value","value":true}}}])";
    expectPreservedReadOnly(
        expectations,
        documentWithCompositionMinor1(
            compositionWithInterior(parameters, "[]", std::string(kMinimalGraphJson))),
        RoundTripPreservationReason::UnknownDiscriminatorKind,
        "/project/compositions/0/parameters/0/source/value/kind",
        "an unrecognized constant value kind in a 1.1 document is PreservedReadOnlyRequired "
        "rather than a hard error");
}

void testUnknownAnimationCurveKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string curves = R"([{"id":"1","kind":"bogus-curve","keyframes":[]}])";
    expectPreservedReadOnly(
        expectations,
        documentWithCompositionMinor1(
            compositionWithInterior("[]", curves, std::string(kMinimalGraphJson))),
        RoundTripPreservationReason::UnknownDiscriminatorKind,
        "/project/compositions/0/animationCurves/0/kind",
        "an unrecognized animation curve kind in a 1.1 document is PreservedReadOnlyRequired "
        "rather than a hard error");
}

void testUnknownEdgeDestinationKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string graph =
        R"({"nodes":[)"
        R"({"id":"1","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]},)"
        R"({"id":"2","typeId":"bloom.composition-output","schemaVersion":1,"parameters":[]}],)"
        R"("edges":[)"
        R"({"id":"1","source":{"nodeId":"1","port":"image"},)"
        R"("destination":{"kind":"bogus-destination","nodeId":"2","port":"image"}}],)"
        R"("layerOutputs":[],"layerStack":{"nodeId":"1","entries":[]},)"
        R"("compositionOutput":{"nodeId":"1","port":"image"}})";
    expectPreservedReadOnly(
        expectations, documentWithCompositionMinor1(compositionWithInterior("[]", "[]", graph)),
        RoundTripPreservationReason::UnknownDiscriminatorKind,
        "/project/compositions/0/graph/edges/0/destination/kind",
        "an unrecognized edge destination kind in a 1.1 document is PreservedReadOnlyRequired "
        "rather than a hard error");
}

void testUnknownReferencePolicyKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string extensionRecordJson =
        R"({"id":"1","ownerId":"vendor.module","typeId":"vendor.module.record-type",)"
        R"("schemaVersion":{"major":1,"minor":0},"subject":null,)"
        R"("mediaType":"application/octet-stream",)"
        R"("referencePolicy":{"kind":"bogus-policy"},"payload":"AA=="})";
    expectPreservedReadOnly(
        expectations, documentWithExtensions(kSchemaVersion11, extensionRecordJson, "1"),
        RoundTripPreservationReason::UnknownDiscriminatorKind, "/extensions/0/referencePolicy/kind",
        "an unrecognized extension reference-policy kind in a 1.1 document is "
        "PreservedReadOnlyRequired rather than a hard error");
}

void testUnknownExtensionSubjectKindIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string extensionRecordJson =
        R"({"id":"1","ownerId":"vendor.module","typeId":"vendor.module.record-type",)"
        R"("schemaVersion":{"major":1,"minor":0},"subject":{"kind":"bogus-target","id":"1"},)"
        R"("mediaType":"application/octet-stream","referencePolicy":{"kind":"none"},)"
        R"("payload":"AA=="})";
    expectPreservedReadOnly(
        expectations, documentWithExtensions(kSchemaVersion11, extensionRecordJson, "1"),
        RoundTripPreservationReason::UnknownDiscriminatorKind, "/extensions/0/subject/kind",
        "an unrecognized extension subject target kind in a 1.1 document is "
        "PreservedReadOnlyRequired rather than a hard error");
}

// ---------------------------------------------------------------------------------------------
// RT1: an unknown number outside the lossless editable subset also classifies
// PreservedReadOnlyRequired instead of hard-failing (see
// docs/architecture/project-format.md, "Unknown JSON Numbers").
// ---------------------------------------------------------------------------------------------

void testUnknownNumberOverflowIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string valid = compositionWithInterior("[]", "[]", std::string(kMinimalGraphJson));
    const std::string composition = valid.substr(0, valid.size() - 1) + R"(,"zzzBigNumber":1e999})";
    expectPreservedReadOnly(expectations, documentWithCompositionMinor1(composition),
                            RoundTripPreservationReason::UnknownNumberOutOfSubset,
                            "/project/compositions/0/zzzBigNumber",
                            "an unknown trailing member number that overflows binary64 in a 1.1 "
                            "document is PreservedReadOnlyRequired rather than a hard error");
}

void testUnknownNumberNonCanonicalSpellingIsPreservedReadOnlyAt1_1(Expectations& expectations) {
    const std::string valid = compositionWithInterior("[]", "[]", std::string(kMinimalGraphJson));
    // "1E5" is syntactically valid JSON but not Bloom's canonical lowercase-'e' Float64 spelling
    // (compare unknown_json_number_tests.cpp's own "1E+21" NonCanonical fixture), so it falls
    // outside the lossless unknown-number subset exactly like an out-of-range value does.
    const std::string composition = valid.substr(0, valid.size() - 1) + R"(,"zzzOddNumber":1E5})";
    expectPreservedReadOnly(expectations, documentWithCompositionMinor1(composition),
                            RoundTripPreservationReason::UnknownNumberOutOfSubset,
                            "/project/compositions/0/zzzOddNumber",
                            "an unknown trailing member number with a non-canonical exponent "
                            "spelling in a 1.1 document is PreservedReadOnlyRequired rather than "
                            "a hard error");
}

} // namespace

int main() try {
    Expectations expectations;

    testBaselineDecodesSuccessfully(expectations);
    testFullRoundTrip(expectations);

    testRejectsRootMemberOrder(expectations);
    testRejectsRootUnknownMember(expectations);
    testRejectsProjectMemberOrder(expectations);
    testRejectsCompositionMemberOrder(expectations);
    testRejectsFormatMemberOrder(expectations);
    testRejectsCompositionMissingFormat(expectations);
    testRejectsProjectIdWrongKind(expectations);

    testAcceptsSchemaVersionMinor11WithoutUnknownMembers(expectations);
    testExactSchemaV1_0ProducesNoRoundTripState(expectations);
    testRejectsSchemaVersionMajor2(expectations);

    testRejectsProjectIdSpellings(expectations);

    testRejectsUnreducedDuration(expectations);
    testRejectsZeroDuration(expectations);
    testRejectsNegativeDuration(expectations);

    testRejectsUnreducedPixelAspect(expectations);

    testRejectsZeroWidth(expectations);
    testRejectsWidthOverCeiling(expectations);
    testRejectsZeroHeight(expectations);
    testRejectsHeightOverCeiling(expectations);
    testRejectsPixelProductOverCeiling(expectations);

    testRejectsWidthFractionSpelling(expectations);
    testRejectsWidthExponentSpelling(expectations);
    testRejectsWidthNegativeZeroSpelling(expectations);

    testRejectsDigestWrongLength(expectations);
    testRejectsDigestUppercaseHex(expectations);

    testRejectsPortabilityLocatorMismatch(expectations);

    testRejectsUnsortedContextVariables(expectations);
    testRejectsDuplicateContextVariables(expectations);

    testRejectsUnsortedCompositions(expectations);
    testRejectsDuplicateCompositions(expectations);

    testComposedRoundTrip(expectations);

    testRejectsClosedCompositionTrailingMember(expectations);

    testRejectsUnsortedParameters(expectations);
    testRejectsDuplicateParameters(expectations);
    testRejectsUnsupportedParameterSourceKind(expectations);

    testRejectsBoolValueWrongOrder(expectations);
    testRejectsInt64ValueWrongOrder(expectations);
    testRejectsFloat64ValueWrongOrder(expectations);
    testRejectsStringValueWrongOrder(expectations);
    testRejectsVec2ValueWrongOrder(expectations);
    testRejectsColor4ValueWrongOrder(expectations);
    testRejectsRationalValueWrongOrder(expectations);
    testRejectsUnknownConstantValueKind(expectations);
    testRejectsNonFiniteFloat64(expectations);

    testRejectsUnsortedAnimationCurves(expectations);
    testRejectsDuplicateAnimationCurves(expectations);
    testRejectsUnknownAnimationCurveKind(expectations);
    testRejectsEmptyKeyframes(expectations);
    testRejectsEqualKeyframeTimes(expectations);
    testRejectsDecreasingKeyframeTimes(expectations);
    testRejectsFinalKeyframeNotLinear(expectations);
    testRejectsUnknownInterpolation(expectations);
    testAcceptsVec2AnimationCurve(expectations);

    testRejectsUnsortedNodes(expectations);
    testRejectsDuplicateNodes(expectations);
    testRejectsSchemaVersionZero(expectations);
    testRejectsUnsortedBindings(expectations);
    testRejectsDuplicateBindings(expectations);
    testRejectsUnsortedEdges(expectations);
    testRejectsDuplicateEdges(expectations);
    testRejectsUnknownEdgeDestinationKind(expectations);
    testRejectsUnsortedLayerOutputs(expectations);
    testRejectsDuplicateLayerOutputs(expectations);
    testAcceptsUnsortedLayerStackEntries(expectations);

    testRejectsDanglingCurveId(expectations);
    testRejectsDanglingParameterId(expectations);
    testRejectsDanglingEdgeSourceNode(expectations);
    testRejectsDanglingEdgeDestinationNode(expectations);
    testRejectsDanglingEdgeDestinationStackNode(expectations);
    testRejectsDanglingLayerOutputNode(expectations);
    testRejectsDanglingLayerStackNode(expectations);
    testRejectsDanglingCompositionOutputNode(expectations);

    testAcceptsMinor1WithUnknownMembersEverywhere(expectations);
    testRejects1_0WithSameUnknownMembersEverywhere(expectations);
    testRejectsMajor2WithUnknownMembersEverywhere(expectations);
    testRejectsUnknownMemberBeforeOrBetweenKnownMembersIn1_1(expectations);
    testRejectsUnsortedTrailingUnknownMembers(expectations);
    testRejectsContextVariableTrailingMemberEvenAtMinor1(expectations);

    testUnknownOcioLocatorKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownParameterSourceKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownConstantValueKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownAnimationCurveKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownEdgeDestinationKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownReferencePolicyKindIsPreservedReadOnlyAt1_1(expectations);
    testUnknownExtensionSubjectKindIsPreservedReadOnlyAt1_1(expectations);

    testUnknownNumberOverflowIsPreservedReadOnlyAt1_1(expectations);
    testUnknownNumberNonCanonicalSpellingIsPreservedReadOnlyAt1_1(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " expectation(s) failed\n";
        return 1;
    }
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "Unexpected test exception: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "Unexpected non-standard test exception\n";
    return 1;
}
