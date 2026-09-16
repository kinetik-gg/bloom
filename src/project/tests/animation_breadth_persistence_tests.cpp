#include <regex>
// Animation breadth on the wire (document 1.3): the colour curve kind, the ease-in-out keyframe
// interpolation token, and the 1.2 -> 1.3 migration that changes nothing but the version. The 1.2
// fixture is produced by downgrading a freshly written 1.3 document rather than by pasting a second
// hand-written document literal, so the two versions cannot drift apart here -- the same discipline
// node_group_persistence_tests.cpp already follows for 1.1 -> 1.2.

#include "zip_container_test_support.hpp"

#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
void removeImageFields(std::string& text) {
    text = std::regex_replace(text, std::regex(R"(,\s*"backgroundColor"\s*:\s*\[[^\]]*\])"), "");
    text = std::regex_replace(text, std::regex(R"(,\s*"assets"\s*:\s*\[\])"), "");
    text = std::regex_replace(text, std::regex(R"(,\s*"asset"\s*:\s*"0")"), "");
}

using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

ProjectIoOperationMemory memory(const std::uint64_t budget = 64U << 20U) {
    auto coordinator = ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator) {
        throw std::runtime_error("memory coordinator");
    }
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation) {
        throw std::runtime_error("memory operation");
    }
    return std::move(*operation);
}

document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes(bytes));
}

// The one emitted handle object of that name, with canonical indentation squeezed out, so the
// assertion pins the SHAPE and the exact numbers rather than the writer's current nesting depth.
std::string handleObject(const std::string& text, const std::string_view name) {
    const auto at = text.find('"' + std::string(name) + "\": {");
    if (at == std::string::npos) {
        return {};
    }
    const auto open = text.find('{', at);
    const auto close = text.find('}', open);
    if (open == std::string::npos || close == std::string::npos) {
        return {};
    }
    std::string compact;
    for (const char character : text.substr(open, close + 1 - open)) {
        if (character != ' ' && character != '\n') {
            compact.push_back(character);
        }
    }
    return compact;
}

std::size_t occurrences(const std::string& text, const std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

core::RationalTime time(const std::int64_t numerator, const std::int64_t denominator) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::runtime_error("fixture time");
    }
    return *value;
}

struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
    document::AnimationCurveId colorCurve;
    document::AnimationCurveId scalarCurve;
};

// A project whose one composition carries BOTH of 1.3's additions at once: a colour curve, and an
// ease-in-out interior key on a scalar curve. The channel values deliberately include a negative
// one, an HDR one, and an exact subframe rational time, because those are the three things a lossy
// encoder would quietly round.
Authored authoredProject() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    auto owned = std::make_unique<document::Document>(std::move(initial.project));
    const auto snapshot = owned->snapshot();
    auto draft = owned->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);

    // makeNewProject() builds an empty composition (a Layer Stack and a Composition Output), so the
    // two curve targets are authored here: a Solid source node carrying a colour parameter, and a
    // Layer Output carrying the five transform parameters of which opacity is one. Built directly
    // in the draft rather than through the command layer, which src/project's tests do not link.
    const auto solidNodeId = draft.ids().allocateNode();
    const auto layerNodeId = draft.ids().allocateNode();
    const auto layerId = draft.ids().allocateLayer();
    const auto slotId = draft.ids().allocateLayerSlot();
    const auto solidEdgeId = draft.ids().allocateEdge();
    const auto stackEdgeId = draft.ids().allocateEdge();
    const auto colorParameter = draft.ids().allocateParameter();
    const auto widthParameter = draft.ids().allocateParameter();
    const auto heightParameter = draft.ids().allocateParameter();
    if (!widthParameter || !heightParameter)
        throw std::runtime_error("dimension ids");
    const auto positionParameter = draft.ids().allocateParameter();
    const auto anchorParameter = draft.ids().allocateParameter();
    const auto scaleParameter = draft.ids().allocateParameter();
    const auto rotationParameter = draft.ids().allocateParameter();
    const auto opacityParameter = draft.ids().allocateParameter();
    const auto blendModeParameter = draft.ids().allocateParameter();
    if (!solidNodeId || !layerNodeId || !layerId || !slotId || !solidEdgeId || !stackEdgeId ||
        !colorParameter || !positionParameter || !anchorParameter || !scaleParameter ||
        !rotationParameter || !opacityParameter || !blendModeParameter) {
        throw std::runtime_error("fixture ids");
    }
    auto& graph = composition.graph();
    const bool authoredTopology =
        graph.addNode({*solidNodeId,
                       std::string(document::kSolidSourceNodeType),
                       {{std::string(document::kSolidColorParameterRole), *colorParameter},
                        {std::string(document::kSolidWidthParameterRole), *widthParameter},
                        {std::string(document::kSolidHeightParameterRole), *heightParameter}},
                       document::kSolidSourceNodeSchemaVersion}) &&
        graph.addNode({*layerNodeId,
                       std::string(document::kLayerOutputNodeType),
                       {{std::string(document::kPositionParameterRole), *positionParameter},
                        {std::string(document::kAnchorParameterRole), *anchorParameter},
                        {std::string(document::kScaleParameterRole), *scaleParameter},
                        {std::string(document::kRotationParameterRole), *rotationParameter},
                        {std::string(document::kOpacityParameterRole), *opacityParameter},
                        {std::string(document::kBlendModeParameterRole), *blendModeParameter}},
                       document::kLayerOutputNodeSchemaVersion}) &&
        graph.addLayerOutput(
            {*layerNodeId, *layerId, "Solid 1", std::string(document::kLayerOutputOutputPort)}) &&
        graph.layerStack().append({*slotId, *layerId}) &&
        graph.addEdge({*solidEdgeId,
                       {*solidNodeId, std::string(document::kSolidSourceOutputPort)},
                       document::NodeInputRef{
                           *layerNodeId, std::string(document::kLayerOutputContentInputPort)}}) &&
        graph.addEdge(
            {*stackEdgeId,
             {*layerNodeId, std::string(document::kLayerOutputOutputPort)},
             document::LayerStackInputRef{graph.layerStack().nodeId(), *slotId,
                                          std::string(document::kLayerStackContentInputRole)}});
    if (!authoredTopology) {
        throw std::runtime_error("fixture topology");
    }
    const bool authoredParameters =
        composition.parameters().insert(
            {*widthParameter, std::string(document::kSolidWidthParameterSchemaKey),
             document::ConstantValueSource{static_cast<double>(composition.format().width())}}) &&
        composition.parameters().insert(
            {*heightParameter, std::string(document::kSolidHeightParameterSchemaKey),
             document::ConstantValueSource{static_cast<double>(composition.format().height())}}) &&
        composition.parameters().insert(
            {*colorParameter, std::string(document::kSolidColorParameterSchemaKey),
             document::ConstantValueSource{core::Color4d{1.0, 1.0, 1.0, 1.0}}}) &&
        composition.parameters().insert({*positionParameter,
                                         std::string(document::kPositionParameterSchemaKey),
                                         document::ConstantValueSource{document::Vec2d{}}}) &&
        composition.parameters().insert(
            {*anchorParameter, std::string(document::kAnchorParameterSchemaKey),
             document::ConstantValueSource{document::kDefaultAnchor}}) &&
        composition.parameters().insert({*scaleParameter,
                                         std::string(document::kScaleParameterSchemaKey),
                                         document::ConstantValueSource{document::kDefaultScale}}) &&
        composition.parameters().insert(
            {*rotationParameter, std::string(document::kRotationParameterSchemaKey),
             document::ConstantValueSource{document::kDefaultRotationDegrees}}) &&
        composition.parameters().insert({*opacityParameter,
                                         std::string(document::kOpacityParameterSchemaKey),
                                         document::ConstantValueSource{1.0}}) &&
        composition.parameters().insert(
            {*blendModeParameter, std::string(document::kBlendModeParameterSchemaKey),
             document::ConstantValueSource{document::kDefaultBlendModeValue}});
    if (!authoredParameters) {
        throw std::runtime_error("fixture parameters");
    }

    const auto colorCurve = draft.ids().allocateAnimationCurve();
    const auto scalarCurve = draft.ids().allocateAnimationCurve();
    std::array<document::KeyframeId, 8> colorKeys{};
    for (auto& key : colorKeys) {
        const auto allocated = draft.ids().allocateKeyframe();
        if (!allocated) {
            throw std::runtime_error("fixture colour key ids");
        }
        key = *allocated;
    }
    const auto thirdKey = draft.ids().allocateKeyframe();
    const auto fourthKey = draft.ids().allocateKeyframe();
    if (!colorCurve || !scalarCurve || !thirdKey || !fourthKey) {
        throw std::runtime_error("fixture curve ids");
    }

    std::array<document::ComponentAnimationCurve, 4> colorComponents{
        document::ComponentAnimationCurve{
            {document::ScalarKeyframe{colorKeys[0], time(0, 1), -0.25,
                                      document::KeyframeInterpolation::EaseInOut},
             document::ScalarKeyframe{colorKeys[1], time(7, 24), 1.0}}},
        document::ComponentAnimationCurve{
            {document::ScalarKeyframe{colorKeys[2], time(0, 1), 2.5,
                                      document::KeyframeInterpolation::EaseInOut},
             document::ScalarKeyframe{colorKeys[3], time(7, 24), 0.0}}},
        document::ComponentAnimationCurve{
            {document::ScalarKeyframe{colorKeys[4], time(0, 1), 0.125,
                                      document::KeyframeInterpolation::EaseInOut},
             document::ScalarKeyframe{colorKeys[5], time(7, 24), 0.5}}},
        document::ComponentAnimationCurve{
            {document::ScalarKeyframe{colorKeys[6], time(0, 1), 0.0,
                                      document::KeyframeInterpolation::EaseInOut},
             document::ScalarKeyframe{colorKeys[7], time(7, 24), 1.0}}}};
    if (!composition.animationCurves().insert(
            document::Color4AnimationCurve{*colorCurve, {}, std::move(colorComponents)})) {
        throw std::runtime_error("fixture colour curve");
    }
    // GRAPH-1, D2: the eased scalar segment also carries NON-DEFAULT ease handles on both of its
    // ends, while every colour key keeps its defaults. That pairing is the whole point of the
    // optional encoding: the handled keys must gain the two members, and the default ones must gain
    // nothing at all.
    document::ScalarKeyframe handledStart{*thirdKey, time(0, 1), 0.25,
                                          document::KeyframeInterpolation::EaseInOut};
    handledStart.outgoingHandle = {0.125, 0.5};
    document::ScalarKeyframe handledEnd{*fourthKey, time(5, 3), 1.0};
    handledEnd.incomingHandle = {0.875, -0.25};
    if (!composition.animationCurves().insert(
            document::ScalarAnimationCurve{*scalarCurve, {handledStart, handledEnd}})) {
        throw std::runtime_error("fixture scalar curve");
    }
    if (!composition.parameters().setSource(*colorParameter,
                                            document::AnimationCurveSource{*colorCurve}) ||
        !composition.parameters().setSource(*opacityParameter,
                                            document::AnimationCurveSource{*scalarCurve})) {
        throw std::runtime_error("fixture sources");
    }
    if (!owned->commit(snapshot.revision(), std::move(draft)).committed()) {
        throw std::runtime_error("fixture commit");
    }
    return {std::move(owned), initial.initialCompositionId, *colorCurve, *scalarCurve};
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive()) {
        throw std::runtime_error("animation archive");
    }
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::string documentTextOf(const std::vector<std::byte>& archive) {
    auto entries = readZipContainer(archive, {}, memory());
    if (!entries) {
        throw std::runtime_error("animation entries");
    }
    const auto bytes = entries.document()->documentBytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void roundTripAndReopen() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    const auto snapshot = authored.document->snapshot();
    const auto* original = snapshot.project().findComposition(authored.compositionId);
    expect(original != nullptr, "the fixture composition exists");
    if (original == nullptr) {
        return;
    }

    const auto archive = archiveOf(snapshot, settings);
    const auto text = documentTextOf(archive);
    expect(text.find("\"kind\": \"color4\"") != std::string::npos,
           "a colour curve is written with the color4 discriminator");
    expect(text.find("\"ease-in-out\"") != std::string::npos,
           "and an eased key is written with the ease-in-out token");
    expect(text.find("\"red\"") != std::string::npos && text.find("\"alpha\"") != std::string::npos,
           "a colour key's value carries the same named channels a constant colour does");
    expect(text.find("\"minor\": 13") != std::string::npos,
           "both constructs declare the current document schema minor");
    expect(handleObject(text, "outgoingHandle") == R"({"time":0.125,"value":0.5})" &&
               handleObject(text, "incomingHandle") == R"({"time":0.875,"value":-0.25})",
           "a non-default ease handle is written as a closed {time, value} object");
    expect(occurrences(text, "\"outgoingHandle\"") == 1 &&
               occurrences(text, "\"incomingHandle\"") == 1,
           "and a default handle is written nowhere: ten default-handled keys add no member");

    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened,
           "a project with a colour curve and an eased key opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened) {
        return;
    }
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.schemaMinor == kCanonicalDocumentSchemaVersionV1.minor && !opened.roundTrip,
           "and is read as the current schema minor with nothing unknown to retain");
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored.compositionId);
    expect(composition != nullptr, "the reopened project keeps its composition");
    if (composition == nullptr) {
        return;
    }

    const auto* colorCurve = composition->animationCurves().findColor4(authored.colorCurve);
    const auto* expectedColor = original->animationCurves().findColor4(authored.colorCurve);
    expect(colorCurve != nullptr && expectedColor != nullptr && *colorCurve == *expectedColor,
           "every colour key's id, exact time, four channels and interpolation round-trip exactly, "
           "negative and HDR channels included");
    const auto* scalarCurve = composition->animationCurves().findScalar(authored.scalarCurve);
    const auto* expectedScalar = original->animationCurves().findScalar(authored.scalarCurve);
    expect(scalarCurve != nullptr && expectedScalar != nullptr && *scalarCurve == *expectedScalar,
           "and the eased scalar key survives with its exact subframe rational time");
    expect(scalarCurve != nullptr && scalarCurve->keyframes.front().outgoingInterpolation ==
                                         document::KeyframeInterpolation::EaseInOut,
           "the decoded interior key really is Ease In-Out, not normalized to Linear");
    expect(scalarCurve != nullptr &&
               scalarCurve->keyframes.front().outgoingHandle ==
                   document::KeyframeHandle{0.125, 0.5} &&
               scalarCurve->keyframes.back().incomingHandle ==
                   document::KeyframeHandle{0.875, -0.25} &&
               document::isDefaultKeyframeHandle(scalarCurve->keyframes.front().incomingHandle),
           "and its ease handles decode to the exact authored pair, the unwritten one defaulted");

    // Byte determinism: the second write of the reopened document must be the first write again.
    expect(archiveOf(reopened, opened.colorSettings) == archive,
           "a reopened animated document re-saves byte-identically");
}

// The 1.2 fixture starts from a project with NO curve at all, because a 1.2 file can hold neither
// of 1.3's additions: downgrading is then purely the version rewrite the migration will undo.
std::vector<std::byte> legacyArchive(std::string& documentText) {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    const document::Document unanimated(std::move(initial.project));
    const auto settings = neutralColorSettings();
    const auto snapshot = unanimated.snapshot();
    documentText = documentTextOf(archiveOf(snapshot, settings));

    const auto minor = documentText.find("\"minor\": 13");
    if (minor == std::string::npos) {
        throw std::logic_error("legacy minor anchor");
    }
    documentText.replace(minor, std::string_view("\"minor\": 13").size(), "\"minor\": 2");

    removeImageFields(documentText);
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 2}};
    const auto size = canonicalManifestSize(manifest);
    if (!size) {
        throw std::logic_error("legacy manifest size");
    }
    std::vector<char> encoded(*size.value());
    if (!encodeCanonicalManifest(manifest, encoded)) {
        throw std::logic_error("legacy manifest encode");
    }
    return test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes({encoded.data(), encoded.size()})),
        test::makeStoredEntry("document.json", test::toBytes(documentText)));
}

void migrationFromTwelve() {
    std::string documentText;
    const auto archive = legacyArchive(documentText);
    const auto result = openProjectArchive(archive, {}, memory());
    const auto* failure = result.failure()
                              ? result.failure()->payloadAs<SaveArchiveDocumentDecodeFailure>()
                              : nullptr;
    expect(result.outcome() == OpenArchiveOutcome::Failed && failure &&
               failure->error == DocumentDecodeError::UnsupportedSchemaVersion,
           "documents below the canonical 1.12 floor are refused without migration");
}

// The additive-minor rule, in both directions: a file claiming a minor that PREDATES 1.3 may not
// carry a 1.3 construct, while one claiming a later minor may.
void minorGating() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    const auto baseline = documentTextOf(archiveOf(authored.document->snapshot(), settings));
    const auto anchor = std::string_view("\"minor\": 13");
    const auto minor = baseline.find(anchor);
    expect(minor != std::string::npos, "the animated fixture declares the current minor");
    if (minor == std::string::npos) {
        return;
    }

    // Claiming 1.2 -- a minor this build KNOWS, and one that does not declare the colour curve
    // kind. There is no round-trip state for a known minor (that exists only for a minor newer than
    // this build), so the unknown discriminator is a hard decode failure naming the exact member,
    // which is the only honest answer: the file says it is 1.2 and 1.2 has no such construct.
    {
        auto text = baseline;
        text.replace(minor, anchor.size(), "\"minor\": 2");
        removeImageFields(text);
        auto dom = parseStrictJsonDom(test::toBytes(text), {}, memory());
        expect(static_cast<bool>(dom), "the 1.2-labelled document parses");
        if (!dom) {
            return;
        }
        const auto decoded = decodeDocumentEnvelope(dom.document()->root());
        expect(decoded.outcome() == DocumentDecodeOutcome::Failed &&
                   decoded.error() == DocumentDecodeError::UnsupportedSchemaVersion,
               "a document claiming 1.2 while carrying a colour curve is refused, never decoded as "
               "if the construct were part of 1.2");
        expect(decoded.path() == "/schemaVersion",
               "and the refusal names the exact discriminator it could not accept");
    }

    // Claiming 1.13 -- a minor NEWER than this build -- still decodes the colour curve, because the
    // gate is "the minor that declares it or later", not "exactly 1.3". That is what makes 1.3's
    // additions additive rather than a one-version island.
    {
        auto text = baseline;
        text.replace(minor, anchor.size(), "\"minor\": 14");
        auto dom = parseStrictJsonDom(test::toBytes(text), {}, memory());
        expect(static_cast<bool>(dom), "the 1.4-labelled document parses");
        if (!dom) {
            return;
        }
        const auto decoded = decodeDocumentEnvelope(dom.document()->root());
        expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                   decoded.classification() == DocumentClassification::EditableWithRoundTrip,
               "a document claiming a newer minor still decodes its colour curve and eased key");
    }
}

} // namespace

int main() {
    try {
        roundTripAndReopen();
        migrationFromTwelve();
        minorGating();
    } catch (const std::exception& error) {
        std::cerr << "animation breadth persistence fixture failure: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
