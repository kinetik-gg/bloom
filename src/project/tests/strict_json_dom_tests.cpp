#include <bloom/project/strict_json_dom.hpp>

#include "strict_json_preflight.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/project_io_memory.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
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

using bloom::project::JsonValue;
using bloom::project::JsonValueKind;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::StrictJsonDomError;
using bloom::project::StrictJsonDomLimits;
using bloom::project::StrictJsonDomResult;

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

[[nodiscard]] StrictJsonDomResult parseGenerous(const std::string_view text,
                                                const StrictJsonDomLimits& limits = {}) {
    return bloom::project::parseStrictJsonDom(asBytes(text), limits,
                                              makeOperation(kGenerousOperationBudget));
}

// ---------------------------------------------------------------------------------------------
// Duplicate-key rejection
// ---------------------------------------------------------------------------------------------

void testDuplicateKeyRejection(Expectations& expectations) {
    {
        const auto result = parseGenerous(R"({"a":1,"a":2})");
        expectations.expect(!result && result.error() == StrictJsonDomError::DuplicateObjectKey &&
                                result.memberPath() == "/a",
                            "a top-level duplicate key is rejected with its exact path");
    }
    {
        const auto result = parseGenerous(R"({"a":1,"b":{"x":1,"x":2}})");
        expectations.expect(!result && result.error() == StrictJsonDomError::DuplicateObjectKey &&
                                result.memberPath() == "/b/x",
                            "a nested object duplicate key reports its full member path");
    }
    {
        const auto result = parseGenerous(R"({"list":[{"n":1,"n":2}]})");
        expectations.expect(!result && result.error() == StrictJsonDomError::DuplicateObjectKey &&
                                result.memberPath() == "/list/0/n",
                            "a duplicate key inside an array element includes the array index");
    }
    {
        // "a" decodes to the same Unicode scalar sequence as "a"; duplicate detection
        // compares decoded strings, not escaped spellings.
        const auto result = parseGenerous(R"({"a":1,"a":2})");
        expectations.expect(!result && result.error() == StrictJsonDomError::DuplicateObjectKey &&
                                result.memberPath() == "/a",
                            "an escaped-spelling collision is rejected the same as a literal one");
    }
    {
        const auto result = parseGenerous(R"({"a":{"b":{"c":1,"c":2}}})");
        expectations.expect(!result && result.error() == StrictJsonDomError::DuplicateObjectKey &&
                                result.memberPath() == "/a/b/c",
                            "a duplicate several levels deep still reports the exact path");
    }
    {
        const auto result = parseGenerous(R"({"a":1,"b":2,"c":3})");
        expectations.expect(static_cast<bool>(result), "distinct sibling keys are accepted");
    }
}

// ---------------------------------------------------------------------------------------------
// Raw number token preservation
// ---------------------------------------------------------------------------------------------

void testRawNumberTokenPreservation(Expectations& expectations) {
    constexpr std::string_view tokens[] = {
        "-9223372036854775808",
        "9223372036854775807",
        "-0",
        "0",
        "1e10",
        "1E+21",
        // The canonical Float64 golden subset mirrored from unknown_json_number_tests.cpp.
        "0.0",
        "-0.0",
        "5e-324",
        "-5e-324",
        "2.225073858507201e-308",
        "2.2250738585072014e-308",
        "1.7976931348623157e+308",
        "-1.7976931348623157e+308",
        "9007199254740992.0",
        "9.999999999999997e-7",
        "0.000001",
        "999999999999999900000.0",
        "1e+21",
    };

    std::string document = "[";
    for (std::size_t index = 0; index < std::size(tokens); ++index) {
        if (index != 0) {
            document += ',';
        }
        document += tokens[index];
    }
    document += ']';

    const auto result = parseGenerous(document);
    if (!result) {
        expectations.expect(false, "the raw-number fixture document parses");
        return;
    }
    const auto& root = result.document()->root();
    expectations.expect(root.kind() == JsonValueKind::Array, "the raw-number fixture is an array");
    const auto elements = root.arrayElements();
    expectations.expect(elements.size() == std::size(tokens),
                        "every fixture token becomes one array element");
    for (std::size_t index = 0; index < std::size(tokens) && index < elements.size(); ++index) {
        const auto token = elements[index].asNumberToken();
        expectations.expect(elements[index].kind() == JsonValueKind::Number && token.has_value() &&
                                *token == tokens[index],
                            "the source number token is preserved exactly, unconverted");
    }
}

// ---------------------------------------------------------------------------------------------
// Member order preservation
// ---------------------------------------------------------------------------------------------

void testMemberOrderPreservation(Expectations& expectations) {
    const auto result = parseGenerous(R"({"zebra":1,"apple":2,"mango":3,"banana":4})");
    if (!result) {
        expectations.expect(false, "the member-order fixture parses");
        return;
    }
    const auto members = result.document()->root().objectMembers();
    const std::string_view expectedOrder[] = {"zebra", "apple", "mango", "banana"};
    expectations.expect(members.size() == std::size(expectedOrder),
                        "the member-order fixture keeps every member");
    for (std::size_t index = 0; index < std::size(expectedOrder) && index < members.size();
         ++index) {
        expectations.expect(members[index].key() == expectedOrder[index],
                            "object members retain their exact source order, not sorted order");
    }
    const auto* mango = result.document()->root().findMember("mango");
    expectations.expect(mango != nullptr && mango->asNumberToken() == "3",
                        "findMember locates a member regardless of its source position");
    expectations.expect(result.document()->root().findMember("missing") == nullptr,
                        "findMember reports absence with a null pointer");
}

// ---------------------------------------------------------------------------------------------
// Depth/limit agreement with the preflight scan
// ---------------------------------------------------------------------------------------------

void testDepthLimitAgreesWithPreflight(Expectations& expectations) {
    const auto depth = bloom::project::kStrictJsonDomMaximumDepth;

    std::string exact(depth, '[');
    exact.append(depth, ']');
    const auto exactResult = parseGenerous(exact);
    expectations.expect(static_cast<bool>(exactResult),
                        "the absolute depth boundary, root included, is accepted");

    std::string over(depth + 1, '[');
    over.append(depth + 1, ']');
    const auto overResult = parseGenerous(over);
    expectations.expect(!overResult &&
                            overResult.error() == StrictJsonDomError::DepthLimitExceeded &&
                            overResult.byteOffset() == depth,
                        "one level beyond the boundary is rejected at the preflight's own offset");

    StrictJsonDomLimits lowered;
    lowered.maximumDepth = 4;
    const auto loweredExact = parseGenerous("[[[[]]]]", lowered);
    expectations.expect(static_cast<bool>(loweredExact),
                        "a caller-lowered depth budget is honored at its own exact boundary");
    const auto loweredOver = parseGenerous("[[[[[]]]]]", lowered);
    expectations.expect(!loweredOver &&
                            loweredOver.error() == StrictJsonDomError::DepthLimitExceeded,
                        "a caller-lowered depth budget rejects one level beyond its own boundary");

    StrictJsonDomLimits raised;
    raised.maximumDepth = bloom::project::kStrictJsonDomMaximumDepth + 1;
    const auto raisedResult = parseGenerous("null", raised);
    expectations.expect(!raisedResult && raisedResult.error() == StrictJsonDomError::InvalidLimits,
                        "a caller cannot raise the depth budget above the fixed v1 ceiling");
}

// ---------------------------------------------------------------------------------------------
// Writer/DOM round trip over the canonical minimal document golden fixture
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::array<std::uint8_t, 32> ascendingDigestBytes() noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));
}

[[nodiscard]] bloom::document::NewProject makeMinimalProject() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    return bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
}

[[nodiscard]] const JsonValue* expectChild(Expectations& expectations, const JsonValue& parent,
                                           const std::string_view key,
                                           const std::string_view context) {
    const auto* child = parent.findMember(key);
    expectations.expect(child != nullptr, context);
    return child;
}

void expectKeyOrder(Expectations& expectations, const JsonValue& value,
                    const std::initializer_list<std::string_view> keys,
                    const std::string_view context) {
    expectations.expect(value.kind() == JsonValueKind::Object, context);
    if (value.kind() != JsonValueKind::Object) {
        return;
    }
    const auto members = value.objectMembers();
    bool matches = members.size() == keys.size();
    std::size_t index = 0;
    for (const auto expectedKey : keys) {
        matches = matches && index < members.size() && members[index].key() == expectedKey;
        ++index;
    }
    expectations.expect(matches, context);
}

void expectString(Expectations& expectations, const JsonValue& value,
                  const std::string_view expected, const std::string_view context) {
    const auto actual = value.asString();
    expectations.expect(actual.has_value() && *actual == expected, context);
}

void expectNumber(Expectations& expectations, const JsonValue& value,
                  const std::string_view expected, const std::string_view context) {
    const auto actual = value.asNumberToken();
    expectations.expect(actual.has_value() && *actual == expected, context);
}

void expectEmptyArray(Expectations& expectations, const JsonValue& value,
                      const std::string_view context) {
    expectations.expect(value.kind() == JsonValueKind::Array && value.arrayElements().empty(),
                        context);
}

void testWriterDomRoundTrip(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    constexpr std::size_t kScratchSize = 64;
    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const bloom::project::CanonicalDocumentV1 request{.snapshot = &snapshot,
                                                      .colorSettings = &settings,
                                                      .payloadScratch = payloadScratch,
                                                      .sortScratch = sortScratch};

    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        expectations.expect(false, "the round-trip fixture preflights with the canonical writer");
        return;
    }
    std::vector<char> encoded(*size.value(), '\0');
    const auto written = bloom::project::encodeCanonicalDocument(request, encoded);
    if (!written || written.bytesWritten() != *size.value()) {
        expectations.expect(false, "the round-trip fixture encodes with the canonical writer");
        return;
    }

    const std::string_view encodedText(encoded.data(), encoded.size());
    const auto parsed = parseGenerous(encodedText);
    if (!parsed) {
        expectations.expect(false, "the canonical writer's own minimal document is accepted");
        return;
    }

    const auto& root = parsed.document()->root();
    expectKeyOrder(expectations, root, {"schemaVersion", "project", "idAllocation", "extensions"},
                   "the root object keeps the canonical writer's exact member order");

    if (const auto* schemaVersion = expectChild(expectations, root, "schemaVersion",
                                                "the root has a schemaVersion member")) {
        expectKeyOrder(expectations, *schemaVersion, {"major", "minor"},
                       "schemaVersion keeps its exact member order");
        if (const auto* major =
                expectChild(expectations, *schemaVersion, "major", "major exists")) {
            expectNumber(expectations, *major, "1", "the document schema major is preserved");
        }
        if (const auto* minor =
                expectChild(expectations, *schemaVersion, "minor", "minor exists")) {
            expectNumber(expectations, *minor, "0", "the document schema minor is preserved");
        }
    }

    const auto* project =
        expectChild(expectations, root, "project", "the root has a project member");
    if (project != nullptr) {
        expectKeyOrder(expectations, *project, {"id", "name", "colorSettings", "compositions"},
                       "project keeps its exact member order");
        if (const auto* id = expectChild(expectations, *project, "id", "project.id exists")) {
            expectString(expectations, *id, "1", "the project id is preserved as a decimal string");
        }
        if (const auto* name = expectChild(expectations, *project, "name", "project.name exists")) {
            expectString(expectations, *name, "Untitled Project", "the project name is preserved");
        }

        const auto* colorSettings =
            expectChild(expectations, *project, "colorSettings", "project.colorSettings exists");
        if (colorSettings != nullptr) {
            expectKeyOrder(expectations, *colorSettings,
                           {"schemaVersion", "processColorSpaceId", "ocioConfig"},
                           "colorSettings keeps its exact member order");
            if (const auto* processColorSpaceId =
                    expectChild(expectations, *colorSettings, "processColorSpaceId",
                                "processColorSpaceId exists")) {
                expectString(expectations, *processColorSpaceId, "lin_rec709_scene",
                             "the fixed v1 process color space id is preserved");
            }
            const auto* ocioConfig =
                expectChild(expectations, *colorSettings, "ocioConfig", "ocioConfig exists");
            if (ocioConfig != nullptr) {
                expectKeyOrder(expectations, *ocioConfig,
                               {"schemaVersion", "locator", "expectedRevision", "portability",
                                "contextVariables"},
                               "ocioConfig keeps its exact member order");
                const auto* locator =
                    expectChild(expectations, *ocioConfig, "locator", "locator exists");
                if (locator != nullptr) {
                    expectKeyOrder(expectations, *locator, {"kind", "uri"},
                                   "the builtin locator keeps its exact member order");
                    if (const auto* kind =
                            expectChild(expectations, *locator, "kind", "locator.kind exists")) {
                        expectString(expectations, *kind, "builtin",
                                     "the locator kind is preserved");
                    }
                    if (const auto* uri =
                            expectChild(expectations, *locator, "uri", "locator.uri exists")) {
                        expectString(expectations, *uri, "bloom://ocio/neutral-v1/config.ocio",
                                     "the builtin locator URI is preserved");
                    }
                }
                const auto* expectedRevision = expectChild(
                    expectations, *ocioConfig, "expectedRevision", "expectedRevision exists");
                if (expectedRevision != nullptr) {
                    expectKeyOrder(expectations, *expectedRevision, {"algorithm", "digest"},
                                   "expectedRevision keeps its exact member order");
                    if (const auto* digest = expectChild(expectations, *expectedRevision, "digest",
                                                         "expectedRevision.digest exists")) {
                        expectString(
                            expectations, *digest,
                            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                            "the ascending digest fixture bytes are preserved as lowercase hex");
                    }
                }
                if (const auto* portability = expectChild(expectations, *ocioConfig, "portability",
                                                          "portability exists")) {
                    expectString(expectations, *portability, "builtin", "portability is preserved");
                }
                if (const auto* contextVariables = expectChild(
                        expectations, *ocioConfig, "contextVariables", "contextVariables exists")) {
                    expectEmptyArray(expectations, *contextVariables,
                                     "the minimal fixture has no context variables");
                }
            }
        }

        const auto* compositions =
            expectChild(expectations, *project, "compositions", "project.compositions exists");
        if (compositions != nullptr) {
            expectations.expect(compositions->kind() == JsonValueKind::Array &&
                                    compositions->arrayElements().size() == 1,
                                "the minimal fixture has exactly one composition");
            if (compositions->kind() == JsonValueKind::Array &&
                !compositions->arrayElements().empty()) {
                const auto& composition = compositions->arrayElements().front();
                expectKeyOrder(
                    expectations, composition,
                    {"id", "name", "duration", "format", "parameters", "animationCurves", "graph"},
                    "the composition keeps its exact member order");
                if (const auto* name =
                        expectChild(expectations, composition, "name", "composition.name exists")) {
                    expectString(expectations, *name, "Main Composition",
                                 "the composition name is preserved");
                }
                const auto* durationValue = expectChild(expectations, composition, "duration",
                                                        "composition.duration exists");
                if (durationValue != nullptr) {
                    expectKeyOrder(expectations, *durationValue, {"numerator", "denominator"},
                                   "duration keeps its exact member order");
                    if (const auto* numerator =
                            expectChild(expectations, *durationValue, "numerator",
                                        "duration.numerator exists")) {
                        expectString(expectations, *numerator, "10",
                                     "the reduced duration numerator is preserved");
                    }
                }
                const auto* format =
                    expectChild(expectations, composition, "format", "composition.format exists");
                if (format != nullptr) {
                    expectKeyOrder(expectations, *format,
                                   {"width", "height", "pixelAspect", "frameRate"},
                                   "format keeps its exact member order");
                    if (const auto* width =
                            expectChild(expectations, *format, "width", "format.width exists")) {
                        expectNumber(expectations, *width, "1920",
                                     "the default composition width is preserved");
                    }
                    if (const auto* height =
                            expectChild(expectations, *format, "height", "format.height exists")) {
                        expectNumber(expectations, *height, "1080",
                                     "the default composition height is preserved");
                    }
                }
                if (const auto* parameters = expectChild(expectations, composition, "parameters",
                                                         "composition.parameters exists")) {
                    expectEmptyArray(expectations, *parameters,
                                     "the minimal fixture has no parameters");
                }
                if (const auto* animationCurves =
                        expectChild(expectations, composition, "animationCurves",
                                    "composition.animationCurves exists")) {
                    expectEmptyArray(expectations, *animationCurves,
                                     "the minimal fixture has no animation curves");
                }

                const auto* graph =
                    expectChild(expectations, composition, "graph", "composition.graph exists");
                if (graph != nullptr) {
                    expectKeyOrder(
                        expectations, *graph,
                        {"nodes", "edges", "layerOutputs", "layerStack", "compositionOutput"},
                        "graph keeps its exact member order");
                    const auto* nodes =
                        expectChild(expectations, *graph, "nodes", "graph.nodes exists");
                    if (nodes != nullptr) {
                        expectations.expect(nodes->kind() == JsonValueKind::Array &&
                                                nodes->arrayElements().size() == 2,
                                            "the minimal graph has exactly two nodes");
                        if (nodes->kind() == JsonValueKind::Array &&
                            nodes->arrayElements().size() == 2) {
                            const auto& firstNode = nodes->arrayElements()[0];
                            expectKeyOrder(expectations, firstNode,
                                           {"id", "typeId", "schemaVersion", "parameters"},
                                           "a node keeps its exact member order");
                            if (const auto* typeId = expectChild(expectations, firstNode, "typeId",
                                                                 "node.typeId exists")) {
                                expectString(expectations, *typeId, "bloom.layer-stack",
                                             "the layer-stack node type id is preserved");
                            }
                            const auto& secondNode = nodes->arrayElements()[1];
                            if (const auto* typeId = expectChild(expectations, secondNode, "typeId",
                                                                 "second node.typeId exists")) {
                                expectString(expectations, *typeId, "bloom.composition-output",
                                             "the composition-output node type id is preserved");
                            }
                        }
                    }
                    const auto* edges =
                        expectChild(expectations, *graph, "edges", "graph.edges exists");
                    if (edges != nullptr && edges->kind() == JsonValueKind::Array &&
                        edges->arrayElements().size() == 1) {
                        const auto& edge = edges->arrayElements().front();
                        expectKeyOrder(expectations, edge, {"id", "source", "destination"},
                                       "an edge keeps its exact member order");
                        const auto* destination = expectChild(expectations, edge, "destination",
                                                              "edge.destination exists");
                        if (destination != nullptr) {
                            expectKeyOrder(expectations, *destination, {"kind", "nodeId", "port"},
                                           "a node-input destination keeps its exact member order");
                            if (const auto* kind = expectChild(expectations, *destination, "kind",
                                                               "destination.kind exists")) {
                                expectString(expectations, *kind, "node-input",
                                             "the edge destination kind is preserved");
                            }
                        }
                    } else {
                        expectations.expect(false, "the minimal graph has exactly one edge");
                    }
                    if (const auto* layerOutputs = expectChild(expectations, *graph, "layerOutputs",
                                                               "graph.layerOutputs exists")) {
                        expectEmptyArray(expectations, *layerOutputs,
                                         "the minimal fixture has no layer output boundaries");
                    }
                    const auto* layerStack =
                        expectChild(expectations, *graph, "layerStack", "graph.layerStack exists");
                    if (layerStack != nullptr) {
                        expectKeyOrder(expectations, *layerStack, {"nodeId", "entries"},
                                       "layerStack keeps its exact member order");
                        if (const auto* entries = expectChild(expectations, *layerStack, "entries",
                                                              "layerStack.entries exists")) {
                            expectEmptyArray(expectations, *entries,
                                             "the minimal fixture has no layer stack entries");
                        }
                    }
                    const auto* compositionOutput =
                        expectChild(expectations, *graph, "compositionOutput",
                                    "graph.compositionOutput exists");
                    if (compositionOutput != nullptr) {
                        expectKeyOrder(expectations, *compositionOutput, {"nodeId", "port"},
                                       "compositionOutput keeps its exact member order");
                    }
                }
            }
        }
    }

    const auto* idAllocation =
        expectChild(expectations, root, "idAllocation", "the root has an idAllocation member");
    if (idAllocation != nullptr) {
        expectKeyOrder(expectations, *idAllocation, {"highestIssued"},
                       "idAllocation keeps its exact member order");
        const auto* highestIssued = expectChild(expectations, *idAllocation, "highestIssued",
                                                "idAllocation.highestIssued exists");
        if (highestIssued != nullptr) {
            expectKeyOrder(expectations, *highestIssued,
                           {"composition", "node", "edge", "layer", "layerSlot", "parameter",
                            "animationCurve", "keyframe", "driverBinding", "extensionRecord"},
                           "highestIssued keeps its exact member order across every namespace");
            if (const auto* composition = expectChild(expectations, *highestIssued, "composition",
                                                      "highestIssued.composition exists")) {
                expectString(expectations, *composition, "1",
                             "the composition high-water mark is preserved");
            }
            if (const auto* node = expectChild(expectations, *highestIssued, "node",
                                               "highestIssued.node exists")) {
                expectString(expectations, *node, "2", "the node high-water mark is preserved");
            }
            if (const auto* driverBinding =
                    expectChild(expectations, *highestIssued, "driverBinding",
                                "highestIssued.driverBinding exists")) {
                expectString(expectations, *driverBinding, "0",
                             "the untouched driverBinding high-water mark is preserved");
            }
        }
    }

    if (const auto* extensions =
            expectChild(expectations, root, "extensions", "the root has an extensions member")) {
        expectEmptyArray(expectations, *extensions, "the minimal fixture has no extension records");
    }
}

// ---------------------------------------------------------------------------------------------
// PMR budget exhaustion
// ---------------------------------------------------------------------------------------------

void testPmrBudgetExhaustion(Expectations& expectations) {
    const auto result = bloom::project::parseStrictJsonDom(asBytes("null"), {}, makeOperation(4));
    expectations.expect(!result && result.error() == StrictJsonDomError::ResourceExhausted,
                        "an operation budget too small for even one allocation is reported");
    expectations.expect(result.document() == nullptr,
                        "a budget-exhaustion failure leaves the destination document untouched");
    expectations.expect(!static_cast<bool>(result),
                        "the result's boolean conversion agrees with the typed failure");
}

// Exhausts the budget *during the DOM walk itself*, not during yyjson's own read. The fixture is
// a flat, grammatically trivial JSON array (so yyjson's own working set is small and roughly
// proportional to the raw input bytes) holding many long strings (so the Bloom DOM's independent
// PMR-owned copy of every decoded string -- built through JsonMember's converting constructor and
// JsonValue::text_.assign/elements_.reserve/emplace_back, all ordinary throwing
// std::pmr::memory_resource operations, never the noexcept yyjson_alc adapter -- is large). A
// budget that comfortably funds yyjson_read_opts() but not that second copy must fail partway
// through buildValue(), strictly after the read already succeeded. Before the fix for the
// supervisor-reported defect (buildValue/JsonMember's converting constructor were incorrectly
// noexcept), any such budget crashed the process via std::terminate() instead of returning
// ResourceExhausted; this test's whole sweep completing at all, with no ParseFailed anywhere,
// is the evidence that the fix holds.
void testPmrBudgetExhaustionDuringDomWalk(Expectations& expectations) {
    constexpr std::size_t kElementCount = 400;
    constexpr std::size_t kStringLength = 2000;
    std::string content = "[";
    for (std::size_t index = 0; index < kElementCount; ++index) {
        if (index != 0) {
            content += ',';
        }
        content += '"';
        content.append(kStringLength, static_cast<char>('a' + static_cast<char>(index % 26)));
        content += '"';
    }
    content += ']';
    const auto rawBytes = static_cast<std::uint64_t>(content.size());

    // A generous known-good budget: comfortably above both yyjson's own read footprint and a
    // second full PMR-owned copy of every string.
    const auto generousResult =
        bloom::project::parseStrictJsonDom(asBytes(content), {}, makeOperation(rawBytes * 8));
    expectations.expect(static_cast<bool>(generousResult),
                        "a generous budget covers both the read and the full DOM walk");

    bool sawSuccess = false;
    bool sawResourceExhausted = false;
    bool sawResourceExhaustedAboveRawInputBytes = false;
    bool sawParseFailed = false;
    for (std::uint64_t numerator = 1; numerator <= 64; ++numerator) {
        const auto budget = rawBytes * numerator / 8;
        if (budget == 0) {
            continue;
        }
        const auto result =
            bloom::project::parseStrictJsonDom(asBytes(content), {}, makeOperation(budget));
        if (result) {
            sawSuccess = true;
            expectations.expect(result.document() != nullptr,
                                "a successful sweep result names a document");
            continue;
        }
        if (result.error() == StrictJsonDomError::ResourceExhausted) {
            sawResourceExhausted = true;
            // yyjson cannot decode this fixture's string content in less than `rawBytes` of
            // working memory (it holds no escapes, so decoded length equals source length); a
            // rejection at a budget already above the raw input size is therefore evidence the
            // read itself had room to succeed and the DOM's own second copy is what exhausted the
            // budget.
            if (budget > rawBytes) {
                sawResourceExhaustedAboveRawInputBytes = true;
            }
            expectations.expect(result.document() == nullptr,
                                "a budget-exhaustion failure leaves the destination untouched at "
                                "every point in the descending sweep");
        } else if (result.error() == StrictJsonDomError::ParseFailed) {
            sawParseFailed = true;
        }
    }

    expectations.expect(sawSuccess,
                        "the descending-budget sweep reaches a comfortably successful budget");
    expectations.expect(sawResourceExhausted,
                        "the descending-budget sweep reaches a budget too small to finish the DOM "
                        "walk, reported as a typed error rather than crashing the process");
    expectations.expect(sawResourceExhaustedAboveRawInputBytes,
                        "at least one rejection happens at a budget already large enough for "
                        "yyjson's own read, so that rejection occurs inside the DOM walk itself");
    expectations.expect(!sawParseFailed,
                        "no budget-driven rejection is ever misreported as ParseFailed, "
                        "confirming yyjson's own read never observed a grammar failure here");
}

// ---------------------------------------------------------------------------------------------
// Hostile inputs rejected by the preflight stage
// ---------------------------------------------------------------------------------------------

void testHostileInputsRejectedByPreflight(Expectations& expectations) {
    {
        std::string bom;
        bom.push_back(static_cast<char>(0xEFU));
        bom.push_back(static_cast<char>(0xBBU));
        bom.push_back(static_cast<char>(0xBFU));
        bom += "null";
        const auto result = parseGenerous(bom);
        expectations.expect(!result && result.error() == StrictJsonDomError::BomForbidden &&
                                result.byteOffset() == 0,
                            "a UTF-8 BOM is rejected by the preflight stage, reporting its error");
    }
    {
        // A string value containing a lone UTF-8 lead byte with no continuation byte.
        std::string truncated = R"({"a":")";
        truncated.push_back(static_cast<char>(0xC2U));
        truncated += "\"}";
        const auto result = parseGenerous(truncated);
        expectations.expect(!result && result.error() == StrictJsonDomError::InvalidUtf8,
                            "truncated UTF-8 is rejected by the preflight stage, reporting its "
                            "error");
    }
}

// ---------------------------------------------------------------------------------------------
// Value-kind accessor coverage
// ---------------------------------------------------------------------------------------------

void testValueKindsAndAccessors(Expectations& expectations) {
    const auto result = parseGenerous(
        R"({"n":null,"b":true,"f":false,"num":42,"str":"hi","arr":[1,2],"obj":{"x":1}})");
    if (!result) {
        expectations.expect(false, "the value-kind fixture parses");
        return;
    }
    const auto& root = result.document()->root();

    const auto* nullValue = root.findMember("n");
    expectations.expect(nullValue != nullptr && nullValue->kind() == JsonValueKind::Null &&
                            nullValue->isNull(),
                        "a JSON null decodes to Null");

    const auto* trueValue = root.findMember("b");
    expectations.expect(trueValue != nullptr && trueValue->kind() == JsonValueKind::Boolean &&
                            trueValue->asBoolean() == std::optional<bool>(true),
                        "a JSON true decodes to Boolean(true)");
    const auto* falseValue = root.findMember("f");
    expectations.expect(falseValue != nullptr && falseValue->kind() == JsonValueKind::Boolean &&
                            falseValue->asBoolean() == std::optional<bool>(false),
                        "a JSON false decodes to Boolean(false)");

    const auto* numberValue = root.findMember("num");
    expectations.expect(numberValue != nullptr && numberValue->kind() == JsonValueKind::Number &&
                            numberValue->asNumberToken() == std::optional<std::string_view>("42"),
                        "a JSON number decodes to Number with its exact token");
    expectations.expect(!numberValue->asString().has_value() &&
                            !numberValue->asBoolean().has_value(),
                        "wrong-kind accessors report nullopt rather than a default value");

    const auto* stringValue = root.findMember("str");
    expectations.expect(stringValue != nullptr && stringValue->kind() == JsonValueKind::String &&
                            stringValue->asString() == std::optional<std::string_view>("hi"),
                        "a JSON string decodes to String with its decoded content");

    const auto* arrayValue = root.findMember("arr");
    expectations.expect(arrayValue != nullptr && arrayValue->kind() == JsonValueKind::Array &&
                            arrayValue->arrayElements().size() == 2 &&
                            arrayValue->arrayElements()[0].asNumberToken() == "1" &&
                            arrayValue->arrayElements()[1].asNumberToken() == "2",
                        "a JSON array decodes to Array with its elements in order");
    expectations.expect(arrayValue->objectMembers().empty() &&
                            arrayValue->findMember("x") == nullptr,
                        "object-only accessors are empty/null on a non-object value");

    const auto* objectValue = root.findMember("obj");
    expectations.expect(objectValue != nullptr && objectValue->kind() == JsonValueKind::Object &&
                            objectValue->arrayElements().empty(),
                        "a JSON object decodes to Object; array-only accessors are empty");
    const auto* nestedX = objectValue != nullptr ? objectValue->findMember("x") : nullptr;
    expectations.expect(nestedX != nullptr && nestedX->asNumberToken() == "1",
                        "findMember resolves a nested object member");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testDuplicateKeyRejection(expectations);
        testRawNumberTokenPreservation(expectations);
        testMemberOrderPreservation(expectations);
        testDepthLimitAgreesWithPreflight(expectations);
        testWriterDomRoundTrip(expectations);
        testPmrBudgetExhaustion(expectations);
        testPmrBudgetExhaustionDuringDomWalk(expectations);
        testHostileInputsRejectedByPreflight(expectations);
        testValueKindsAndAccessors(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
