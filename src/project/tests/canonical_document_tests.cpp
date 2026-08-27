#include <bloom/project/canonical_document.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/unknown_json_number.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numeric>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::core::RationalTime;
using bloom::document::AnimationCurveSource;
using bloom::document::Composition;
using bloom::document::CompositionFormat;
using bloom::document::ConstantValueSource;
using bloom::document::Document;
using bloom::document::DriverBindingSource;
using bloom::document::FrameRate;
using bloom::document::OpaqueExtensionPayload;
using bloom::document::Project;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::project::CanonicalDocumentError;
using bloom::project::CanonicalDocumentLimits;
using bloom::project::CanonicalDocumentV1;
using bloom::project::DocumentDecodeResult;
using bloom::project::JsonValue;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::RetainedJsonMember;
using bloom::project::RetainedJsonValue;
using bloom::project::RoundTripAttachmentPath;
using bloom::project::RoundTripCollectionKind;
using bloom::project::RoundTripPathSegment;
using bloom::project::RoundTripState;
using bloom::project::UnknownJsonNumber;

constexpr std::size_t kScratchSize = 64;

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
    const auto duration = RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    return bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
}

struct Encoded final {
    bool ok = false;
    std::string bytes;
};

// Sizes, encodes at exact capacity plus slack, and reports both results so callers can assert
// golden bytes and untouched slack in one place.
[[nodiscard]] Encoded
encodeWithSlack(const CanonicalDocumentV1& request,
                const CanonicalDocumentLimits limits = CanonicalDocumentLimits{}) {
    Encoded encoded;
    const auto size = bloom::project::canonicalDocumentSize(request, limits);
    if (!size.hasValue()) {
        return encoded;
    }
    const auto required = *size.value();
    std::vector<char> output(required + 8, '?');
    const auto result = bloom::project::encodeCanonicalDocument(request, output, limits);
    if (!result || result.bytesWritten() != required) {
        return encoded;
    }
    for (std::size_t index = required; index < output.size(); ++index) {
        if (output[index] != '?') {
            return encoded;
        }
    }
    encoded.ok = true;
    encoded.bytes.assign(output.data(), required);
    return encoded;
}

void expectRequestError(Expectations& expectations, const CanonicalDocumentV1& request,
                        const CanonicalDocumentLimits limits,
                        const CanonicalDocumentError expectedError,
                        const std::size_t expectedCompositionIndex,
                        const std::size_t expectedElementIndex, const std::string_view message) {
    const auto size = bloom::project::canonicalDocumentSize(request, limits);
    expectations.expect(!size.hasValue() && size.error() == expectedError &&
                            size.compositionIndex() == expectedCompositionIndex &&
                            size.elementIndex() == expectedElementIndex,
                        message);

    std::array<char, 8> tinyOutput{};
    tinyOutput.fill('?');
    const auto encode = bloom::project::encodeCanonicalDocument(request, tinyOutput, limits);
    expectations.expect(!encode && !encode.requiredSize().has_value() &&
                            encode.error() == expectedError && encode.bytesWritten() == 0 &&
                            encode.compositionIndex() == expectedCompositionIndex &&
                            encode.elementIndex() == expectedElementIndex,
                        "the matching encode path reports the same typed failure");
}

// The exact canonical bytes for makeMinimalProject()'s live document at document schema {1, 0}
// with no overlay. Factored out (rather than kept local to testMinimalGoldenBytes, as originally)
// so RT2's overlay tests below can build on it: they take this known-good text and apply small,
// explicit, verified-anchor edits (splicing retained members at exact known canonical positions)
// rather than hand-transcribing a second, separately-error-prone multi-hundred-line literal. See
// this file's RT2 section for why splicing was chosen over the alternative of building fixtures
// straight from the writer plus a golden diff.
constexpr std::string_view kMinimalDocumentGolden =
    "{\n"
    "  \"schemaVersion\": {\n"
    "    \"major\": 1,\n"
    "    \"minor\": 0\n"
    "  },\n"
    "  \"project\": {\n"
    "    \"id\": \"1\",\n"
    "    \"name\": \"Untitled Project\",\n"
    "    \"colorSettings\": {\n"
    "      \"schemaVersion\": {\n"
    "        \"major\": 1,\n"
    "        \"minor\": 0\n"
    "      },\n"
    "      \"processColorSpaceId\": \"lin_rec709_scene\",\n"
    "      \"ocioConfig\": {\n"
    "        \"schemaVersion\": {\n"
    "          \"major\": 1,\n"
    "          \"minor\": 0\n"
    "        },\n"
    "        \"locator\": {\n"
    "          \"kind\": \"builtin\",\n"
    "          \"uri\": \"bloom://ocio/neutral-v1/config.ocio\"\n"
    "        },\n"
    "        \"expectedRevision\": {\n"
    "          \"algorithm\": \"sha256\",\n"
    "          \"digest\": "
    "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"\n"
    "        },\n"
    "        \"portability\": \"builtin\",\n"
    "        \"contextVariables\": []\n"
    "      }\n"
    "    },\n"
    "    \"compositions\": [\n"
    "      {\n"
    "        \"id\": \"1\",\n"
    "        \"name\": \"Main Composition\",\n"
    "        \"duration\": {\n"
    "          \"numerator\": \"10\",\n"
    "          \"denominator\": \"1\"\n"
    "        },\n"
    "        \"format\": {\n"
    "          \"width\": 1920,\n"
    "          \"height\": 1080,\n"
    "          \"pixelAspect\": {\n"
    "            \"numerator\": \"1\",\n"
    "            \"denominator\": \"1\"\n"
    "          },\n"
    "          \"frameRate\": {\n"
    "            \"numerator\": \"24\",\n"
    "            \"denominator\": \"1\"\n"
    "          }\n"
    "        },\n"
    "        \"parameters\": [],\n"
    "        \"animationCurves\": [],\n"
    "        \"graph\": {\n"
    "          \"nodes\": [\n"
    "            {\n"
    "              \"id\": \"1\",\n"
    "              \"typeId\": \"bloom.layer-stack\",\n"
    "              \"schemaVersion\": 1,\n"
    "              \"parameters\": []\n"
    "            },\n"
    "            {\n"
    "              \"id\": \"2\",\n"
    "              \"typeId\": \"bloom.composition-output\",\n"
    "              \"schemaVersion\": 1,\n"
    "              \"parameters\": []\n"
    "            }\n"
    "          ],\n"
    "          \"edges\": [\n"
    "            {\n"
    "              \"id\": \"1\",\n"
    "              \"source\": {\n"
    "                \"nodeId\": \"1\",\n"
    "                \"port\": \"image\"\n"
    "              },\n"
    "              \"destination\": {\n"
    "                \"kind\": \"node-input\",\n"
    "                \"nodeId\": \"2\",\n"
    "                \"port\": \"image\"\n"
    "              }\n"
    "            }\n"
    "          ],\n"
    "          \"layerOutputs\": [],\n"
    "          \"layerStack\": {\n"
    "            \"nodeId\": \"1\",\n"
    "            \"entries\": []\n"
    "          },\n"
    "          \"compositionOutput\": {\n"
    "            \"nodeId\": \"2\",\n"
    "            \"port\": \"image\"\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    ]\n"
    "  },\n"
    "  \"idAllocation\": {\n"
    "    \"highestIssued\": {\n"
    "      \"composition\": \"1\",\n"
    "      \"node\": \"2\",\n"
    "      \"edge\": \"1\",\n"
    "      \"layer\": \"0\",\n"
    "      \"layerSlot\": \"0\",\n"
    "      \"parameter\": \"0\",\n"
    "      \"animationCurve\": \"0\",\n"
    "      \"keyframe\": \"0\",\n"
    "      \"driverBinding\": \"0\",\n"
    "      \"extensionRecord\": \"0\"\n"
    "    }\n"
    "  },\n"
    "  \"extensions\": []\n"
    "}\n";

void testMinimalGoldenBytes(Expectations& expectations) {
    const std::string_view expected = kMinimalDocumentGolden;

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const CanonicalDocumentV1 missingSettings{.snapshot = &snapshot,
                                              .colorSettings = nullptr,
                                              .payloadScratch = payloadScratch,
                                              .sortScratch = sortScratch};
    expectRequestError(expectations, missingSettings, {}, CanonicalDocumentError::MissingInput,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "a request without explicit color settings is rejected before sizing");

    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 valid{.snapshot = &snapshot,
                                    .colorSettings = &settings,
                                    .payloadScratch = payloadScratch,
                                    .sortScratch = sortScratch};
    const auto size = bloom::project::canonicalDocumentSize(valid);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "minimal new-project preflight returns its exact golden byte count");

    const auto encoded = encodeWithSlack(valid);
    expectations.expect(encoded.ok && encoded.bytes == expected,
                        "the minimal document emits exact canonical member order and layout");
}

// __PART2__

void testComposedGoldenBytes(Expectations& expectations) {
    constexpr std::string_view expected =
        "{\n"
        "  \"schemaVersion\": {\n"
        "    \"major\": 1,\n"
        "    \"minor\": 0\n"
        "  },\n"
        "  \"project\": {\n"
        "    \"id\": \"1\",\n"
        "    \"name\": \"Spot Check\",\n"
        "    \"colorSettings\": {\n"
        "      \"schemaVersion\": {\n"
        "        \"major\": 1,\n"
        "        \"minor\": 0\n"
        "      },\n"
        "      \"processColorSpaceId\": \"lin_rec709_scene\",\n"
        "      \"ocioConfig\": {\n"
        "        \"schemaVersion\": {\n"
        "          \"major\": 1,\n"
        "          \"minor\": 0\n"
        "        },\n"
        "        \"locator\": {\n"
        "          \"kind\": \"builtin\",\n"
        "          \"uri\": \"bloom://ocio/neutral-v1/config.ocio\"\n"
        "        },\n"
        "        \"expectedRevision\": {\n"
        "          \"algorithm\": \"sha256\",\n"
        "          \"digest\": "
        "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"\n"
        "        },\n"
        "        \"portability\": \"builtin\",\n"
        "        \"contextVariables\": []\n"
        "      }\n"
        "    },\n"
        "    \"compositions\": [\n"
        "      {\n"
        "        \"id\": \"1\",\n"
        "        \"name\": \"Hero Shot\",\n"
        "        \"duration\": {\n"
        "          \"numerator\": \"2\",\n"
        "          \"denominator\": \"1\"\n"
        "        },\n"
        "        \"format\": {\n"
        "          \"width\": 1280,\n"
        "          \"height\": 720,\n"
        "          \"pixelAspect\": {\n"
        "            \"numerator\": \"2\",\n"
        "            \"denominator\": \"1\"\n"
        "          },\n"
        "          \"frameRate\": {\n"
        "            \"numerator\": \"24000\",\n"
        "            \"denominator\": \"1001\"\n"
        "          }\n"
        "        },\n"
        "        \"parameters\": [\n"
        "          {\n"
        "            \"id\": \"3\",\n"
        "            \"schemaKey\": \"bloom.layer.opacity\",\n"
        "            \"source\": {\n"
        "              \"kind\": \"animation-curve\",\n"
        "              \"curveId\": \"9\"\n"
        "            }\n"
        "          },\n"
        "          {\n"
        "            \"id\": \"5\",\n"
        "            \"schemaKey\": \"bloom.transform.position\",\n"
        "            \"source\": {\n"
        "              \"kind\": \"constant\",\n"
        "              \"value\": {\n"
        "                \"kind\": \"vec2\",\n"
        "                \"x\": 96.0,\n"
        "                \"y\": -48.0\n"
        "              }\n"
        "            }\n"
        "          },\n"
        "          {\n"
        "            \"id\": \"7\",\n"
        "            \"schemaKey\": \"bloom.solid.color\",\n"
        "            \"source\": {\n"
        "              \"kind\": \"constant\",\n"
        "              \"value\": {\n"
        "                \"kind\": \"color4\",\n"
        "                \"red\": 0.0,\n"
        "                \"green\": 0.5,\n"
        "                \"blue\": 1.0,\n"
        "                \"alpha\": 1.0\n"
        "              }\n"
        "            }\n"
        "          }\n"
        "        ],\n"
        "        \"animationCurves\": [\n"
        "          {\n"
        "            \"id\": \"9\",\n"
        "            \"kind\": \"scalar\",\n"
        "            \"keyframes\": [\n"
        "              {\n"
        "                \"id\": \"21\",\n"
        "                \"time\": {\n"
        "                  \"numerator\": \"0\",\n"
        "                  \"denominator\": \"1\"\n"
        "                },\n"
        "                \"value\": 0.25,\n"
        "                \"outgoingInterpolation\": \"hold\"\n"
        "              },\n"
        "              {\n"
        "                \"id\": \"22\",\n"
        "                \"time\": {\n"
        "                  \"numerator\": \"48\",\n"
        "                  \"denominator\": \"1\"\n"
        "                },\n"
        "                \"value\": 1.0,\n"
        "                \"outgoingInterpolation\": \"linear\"\n"
        "              }\n"
        "            ]\n"
        "          }\n"
        "        ],\n"
        "        \"graph\": {\n"
        "          \"nodes\": [\n"
        "            {\n"
        "              \"id\": \"1\",\n"
        "              \"typeId\": \"bloom.layer-stack\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": []\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"2\",\n"
        "              \"typeId\": \"bloom.solid-source\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": [\n"
        "                {\n"
        "                  \"role\": \"color\",\n"
        "                  \"parameterId\": \"7\"\n"
        "                }\n"
        "              ]\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"3\",\n"
        "              \"typeId\": \"bloom.layer-output\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": [\n"
        "                {\n"
        "                  \"role\": \"opacity\",\n"
        "                  \"parameterId\": \"3\"\n"
        "                },\n"
        "                {\n"
        "                  \"role\": \"position\",\n"
        "                  \"parameterId\": \"5\"\n"
        "                }\n"
        "              ]\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"4\",\n"
        "              \"typeId\": \"bloom.composition-output\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": []\n"
        "            }\n"
        "          ],\n"
        "          \"edges\": [\n"
        "            {\n"
        "              \"id\": \"1\",\n"
        "              \"source\": {\n"
        "                \"nodeId\": \"2\",\n"
        "                \"port\": \"image\"\n"
        "              },\n"
        "              \"destination\": {\n"
        "                \"kind\": \"node-input\",\n"
        "                \"nodeId\": \"3\",\n"
        "                \"port\": \"image\"\n"
        "              }\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"2\",\n"
        "              \"source\": {\n"
        "                \"nodeId\": \"3\",\n"
        "                \"port\": \"image\"\n"
        "              },\n"
        "              \"destination\": {\n"
        "                \"kind\": \"layer-stack-input\",\n"
        "                \"stackNodeId\": \"1\",\n"
        "                \"slotId\": \"1\",\n"
        "                \"role\": \"content\"\n"
        "              }\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"3\",\n"
        "              \"source\": {\n"
        "                \"nodeId\": \"1\",\n"
        "                \"port\": \"image\"\n"
        "              },\n"
        "              \"destination\": {\n"
        "                \"kind\": \"node-input\",\n"
        "                \"nodeId\": \"4\",\n"
        "                \"port\": \"image\"\n"
        "              }\n"
        "            }\n"
        "          ],\n"
        "          \"layerOutputs\": [\n"
        "            {\n"
        "              \"nodeId\": \"3\",\n"
        "              \"layerId\": \"1\",\n"
        "              \"name\": \"Hero Plate\",\n"
        "              \"outputPort\": \"image\"\n"
        "            }\n"
        "          ],\n"
        "          \"layerStack\": {\n"
        "            \"nodeId\": \"1\",\n"
        "            \"entries\": [\n"
        "              {\n"
        "                \"slotId\": \"1\",\n"
        "                \"layerId\": \"1\"\n"
        "              }\n"
        "            ]\n"
        "          },\n"
        "          \"compositionOutput\": {\n"
        "            \"nodeId\": \"4\",\n"
        "            \"port\": \"image\"\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    ]\n"
        "  },\n"
        "  \"idAllocation\": {\n"
        "    \"highestIssued\": {\n"
        "      \"composition\": \"1\",\n"
        "      \"node\": \"4\",\n"
        "      \"edge\": \"3\",\n"
        "      \"layer\": \"1\",\n"
        "      \"layerSlot\": \"1\",\n"
        "      \"parameter\": \"7\",\n"
        "      \"animationCurve\": \"9\",\n"
        "      \"keyframe\": \"22\",\n"
        "      \"driverBinding\": \"0\",\n"
        "      \"extensionRecord\": \"0\"\n"
        "    }\n"
        "  },\n"
        "  \"extensions\": []\n"
        "}\n";

    using namespace bloom::document;
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

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 requestTooSmallSort{.snapshot = &snapshot,
                                                  .colorSettings = &settings,
                                                  .payloadScratch = payloadScratch,
                                                  .sortScratch = {}};
    expectRequestError(
        expectations, requestTooSmallSort, {}, CanonicalDocumentError::SortBufferTooSmall,
        bloom::project::kCanonicalDocumentNoIndex, bloom::project::kCanonicalDocumentNoIndex,
        "an exhausted sort scratch budget is rejected before any emission");

    const CanonicalDocumentV1 valid{.snapshot = &snapshot,
                                    .colorSettings = &settings,
                                    .payloadScratch = payloadScratch,
                                    .sortScratch = sortScratch};
    const auto size = bloom::project::canonicalDocumentSize(valid);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "composed preflight returns its exact golden byte count");

    const auto encoded = encodeWithSlack(valid);
    expectations.expect(encoded.ok && encoded.bytes == expected,
                        "scrambled live collections emit in canonical numeric and UTF-8 order");

    const auto repeat = encodeWithSlack(valid);
    expectations.expect(repeat.ok && repeat.bytes == encoded.bytes,
                        "repeated encodes of one snapshot are byte-identical");
}

void testExtensionGoldenBytes(Expectations& expectations) {
    constexpr std::string_view expected =
        "{\n"
        "  \"schemaVersion\": {\n"
        "    \"major\": 1,\n"
        "    \"minor\": 0\n"
        "  },\n"
        "  \"project\": {\n"
        "    \"id\": \"1\",\n"
        "    \"name\": \"Untitled Project\",\n"
        "    \"colorSettings\": {\n"
        "      \"schemaVersion\": {\n"
        "        \"major\": 1,\n"
        "        \"minor\": 0\n"
        "      },\n"
        "      \"processColorSpaceId\": \"lin_rec709_scene\",\n"
        "      \"ocioConfig\": {\n"
        "        \"schemaVersion\": {\n"
        "          \"major\": 1,\n"
        "          \"minor\": 0\n"
        "        },\n"
        "        \"locator\": {\n"
        "          \"kind\": \"builtin\",\n"
        "          \"uri\": \"bloom://ocio/neutral-v1/config.ocio\"\n"
        "        },\n"
        "        \"expectedRevision\": {\n"
        "          \"algorithm\": \"sha256\",\n"
        "          \"digest\": "
        "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"\n"
        "        },\n"
        "        \"portability\": \"builtin\",\n"
        "        \"contextVariables\": []\n"
        "      }\n"
        "    },\n"
        "    \"compositions\": [\n"
        "      {\n"
        "        \"id\": \"1\",\n"
        "        \"name\": \"Main Composition\",\n"
        "        \"duration\": {\n"
        "          \"numerator\": \"10\",\n"
        "          \"denominator\": \"1\"\n"
        "        },\n"
        "        \"format\": {\n"
        "          \"width\": 1920,\n"
        "          \"height\": 1080,\n"
        "          \"pixelAspect\": {\n"
        "            \"numerator\": \"1\",\n"
        "            \"denominator\": \"1\"\n"
        "          },\n"
        "          \"frameRate\": {\n"
        "            \"numerator\": \"24\",\n"
        "            \"denominator\": \"1\"\n"
        "          }\n"
        "        },\n"
        "        \"parameters\": [],\n"
        "        \"animationCurves\": [],\n"
        "        \"graph\": {\n"
        "          \"nodes\": [\n"
        "            {\n"
        "              \"id\": \"1\",\n"
        "              \"typeId\": \"bloom.layer-stack\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": []\n"
        "            },\n"
        "            {\n"
        "              \"id\": \"2\",\n"
        "              \"typeId\": \"bloom.composition-output\",\n"
        "              \"schemaVersion\": 1,\n"
        "              \"parameters\": []\n"
        "            }\n"
        "          ],\n"
        "          \"edges\": [\n"
        "            {\n"
        "              \"id\": \"1\",\n"
        "              \"source\": {\n"
        "                \"nodeId\": \"1\",\n"
        "                \"port\": \"image\"\n"
        "              },\n"
        "              \"destination\": {\n"
        "                \"kind\": \"node-input\",\n"
        "                \"nodeId\": \"2\",\n"
        "                \"port\": \"image\"\n"
        "              }\n"
        "            }\n"
        "          ],\n"
        "          \"layerOutputs\": [],\n"
        "          \"layerStack\": {\n"
        "            \"nodeId\": \"1\",\n"
        "            \"entries\": []\n"
        "          },\n"
        "          \"compositionOutput\": {\n"
        "            \"nodeId\": \"2\",\n"
        "            \"port\": \"image\"\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    ]\n"
        "  },\n"
        "  \"idAllocation\": {\n"
        "    \"highestIssued\": {\n"
        "      \"composition\": \"1\",\n"
        "      \"node\": \"2\",\n"
        "      \"edge\": \"1\",\n"
        "      \"layer\": \"0\",\n"
        "      \"layerSlot\": \"0\",\n"
        "      \"parameter\": \"0\",\n"
        "      \"animationCurve\": \"0\",\n"
        "      \"keyframe\": \"0\",\n"
        "      \"driverBinding\": \"0\",\n"
        "      \"extensionRecord\": \"9\"\n"
        "    }\n"
        "  },\n"
        "  \"extensions\": [\n"
        "    {\n"
        "      \"id\": \"2\",\n"
        "      \"ownerId\": \"vendor.module\",\n"
        "      \"typeId\": \"vendor.module.record-type\",\n"
        "      \"schemaVersion\": {\n"
        "        \"major\": 1,\n"
        "        \"minor\": 0\n"
        "      },\n"
        "      \"subject\": null,\n"
        "      \"mediaType\": \"text/x-vendor-note\",\n"
        "      \"referencePolicy\": {\n"
        "        \"kind\": \"owner-remapper\",\n"
        "        \"remapperId\": \"vendor.module.record-remapper\",\n"
        "        \"version\": {\n"
        "          \"major\": 1,\n"
        "          \"minor\": 0\n"
        "        }\n"
        "      },\n"
        "      \"payload\": \"3q2+7w==\"\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"5\",\n"
        "      \"ownerId\": \"vendor.module\",\n"
        "      \"typeId\": \"vendor.module.marker\",\n"
        "      \"schemaVersion\": {\n"
        "        \"major\": 1,\n"
        "        \"minor\": 0\n"
        "      },\n"
        "      \"subject\": {\n"
        "        \"kind\": \"composition\",\n"
        "        \"id\": \"1\"\n"
        "      },\n"
        "      \"mediaType\": \"application/octet-stream\",\n"
        "      \"referencePolicy\": {\n"
        "        \"kind\": \"none\"\n"
        "      },\n"
        "      \"payload\": \"AA==\"\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"9\",\n"
        "      \"ownerId\": \"vendor.module\",\n"
        "      \"typeId\": \"vendor.module.link-table\",\n"
        "      \"schemaVersion\": {\n"
        "        \"major\": 1,\n"
        "        \"minor\": 0\n"
        "      },\n"
        "      \"subject\": null,\n"
        "      \"mediaType\": \"application/x-vendor-table\",\n"
        "      \"referencePolicy\": {\n"
        "        \"kind\": \"host-table\",\n"
        "        \"references\": [\n"
        "          {\n"
        "            \"key\": \"primary-comp\",\n"
        "            \"target\": {\n"
        "              \"kind\": \"composition\",\n"
        "              \"id\": \"1\"\n"
        "            }\n"
        "          }\n"
        "        ]\n"
        "      },\n"
        "      \"payload\": \"\"\n"
        "    }\n"
        "  ]\n"
        "}\n";

    using namespace bloom::document;
    auto newProject = makeMinimalProject();
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
                        "the extension fixture records insert out of numeric ID order");

    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 requestNoPayloadBuffer{.snapshot = &snapshot,
                                                     .colorSettings = &settings,
                                                     .payloadScratch = {},
                                                     .sortScratch = sortScratch};
    expectRequestError(
        expectations, requestNoPayloadBuffer, {}, CanonicalDocumentError::PayloadBufferTooSmall,
        bloom::project::kCanonicalDocumentNoIndex, bloom::project::kCanonicalDocumentNoIndex,
        "an exhausted payload scratch budget is rejected before any emission");

    const CanonicalDocumentV1 valid{.snapshot = &snapshot,
                                    .colorSettings = &settings,
                                    .payloadScratch = payloadScratch,
                                    .sortScratch = sortScratch};
    const auto size = bloom::project::canonicalDocumentSize(valid);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "extension preflight returns its exact golden byte count");

    const auto encoded = encodeWithSlack(valid);
    expectations.expect(encoded.ok && encoded.bytes == expected,
                        "extension envelopes emit every policy shape and canonical base64");
}

void testInvalidColorSettingsRejections(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto noIndex = bloom::project::kCanonicalDocumentNoIndex;

    auto wrongProcessSpace = neutralColorSettings();
    wrongProcessSpace.processColorSpaceId = "rec709_display";
    const CanonicalDocumentV1 processRequest{.snapshot = &snapshot,
                                             .colorSettings = &wrongProcessSpace,
                                             .payloadScratch = payloadScratch,
                                             .sortScratch = sortScratch};
    expectRequestError(expectations, processRequest, {},
                       CanonicalDocumentError::InvalidProcessColorSpaceId, noIndex, noIndex,
                       "only the fixed v1 process color space identifier is writable");

    auto unknownRevision = neutralColorSettings();
    unknownRevision.ocioConfig.expectedRevision.algorithm =
        bloom::document::OcioRevisionAlgorithm::Unknown;
    const CanonicalDocumentV1 revisionRequest{.snapshot = &snapshot,
                                              .colorSettings = &unknownRevision,
                                              .payloadScratch = payloadScratch,
                                              .sortScratch = sortScratch};
    expectRequestError(expectations, revisionRequest, {},
                       CanonicalDocumentError::InvalidOcioRevisionAlgorithm, noIndex, noIndex,
                       "the revision algorithm must be SHA-256 before encoding");

    auto mismatchedPortability = neutralColorSettings();
    mismatchedPortability.ocioConfig.portability = bloom::document::OcioConfigPortability::External;
    const CanonicalDocumentV1 portabilityRequest{.snapshot = &snapshot,
                                                 .colorSettings = &mismatchedPortability,
                                                 .payloadScratch = payloadScratch,
                                                 .sortScratch = sortScratch};
    expectRequestError(expectations, portabilityRequest, {},
                       CanonicalDocumentError::OcioPortabilityMismatch, noIndex, noIndex,
                       "portability must agree with the locator family");

    auto foreignBuiltinUri = neutralColorSettings();
    foreignBuiltinUri.ocioConfig.locator =
        bloom::document::BuiltInOcioConfigLocator{"bloom://ocio/neutral-v2/config.ocio"};
    const CanonicalDocumentV1 uriRequest{.snapshot = &snapshot,
                                         .colorSettings = &foreignBuiltinUri,
                                         .payloadScratch = payloadScratch,
                                         .sortScratch = sortScratch};
    expectRequestError(expectations, uriRequest, {}, CanonicalDocumentError::InvalidColorSettings,
                       noIndex, noIndex,
                       "the only writable builtin locator is the qualified Bloom Neutral URI");

    auto unsortedContext = neutralColorSettings();
    unsortedContext.ocioConfig.contextVariables = {
        {"BETA", "1"},
        {"ALPHA", "2"},
    };
    const CanonicalDocumentV1 contextRequest{.snapshot = &snapshot,
                                             .colorSettings = &unsortedContext,
                                             .payloadScratch = payloadScratch,
                                             .sortScratch = sortScratch};
    expectRequestError(expectations, contextRequest, {},
                       CanonicalDocumentError::InvalidColorSettings, noIndex, noIndex,
                       "context variables must already be unique and sorted by UTF-8 name");

    auto staleSchema = neutralColorSettings();
    staleSchema.schemaVersion = {2, 0};
    const CanonicalDocumentV1 schemaRequest{.snapshot = &snapshot,
                                            .colorSettings = &staleSchema,
                                            .payloadScratch = payloadScratch,
                                            .sortScratch = sortScratch};
    expectRequestError(expectations, schemaRequest, {},
                       CanonicalDocumentError::InvalidColorSettings, noIndex, noIndex,
                       "color settings schema version is fixed at 1.0");
}

void testDriverSourceAdmission(Expectations& expectations) {
    using namespace bloom::document;
    auto duration = RationalTime::create(240, 24);
    if (!duration.has_value()) {
        expectations.expect(false, "the driver fixture duration is constructible");
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Driver Project", "Main Composition", *duration);
    auto* composition = newProject.project.findComposition(newProject.initialCompositionId);
    if (composition == nullptr) {
        expectations.expect(false, "the driver fixture composition exists");
        return;
    }
    expectations.expect(composition->parameters().insert(
                            {ParameterId::fromRaw(2), std::string(kOpacityParameterSchemaKey),
                             DriverBindingSource{DriverBindingId::fromRaw(4)}}),
                        "the driver fixture parameter inserts with a live driver source");

    IdAllocatorHighWater highWater{};
    highWater.composition = 1;
    highWater.node = 2;
    highWater.edge = 1;
    highWater.parameter = 2;
    highWater.driverBinding = 4;
    Document document{std::move(newProject.project), highWater};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch};
    expectRequestError(expectations, request, {},
                       CanonicalDocumentError::UnsupportedDriverBindingSource, 0, 0,
                       "a live driver source fails admission with its composition and parameter");
}

void testLimitsAndCapacityAdversarial(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch};

    CanonicalDocumentLimits raisedCeilings;
    raisedCeilings.maximumValues = bloom::project::kCanonicalJsonMaximumValues + 1;
    expectRequestError(expectations, request, raisedCeilings, CanonicalDocumentError::InvalidLimits,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "caller budgets cannot raise the accepted v1 ceilings");

    CanonicalDocumentLimits starvedValues;
    starvedValues.maximumValues = 10;
    expectRequestError(
        expectations, request, starvedValues, CanonicalDocumentError::ValueCountExceeded,
        bloom::project::kCanonicalDocumentNoIndex, bloom::project::kCanonicalDocumentNoIndex,
        "a lower value budget rejects during preflight accounting");

    std::array<char, 64> starvedOutput{};
    starvedOutput.fill('?');
    const auto starvedEncode =
        bloom::project::encodeCanonicalDocument(request, starvedOutput, starvedValues);
    expectations.expect(!starvedEncode &&
                            starvedEncode.error() == CanonicalDocumentError::ValueCountExceeded &&
                            starvedEncode.bytesWritten() == 0 && starvedOutput.front() == '?' &&
                            starvedOutput.back() == '?',
                        "a value-budget failure leaves the destination untouched");

    CanonicalDocumentLimits starvedContainers;
    starvedContainers.maximumContainerEntries = 3;
    expectRequestError(expectations, request, starvedContainers,
                       CanonicalDocumentError::ContainerEntryCountExceeded,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "the root object member count is checked against the container budget");

    const auto exact = bloom::project::canonicalDocumentSize(request);
    if (!exact.hasValue()) {
        expectations.expect(false, "the capacity fixture preflights successfully");
        return;
    }

    CanonicalDocumentLimits oneByteShort;
    oneByteShort.maximumOutputBytes = *exact.value() - 1;
    expectRequestError(
        expectations, request, oneByteShort, CanonicalDocumentError::DocumentSizeExceeded,
        bloom::project::kCanonicalDocumentNoIndex, bloom::project::kCanonicalDocumentNoIndex,
        "the exact canonical byte count is checked against the output limit");

    CanonicalDocumentLimits atLimit;
    atLimit.maximumOutputBytes = *exact.value();
    expectations.expect(bloom::project::canonicalDocumentSize(request, atLimit).hasValue(),
                        "an output limit equal to the exact canonical size succeeds");

    std::vector<char> shortOutput(*exact.value() - 1, '?');
    const auto shortResult = bloom::project::encodeCanonicalDocument(request, shortOutput);
    expectations.expect(
        !shortResult && shortResult.error() == CanonicalDocumentError::OutputCapacityExceeded &&
            shortResult.requiredSize().has_value() &&
            *shortResult.requiredSize() == *exact.value() && shortResult.bytesWritten() == 0 &&
            shortOutput.front() == '?' && shortOutput.back() == '?',
        "capacity shortage reports the exact requirement and writes nothing");

    std::vector<char> exactOutput(*exact.value(), '?');
    const auto exactResult = bloom::project::encodeCanonicalDocument(request, exactOutput);
    expectations.expect(exactResult && exactResult.bytesWritten() == *exact.value() &&
                            exactOutput.back() == '\n',
                        "exact capacity emits the complete document ending in one LF");
}

// ---------------------------------------------------------------------------------------------
// RT2: writer-side overlay (docs/architecture/project-format.md, "Versions, Migrations, And
// Preservation"). Fixture strategy: rather than hand-transcribing a second large canonical-bytes
// literal per test (error-prone at this size and hard to keep visibly correct), every test below
// starts from an already-verified canonical text -- kMinimalDocumentGolden above, or a fixture's
// own plain {1,0} encode (itself checked against the same writer this suite already trusts) --
// and applies small, explicit, anchor-verified edits that splice one retained member at one exact
// canonical position. requireReplace() aborts loudly on an anchor miss (a fixture bug, not a
// condition this suite reports through Expectations) rather than silently building a wrong golden.
// ---------------------------------------------------------------------------------------------

using bloom::project::DocumentClassification;
using bloom::project::DocumentDecodeOutcome;

constexpr std::uint64_t kGenerousOperationBudget = 8ULL << 20U; // 8 MiB: ample for every fixture.

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

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

[[nodiscard]] DocumentDecodeResult decodeText(const std::string& text) {
    auto parsed = bloom::project::parseStrictJsonDom(asBytes(text), {},
                                                     makeOperation(kGenerousOperationBudget));
    if (!parsed) {
        std::cerr << "RT2 fixture failed to parse as strict JSON (error="
                  << static_cast<int>(parsed.error()) << ")\n";
        std::abort();
    }
    return bloom::project::decodeDocumentEnvelope(parsed.document()->root());
}

[[nodiscard]] UnknownJsonNumber unknownNumber(const std::string_view canonicalToken) {
    const auto parsed = bloom::project::parseUnknownJsonNumber(canonicalToken);
    if (!parsed.hasValue()) {
        std::abort();
    }
    return *parsed.value();
}

// Replaces the first (and, at every call site below, only) occurrence of `anchor` in `text` with
// `replacement`. Aborts immediately if `anchor` is missing: that means a fixture edit above it
// already changed the text this call expected, or the anchor was never correct -- either way a
// fixture-construction bug, not a runtime document condition.
void requireReplace(std::string& text, const std::string_view anchor,
                    const std::string_view replacement) {
    const auto pos = text.find(anchor);
    if (pos == std::string::npos) {
        std::cerr << "RT2 fixture splice anchor not found:\n" << anchor << '\n';
        std::abort();
    }
    text.replace(pos, anchor.size(), replacement);
}

// Byte-equality assertion that also prints the first differing offset on failure, since a wrong
// splice/indent among these large fixtures is otherwise tedious to localize from a bare pass/fail.
void expectBytesEqual(Expectations& expectations, const std::string_view actual,
                      const std::string_view expected, const std::string_view message) {
    if (actual == expected) {
        expectations.expect(true, message);
        return;
    }
    std::size_t mismatch = 0;
    while (mismatch < actual.size() && mismatch < expected.size() &&
           actual[mismatch] == expected[mismatch]) {
        ++mismatch;
    }
    std::cerr << "RT2 byte mismatch for \"" << message << "\" at offset " << mismatch
              << " (actual size " << actual.size() << ", expected size " << expected.size()
              << ")\n  actual:   ..." << actual.substr(mismatch, 80) << "...\n  expected: ..."
              << expected.substr(mismatch, 80) << "...\n";
    expectations.expect(false, message);
}

// Plain writes (null RoundTripState) reproduce every pre-RT2 golden byte-for-byte with the new
// CanonicalDocumentV1 fields explicitly at their plain-write defaults.
void testPlainWriteExplicitDefaultsUnchanged(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = nullptr,
                                      .schemaMinor = 0};
    const auto encoded = encodeWithSlack(request);
    expectations.expect(encoded.ok && encoded.bytes == kMinimalDocumentGolden,
                        "explicit null overlay and minor 0 reproduce the exact pre-RT2 golden");
}

// Version parameterization: schemaMinor alone (no overlay) changes only the root schemaVersion.
void testSchemaMinorParameterization(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = nullptr,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "schemaMinor=1 with no overlay sizes exactly with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "schemaMinor=1 with no overlay emits {1, 1} and is otherwise "
                     "byte-identical");
}

// The document root itself is an attachment point (its schema path is the empty path).
void testOverlayRootAttachmentPoint(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected, "  \"extensions\": []\n}\n",
                   "  \"extensions\": [],\n  \"zzzRoot\": true\n}\n");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> rootMembers;
    rootMembers.emplace_back("zzzRoot", RetainedJsonValue(true));
    roundTrip.attach({}, std::move(rootMembers));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "a root-attached retained member sizes exactly with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "a root-attached retained member re-emits after every known root member");
}

// A singleton schema-path attachment point nested one level in (project).
void testOverlayProjectAttachmentPoint(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected, "    ]\n  },\n  \"idAllocation\"",
                   "    ],\n    \"zzzProject\": \"hello\"\n  },\n  \"idAllocation\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> members;
    members.emplace_back("zzzProject", RetainedJsonValue(std::string("hello")));
    roundTrip.attach({RoundTripPathSegment::named("project")}, std::move(members));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "a project-attached retained member sizes exactly with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "a project-attached retained member re-emits after compositions");
}

// A collection-element attachment point (Composition:"1") and its nested singleton (format),
// combined so this test also covers a retained deep subtree (an object whose member is an array
// mixing numbers and a string) and a retained lossless integer whose input spelling is already
// canonical (byte equality proves no re-derivation/renormalization happened).
void testOverlayCompositionAndFormatAttachmentPoints(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected,
                   "          \"frameRate\": {\n"
                   "            \"numerator\": \"24\",\n"
                   "            \"denominator\": \"1\"\n"
                   "          }\n"
                   "        },\n"
                   "        \"parameters\"",
                   "          \"frameRate\": {\n"
                   "            \"numerator\": \"24\",\n"
                   "            \"denominator\": \"1\"\n"
                   "          },\n"
                   "          \"zzzFormat\": {\n"
                   "            \"list\": [\n"
                   "              1,\n"
                   "              2,\n"
                   "              \"three\"\n"
                   "            ],\n"
                   "            \"note\": \"deep\"\n"
                   "          }\n"
                   "        },\n"
                   "        \"parameters\"");
    requireReplace(expected,
                   "          }\n"
                   "        }\n"
                   "      }\n"
                   "    ]\n"
                   "  },\n"
                   "  \"idAllocation\"",
                   "          }\n"
                   "        },\n"
                   "        \"zzzComp\": -7\n"
                   "      }\n"
                   "    ]\n"
                   "  },\n"
                   "  \"idAllocation\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> compositionMembers;
    compositionMembers.emplace_back("zzzComp", RetainedJsonValue(unknownNumber("-7")));
    roundTrip.attach(
        {RoundTripPathSegment::named("project"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1")},
        std::move(compositionMembers));

    std::vector<RetainedJsonValue> listElements;
    listElements.emplace_back(unknownNumber("1"));
    listElements.emplace_back(unknownNumber("2"));
    listElements.emplace_back(std::string("three"));
    std::vector<RetainedJsonMember> formatValueMembers;
    formatValueMembers.emplace_back("list", RetainedJsonValue::makeArray(std::move(listElements)));
    formatValueMembers.emplace_back("note", RetainedJsonValue(std::string("deep")));
    std::vector<RetainedJsonMember> formatMembers;
    formatMembers.emplace_back("zzzFormat",
                               RetainedJsonValue::makeObject(std::move(formatValueMembers)));
    roundTrip.attach(
        {RoundTripPathSegment::named("project"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
         RoundTripPathSegment::named("format")},
        std::move(formatMembers));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "composition- and format-attached retained members size exactly with the "
                        "golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(
        expectations, encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
        "a retained deep subtree and a canonical-spelling lossless integer both re-emit "
        "byte-identically");
}

// A collection-element attachment point nested two levels in (a graph node).
void testOverlayNodeAttachmentPoint(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected,
                   "              \"parameters\": []\n"
                   "            },\n"
                   "            {\n"
                   "              \"id\": \"2\"",
                   "              \"parameters\": [],\n"
                   "              \"zzzNode\": null\n"
                   "            },\n"
                   "            {\n"
                   "              \"id\": \"2\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> members;
    members.emplace_back("zzzNode", RetainedJsonValue{});
    roundTrip.attach(
        {RoundTripPathSegment::named("project"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
         RoundTripPathSegment::named("graph"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Node, "1")},
        std::move(members));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "a node-attached retained member sizes exactly with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "a null-valued retained member on a graph node re-emits exactly");
}

// A second collection kind under graph (Edge), demonstrating the identity model generalizes past
// Composition/Node.
void testOverlayEdgeAttachmentPoint(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected,
                   "              }\n"
                   "            }\n"
                   "          ],\n"
                   "          \"layerOutputs\"",
                   "              },\n"
                   "              \"zzzEdge\": \"e\"\n"
                   "            }\n"
                   "          ],\n"
                   "          \"layerOutputs\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> members;
    members.emplace_back("zzzEdge", RetainedJsonValue(std::string("e")));
    roundTrip.attach(
        {RoundTripPathSegment::named("project"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "1"),
         RoundTripPathSegment::named("graph"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Edge, "1")},
        std::move(members));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "an edge-attached retained member sizes exactly with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "an edge-attached retained member re-emits after destination");
}

// Two nested singleton attachment points (idAllocation, and highestIssued inside it), the second
// carrying a retained array of booleans.
void testOverlayIdAllocationAttachmentPoints(Expectations& expectations) {
    std::string expected(kMinimalDocumentGolden);
    requireReplace(expected, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");
    requireReplace(expected,
                   "      \"extensionRecord\": \"0\"\n"
                   "    }\n"
                   "  },\n"
                   "  \"extensions\"",
                   "      \"extensionRecord\": \"0\",\n"
                   "      \"zzzHighWater\": 5\n"
                   "    },\n"
                   "    \"zzzIdAllocation\": [\n"
                   "      true,\n"
                   "      false\n"
                   "    ]\n"
                   "  },\n"
                   "  \"extensions\"");

    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> highWaterMembers;
    highWaterMembers.emplace_back("zzzHighWater", RetainedJsonValue(unknownNumber("5")));
    roundTrip.attach(
        {RoundTripPathSegment::named("idAllocation"), RoundTripPathSegment::named("highestIssued")},
        std::move(highWaterMembers));

    std::vector<RetainedJsonValue> boolElements;
    boolElements.emplace_back(true);
    boolElements.emplace_back(false);
    std::vector<RetainedJsonMember> idAllocationMembers;
    idAllocationMembers.emplace_back("zzzIdAllocation",
                                     RetainedJsonValue::makeArray(std::move(boolElements)));
    roundTrip.attach({RoundTripPathSegment::named("idAllocation")}, std::move(idAllocationMembers));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(size.hasValue() && *size.value() == expected.size(),
                        "idAllocation- and highestIssued-attached retained members size exactly "
                        "with the golden");
    const auto encoded = encodeWithSlack(request);
    expectBytesEqual(expectations,
                     encoded.ok ? std::string_view(encoded.bytes) : std::string_view{}, expected,
                     "nested idAllocation/highestIssued retained members re-emit at their own "
                     "exact positions");
}

// An attachment path naming a collection element the walk never visits (here, a Composition id
// that does not exist in the live document being written) is a typed error, never silently
// dropped -- the contract this suite must enforce for every RoundTripState an overlay accepts.
void testOverlayLeftoverEntryIsTypedError(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> members;
    members.emplace_back("zzzGhost", RetainedJsonValue(true));
    roundTrip.attach(
        {RoundTripPathSegment::named("project"),
         RoundTripPathSegment::collectionElement(RoundTripCollectionKind::Composition, "999")},
        std::move(members));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    expectations.expect(!size.hasValue() &&
                            size.error() == CanonicalDocumentError::RoundTripStateMismatch,
                        "an attachment point the walk never visits is a typed leftover error, "
                        "never silently dropped");
    expectations.expect(size.compositionIndex() == bloom::project::kCanonicalDocumentNoIndex &&
                            size.elementIndex() == bloom::project::kCanonicalDocumentNoIndex,
                        "the leftover-entry error carries no composition/element location -- it "
                        "is a document-wide state mismatch, not a per-record failure");

    std::array<char, 8> tinyOutput{};
    tinyOutput.fill('?');
    const auto encode = bloom::project::encodeCanonicalDocument(request, tinyOutput);
    expectations.expect(!encode &&
                            encode.error() == CanonicalDocumentError::RoundTripStateMismatch &&
                            encode.bytesWritten() == 0 && tinyOutput.front() == '?',
                        "the encode path reports the same leftover-entry error and writes "
                        "nothing");
}

// Count-then-write byte-equality invariant exercised with an overlay present: an exact-size buffer
// succeeds and repeated sizing is deterministic; one byte short fails cleanly and reports the
// exact requirement, mirroring testLimitsAndCapacityAdversarial's plain-write coverage.
void testOverlayCapacityBoundary(Expectations& expectations) {
    auto newProject = makeMinimalProject();
    Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();

    RoundTripState roundTrip;
    std::vector<RetainedJsonMember> members;
    members.emplace_back("zzzProject", RetainedJsonValue(std::string("hello")));
    roundTrip.attach({RoundTripPathSegment::named("project")}, std::move(members));

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 request{.snapshot = &snapshot,
                                      .colorSettings = &settings,
                                      .payloadScratch = payloadScratch,
                                      .sortScratch = sortScratch,
                                      .roundTrip = &roundTrip,
                                      .schemaMinor = 1};
    const auto size = bloom::project::canonicalDocumentSize(request);
    if (!size.hasValue()) {
        expectations.expect(false, "the overlay capacity fixture preflights successfully");
        return;
    }
    const auto exactSize = *size.value();
    expectations.expect(exactSize > kMinimalDocumentGolden.size(),
                        "the overlay-attached retained member adds bytes beyond the plain golden "
                        "size");

    std::vector<char> exactOutput(exactSize, '?');
    const auto exactResult = bloom::project::encodeCanonicalDocument(request, exactOutput);
    expectations.expect(exactResult && exactResult.bytesWritten() == exactSize &&
                            exactOutput.back() == '\n',
                        "an exact-size buffer succeeds with an overlay present");

    std::vector<char> shortOutput(exactSize - 1, '?');
    const auto shortResult = bloom::project::encodeCanonicalDocument(request, shortOutput);
    expectations.expect(
        !shortResult && shortResult.error() == CanonicalDocumentError::OutputCapacityExceeded &&
            shortResult.requiredSize().has_value() && *shortResult.requiredSize() == exactSize &&
            shortResult.bytesWritten() == 0 && shortOutput.front() == '?' &&
            shortOutput.back() == '?',
        "a one-byte-short buffer fails cleanly with the exact requirement reported, overlay "
        "present");

    const auto repeatSize = bloom::project::canonicalDocumentSize(request);
    expectations.expect(repeatSize.hasValue() && *repeatSize.value() == exactSize,
                        "repeated sizing with an overlay present is deterministic");
}

// ---------------------------------------------------------------------------------------------
// THE preservation determinism cycle: canonical 1.1 bytes with unknown trailing members at seven
// attachment kinds (root, project, a composition, that composition's format, a node, an extension
// record, and a layer-stack entry) -> parse -> decode (EditableWithRoundTrip) -> reconstruct the
// live document -> re-encode with the overlay and minor 1 -> byte-identical to the original input.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] Document buildPreservationCycleFixtureDocument() {
    using namespace bloom::document;

    const auto duration = RationalTime::create(10, 1);
    const auto pixelAspect = PixelAspectRatio::create(1, 1);
    const auto frameRate = FrameRate::create(24, 1);
    const auto format =
        CompositionFormat::create(1920, 1080, pixelAspect.value_or(PixelAspectRatio::square()),
                                  frameRate.value_or(FrameRate::framesPerSecond24()));
    if (!duration.has_value() || !format.has_value()) {
        std::abort();
    }

    CanonicalGraph graph{NodeId::fromRaw(1)};
    const NodeRecord layerStackNode{
        NodeId::fromRaw(1), std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion};
    const NodeRecord layerOutputNode{
        NodeId::fromRaw(2),
        std::string(kLayerOutputNodeType),
        {{"opacity", ParameterId::fromRaw(3)}, {"position", ParameterId::fromRaw(5)}},
        kLayerOutputNodeSchemaVersion};
    const NodeRecord compositionOutputNode{NodeId::fromRaw(3),
                                           std::string(kCompositionOutputNodeType),
                                           {},
                                           kCompositionOutputNodeSchemaVersion};
    const bool nodesAdded = graph.addNode(layerStackNode) && graph.addNode(layerOutputNode) &&
                            graph.addNode(compositionOutputNode);
    const EdgeRecord layerToStackEdge{EdgeId::fromRaw(1),
                                      {NodeId::fromRaw(2), std::string(kLayerOutputOutputPort)},
                                      LayerStackInputRef{NodeId::fromRaw(1),
                                                         LayerSlotId::fromRaw(1),
                                                         std::string(kLayerStackContentInputRole)}};
    const EdgeRecord stackToOutputEdge{
        EdgeId::fromRaw(2),
        {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
        NodeInputRef{NodeId::fromRaw(3), std::string(kCompositionOutputInputPort)}};
    const bool edgesAdded = graph.addEdge(layerToStackEdge) && graph.addEdge(stackToOutputEdge);
    const bool boundaryAdded = graph.addLayerOutput(
        {NodeId::fromRaw(2), LayerId::fromRaw(1), "Layer", std::string(kLayerOutputOutputPort)});
    const bool entryAdded =
        graph.layerStack().append({LayerSlotId::fromRaw(1), LayerId::fromRaw(1)});
    if (!nodesAdded || !edgesAdded || !boundaryAdded || !entryAdded) {
        std::abort();
    }
    graph.setCompositionOutput({NodeId::fromRaw(3), std::string(kCompositionOutputOutputPort)});

    Composition composition{CompositionId::fromRaw(1), "Comp", *duration, std::move(graph),
                            *format};
    const bool paramsInserted =
        composition.parameters().insert({ParameterId::fromRaw(3),
                                         std::string(kOpacityParameterSchemaKey),
                                         ConstantValueSource{1.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(5),
                                         std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{0.0, 0.0}}});
    if (!paramsInserted) {
        std::abort();
    }

    Project project{ProjectId::fromRaw(1), "RT2 Fixture"};
    if (!project.addComposition(std::move(composition))) {
        std::abort();
    }
    const ExtensionRecord markerRecord{
        ExtensionRecordId::fromRaw(1), "vendor.module",
        "vendor.module.marker",        {1, 0},
        CompositionId::fromRaw(1),     "application/octet-stream",
        NoExtensionReferences{},       OpaqueExtensionPayload{std::byte{0x00}}};
    if (!project.addExtensionRecord(markerRecord)) {
        std::abort();
    }

    IdAllocatorHighWater highWater{};
    highWater.composition = 1;
    highWater.node = 3;
    highWater.edge = 2;
    highWater.layer = 1;
    highWater.layerSlot = 1;
    highWater.parameter = 5;
    highWater.extensionRecord = 1;
    return Document{std::move(project), highWater};
}

void testPreservationDeterminismCycle(Expectations& expectations) {
    auto document = buildPreservationCycleFixtureDocument();
    auto snapshot = document.snapshot();

    std::array<char, kScratchSize> payloadScratch{};
    std::array<std::size_t, kScratchSize> sortScratch{};
    const auto settings = neutralColorSettings();
    const CanonicalDocumentV1 plainRequest{.snapshot = &snapshot,
                                           .colorSettings = &settings,
                                           .payloadScratch = payloadScratch,
                                           .sortScratch = sortScratch};
    const auto plainEncoded = encodeWithSlack(plainRequest);
    expectations.expect(plainEncoded.ok, "the RT2 cycle fixture's plain {1,0} encode succeeds");
    if (!plainEncoded.ok) {
        return;
    }

    std::string original(plainEncoded.bytes);

    requireReplace(original, "\"minor\": 0\n  },\n  \"project\"",
                   "\"minor\": 1\n  },\n  \"project\"");

    requireReplace(original,
                   "                \"slotId\": \"1\",\n"
                   "                \"layerId\": \"1\"\n"
                   "              }\n",
                   "                \"slotId\": \"1\",\n"
                   "                \"layerId\": \"1\",\n"
                   "                \"zzzEntry\": \"slot-note\"\n"
                   "              }\n");

    requireReplace(original,
                   "              \"typeId\": \"bloom.composition-output\",\n"
                   "              \"schemaVersion\": 1,\n"
                   "              \"parameters\": []\n"
                   "            }\n",
                   "              \"typeId\": \"bloom.composition-output\",\n"
                   "              \"schemaVersion\": 1,\n"
                   "              \"parameters\": [],\n"
                   "              \"zzzNode\": null\n"
                   "            }\n");

    requireReplace(original,
                   "          \"frameRate\": {\n"
                   "            \"numerator\": \"24\",\n"
                   "            \"denominator\": \"1\"\n"
                   "          }\n"
                   "        },\n"
                   "        \"parameters\"",
                   "          \"frameRate\": {\n"
                   "            \"numerator\": \"24\",\n"
                   "            \"denominator\": \"1\"\n"
                   "          },\n"
                   "          \"zzzFormat\": {\n"
                   "            \"list\": [\n"
                   "              1,\n"
                   "              2,\n"
                   "              \"three\",\n"
                   "              [\n"
                   "                4,\n"
                   "                5\n"
                   "              ]\n"
                   "            ],\n"
                   "            \"note\": \"deep\"\n"
                   "          }\n"
                   "        },\n"
                   "        \"parameters\"");

    requireReplace(original,
                   "          \"compositionOutput\": {\n"
                   "            \"nodeId\": \"3\",\n"
                   "            \"port\": \"image\"\n"
                   "          }\n"
                   "        }\n"
                   "      }\n"
                   "    ]\n"
                   "  },\n"
                   "  \"idAllocation\"",
                   "          \"compositionOutput\": {\n"
                   "            \"nodeId\": \"3\",\n"
                   "            \"port\": \"image\"\n"
                   "          }\n"
                   "        },\n"
                   "        \"zzzComp\": true\n"
                   "      }\n"
                   "    ]\n"
                   "  },\n"
                   "  \"idAllocation\"");

    requireReplace(original, "    ]\n  },\n  \"idAllocation\"",
                   "    ],\n    \"zzzProject\": \"hello world\"\n  },\n  \"idAllocation\"");

    requireReplace(original, "      \"payload\": \"AA==\"\n    }\n",
                   "      \"payload\": \"AA==\",\n      \"zzzExt\": 1.5\n    }\n");

    requireReplace(original, "      \"zzzExt\": 1.5\n    }\n  ]\n}\n",
                   "      \"zzzExt\": 1.5\n    }\n  ],\n  \"zzzRoot\": 42\n}\n");

    const auto decoded = decodeText(original);
    expectations.expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            static_cast<bool>(decoded) && decoded.value() != nullptr,
                        "the spliced 1.1 fixture decodes with a trusted envelope");
    expectations.expect(decoded.classification() == DocumentClassification::EditableWithRoundTrip,
                        "the spliced 1.1 fixture classifies EditableWithRoundTrip");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        expectations.expect(false, "a RoundTripState is present for the spliced fixture");
        return;
    }
    expectations.expect(decoded.roundTrip()->size() == 7,
                        "exactly seven attachment points were captured (root, project, "
                        "composition, format, node, extension record, layer-stack entry)");
    {
        const RoundTripAttachmentPath extensionPath{
            RoundTripPathSegment::collectionElement(RoundTripCollectionKind::ExtensionRecord, "1")};
        const auto* members = decoded.roundTrip()->find(extensionPath);
        expectations.expect(
            members != nullptr && members->size() == 1 && (*members)[0].key() == "zzzExt" &&
                (*members)[0].value().kind() == bloom::project::RetainedJsonValueKind::Number,
            "the extension record attachment point captured its exact retained member, keyed by "
            "numeric ExtensionRecordId directly off the document root");
    }

    auto reconstructed = bloom::project::reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed), "the spliced fixture reconstructs");
    if (!reconstructed) {
        return;
    }
    auto* reconstructedValue = reconstructed.value();
    if (reconstructedValue == nullptr || reconstructedValue->document == nullptr) {
        expectations.expect(false, "reconstruction produced a live Document");
        return;
    }
    auto reconstructedSnapshot = reconstructedValue->document->snapshot();
    expectations.expect(reconstructedSnapshot.ids().highWater() == snapshot.ids().highWater(),
                        "reconstruction from the spliced fixture preserves the exact allocator "
                        "high-water state");

    std::array<char, kScratchSize> overlayPayloadScratch{};
    std::array<std::size_t, kScratchSize> overlaySortScratch{};
    const CanonicalDocumentV1 overlayRequest{.snapshot = &reconstructedSnapshot,
                                             .colorSettings = &reconstructedValue->colorSettings,
                                             .payloadScratch = overlayPayloadScratch,
                                             .sortScratch = overlaySortScratch,
                                             .roundTrip = decoded.roundTrip(),
                                             .schemaMinor = 1};
    const auto overlaySize = bloom::project::canonicalDocumentSize(overlayRequest);
    expectations.expect(overlaySize.hasValue() && *overlaySize.value() == original.size(),
                        "the overlay re-encode sizes exactly to the spliced original's byte "
                        "count");

    const auto overlayEncoded = encodeWithSlack(overlayRequest);
    expectBytesEqual(
        expectations,
        overlayEncoded.ok ? std::string_view(overlayEncoded.bytes) : std::string_view{}, original,
        "the overlay re-encode is byte-identical to the spliced original: parse -> decode -> "
        "reconstruct -> re-encode reproduces preserved intent exactly");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testMinimalGoldenBytes(expectations);
        testComposedGoldenBytes(expectations);
        testExtensionGoldenBytes(expectations);
        testInvalidColorSettingsRejections(expectations);
        testDriverSourceAdmission(expectations);
        testLimitsAndCapacityAdversarial(expectations);
        testPlainWriteExplicitDefaultsUnchanged(expectations);
        testSchemaMinorParameterization(expectations);
        testOverlayRootAttachmentPoint(expectations);
        testOverlayProjectAttachmentPoint(expectations);
        testOverlayCompositionAndFormatAttachmentPoints(expectations);
        testOverlayNodeAttachmentPoint(expectations);
        testOverlayEdgeAttachmentPoint(expectations);
        testOverlayIdAllocationAttachmentPoints(expectations);
        testOverlayLeftoverEntryIsTypedError(expectations);
        testOverlayCapacityBoundary(expectations);
        testPreservationDeterminismCycle(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
