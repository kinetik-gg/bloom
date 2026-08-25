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
#include <bloom/document/project.hpp>

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
                        const std::size_t expectedElementIndex,
                        const std::string_view message) {
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

void testMinimalGoldenBytes(Expectations& expectations) {
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
        "      \"extensionRecord\": \"0\"\n"
        "    }\n"
        "  },\n"
        "  \"extensions\": []\n"
        "}\n";

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
    const auto format = CompositionFormat::create(
        1280, 720, pixelAspect.value_or(PixelAspectRatio::square()),
        frameRate.value_or(FrameRate::framesPerSecond24()));
    if (!duration.has_value() || !format.has_value()) {
        expectations.expect(false, "the composed fixture values are constructible");
        return;
    }

    CanonicalGraph graph{NodeId::fromRaw(1)};
    const NodeRecord layerOutputNode{NodeId::fromRaw(3),
                                     std::string(kLayerOutputNodeType),
                                     {{"opacity", ParameterId::fromRaw(3)},
                                      {"position", ParameterId::fromRaw(5)}},
                                     kLayerOutputNodeSchemaVersion};
    const NodeRecord layerStackNode{NodeId::fromRaw(1), std::string(kLayerStackNodeType), {},
                                    kLayerStackNodeSchemaVersion};
    const NodeRecord compositionOutputNode{NodeId::fromRaw(4),
                                           std::string(kCompositionOutputNodeType),
                                           {},
                                           kCompositionOutputNodeSchemaVersion};
    const NodeRecord solidSourceNode{NodeId::fromRaw(2),
                                     std::string(kSolidSourceNodeType),
                                     {{"color", ParameterId::fromRaw(7)}},
                                     kSolidSourceNodeSchemaVersion};
    const bool nodesAdded =
        graph.addNode(layerOutputNode) && graph.addNode(layerStackNode) &&
        graph.addNode(compositionOutputNode) && graph.addNode(solidSourceNode);
    const EdgeRecord stackToOutputEdge{
        EdgeId::fromRaw(3),
        {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
        NodeInputRef{NodeId::fromRaw(4), std::string(kCompositionOutputInputPort)}};
    const EdgeRecord solidToLayerEdge{
        EdgeId::fromRaw(1),
        {NodeId::fromRaw(2), std::string(kSolidSourceOutputPort)},
        NodeInputRef{NodeId::fromRaw(3), std::string(kLayerOutputContentInputPort)}};
    const EdgeRecord layerToStackEdge{
        EdgeId::fromRaw(2),
        {NodeId::fromRaw(3), std::string(kLayerOutputOutputPort)},
        LayerStackInputRef{NodeId::fromRaw(1), LayerSlotId::fromRaw(1),
                           std::string(kLayerStackContentInputRole)}};
    const bool edgesAdded =
        graph.addEdge(stackToOutputEdge) && graph.addEdge(solidToLayerEdge) &&
        graph.addEdge(layerToStackEdge);
    const bool boundariesAdded =
        graph.addLayerOutput({NodeId::fromRaw(3), LayerId::fromRaw(1), "Hero Plate",
                              std::string(kLayerOutputOutputPort)});
    expectations.expect(nodesAdded && edgesAdded && boundariesAdded &&
                            graph.layerStack().append(
                                {LayerSlotId::fromRaw(1), LayerId::fromRaw(1)}),
                        "the composed fixture graph accepts its deliberately scrambled parts");

    graph.setCompositionOutput({NodeId::fromRaw(4), std::string(kCompositionOutputOutputPort)});
    Composition composition{CompositionId::fromRaw(1), "Hero Shot", *duration, std::move(graph),
                            *format};
    expectations.expect(
        composition.parameters().insert(
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
    curve.keyframes.push_back(
        {KeyframeId::fromRaw(22), RationalTime::fromInteger(48), 1.0,
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
    expectRequestError(expectations, requestTooSmallSort, {},
                       CanonicalDocumentError::SortBufferTooSmall,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
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
    const ExtensionRecord markerRecord{ExtensionRecordId::fromRaw(5),
                                       "vendor.module",
                                       "vendor.module.marker",
                                       {1, 0},
                                       CompositionId::fromRaw(1),
                                       "application/octet-stream",
                                       NoExtensionReferences{},
                                       OpaqueExtensionPayload{std::byte{0x00}}};
    const ExtensionRecord remapperRecord{
        ExtensionRecordId::fromRaw(2),
        "vendor.module",
        "vendor.module.record-type",
        {1, 0},
        std::nullopt,
        "text/x-vendor-note",
        ExtensionOwnerRemapper{"vendor.module.record-remapper", {1, 0}},
        OpaqueExtensionPayload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                               std::byte{0xEF}}};
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
    expectRequestError(expectations, requestNoPayloadBuffer, {},
                       CanonicalDocumentError::PayloadBufferTooSmall,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
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
    mismatchedPortability.ocioConfig.portability =
        bloom::document::OcioConfigPortability::External;
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
    expectRequestError(expectations, uriRequest, {},
                       CanonicalDocumentError::InvalidColorSettings, noIndex, noIndex,
                       "the only writable builtin locator is the qualified Bloom Neutral URI");

    auto unsortedContext = neutralColorSettings();
    unsortedContext.ocioConfig.contextVariables = {
        {"BETA", "1"}, {"ALPHA", "2"},
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
    expectations.expect(
        composition->parameters().insert(
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
    expectRequestError(expectations, request, raisedCeilings,
                       CanonicalDocumentError::InvalidLimits,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "caller budgets cannot raise the accepted v1 ceilings");

    CanonicalDocumentLimits starvedValues;
    starvedValues.maximumValues = 10;
    expectRequestError(expectations, request, starvedValues,
                       CanonicalDocumentError::ValueCountExceeded,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "a lower value budget rejects during preflight accounting");

    std::array<char, 64> starvedOutput{};
    starvedOutput.fill('?');
    const auto starvedEncode =
        bloom::project::encodeCanonicalDocument(request, starvedOutput, starvedValues);
    expectations.expect(!starvedEncode &&
                            starvedEncode.error() == CanonicalDocumentError::ValueCountExceeded &&
                            starvedEncode.bytesWritten() == 0 &&
                            starvedOutput.front() == '?' && starvedOutput.back() == '?',
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
    expectRequestError(expectations, request, oneByteShort,
                       CanonicalDocumentError::DocumentSizeExceeded,
                       bloom::project::kCanonicalDocumentNoIndex,
                       bloom::project::kCanonicalDocumentNoIndex,
                       "the exact canonical byte count is checked against the output limit");

    CanonicalDocumentLimits atLimit;
    atLimit.maximumOutputBytes = *exact.value();
    expectations.expect(bloom::project::canonicalDocumentSize(request, atLimit).hasValue(),
                        "an output limit equal to the exact canonical size succeeds");

    std::vector<char> shortOutput(*exact.value() - 1, '?');
    const auto shortResult = bloom::project::encodeCanonicalDocument(request, shortOutput);
    expectations.expect(!shortResult &&
                            shortResult.error() ==
                                CanonicalDocumentError::OutputCapacityExceeded &&
                            shortResult.requiredSize().has_value() &&
                            *shortResult.requiredSize() == *exact.value() &&
                            shortResult.bytesWritten() == 0 &&
                            shortOutput.front() == '?' && shortOutput.back() == '?',
                        "capacity shortage reports the exact requirement and writes nothing");

    std::vector<char> exactOutput(*exact.value(), '?');
    const auto exactResult = bloom::project::encodeCanonicalDocument(request, exactOutput);
    expectations.expect(exactResult && exactResult.bytesWritten() == *exact.value() &&
                            exactOutput.back() == '\n',
                        "exact capacity emits the complete document ending in one LF");
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
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
