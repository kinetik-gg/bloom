// The value graph on the wire (introduced in document 1.4): the vec3 constant kind and the driver
// parameter source, written and read back at the current minor. The gating cases are produced by
// restamping a freshly written document's declared minor rather than by pasting a second
// hand-written literal, so the claimed version and the payload cannot drift apart here.

#include "zip_container_test_support.hpp"

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
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

struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
    document::ParameterId vector3Parameter;
    document::ParameterId drivenOpacity;
    document::NodeId scalarValueNode;
};

// One composition carrying BOTH of 1.4's additions at once: a Vector 3 value node whose constant is
// a vec3, and a Layer Output whose opacity is driven by a Scalar value node. The vec3 components
// deliberately include a negative one and a value with no short decimal spelling, because those are
// what a lossy encoder would quietly round.
Authored authoredProject() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    auto owned = std::make_unique<document::Document>(std::move(initial.project));
    const auto snapshot = owned->snapshot();
    auto draft = owned->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);

    const auto solidNodeId = draft.ids().allocateNode();
    const auto layerNodeId = draft.ids().allocateNode();
    const auto scalarNodeId = draft.ids().allocateNode();
    const auto vector3NodeId = draft.ids().allocateNode();
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
    const auto scalarValueParameter = draft.ids().allocateParameter();
    const auto vector3ValueParameter = draft.ids().allocateParameter();
    if (!solidNodeId || !layerNodeId || !scalarNodeId || !vector3NodeId || !layerId || !slotId ||
        !solidEdgeId || !stackEdgeId || !colorParameter || !positionParameter || !anchorParameter ||
        !scaleParameter || !rotationParameter || !opacityParameter || !blendModeParameter ||
        !scalarValueParameter || !vector3ValueParameter) {
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
        graph.addNode({*scalarNodeId,
                       std::string(document::kScalarValueNodeType),
                       {{std::string(document::kValueParameterRole), *scalarValueParameter}},
                       document::kValueNodeSchemaVersion}) &&
        graph.addNode({*vector3NodeId,
                       std::string(document::kVector3ValueNodeType),
                       {{std::string(document::kValueParameterRole), *vector3ValueParameter}},
                       document::kValueNodeSchemaVersion}) &&
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
        composition.parameters().insert(
            {*blendModeParameter, std::string(document::kBlendModeParameterSchemaKey),
             document::ConstantValueSource{document::kDefaultBlendModeValue}}) &&
        composition.parameters().insert({*scalarValueParameter,
                                         std::string(document::kScalarValueParameterSchemaKey),
                                         document::ConstantValueSource{0.625}}) &&
        composition.parameters().insert(
            {*vector3ValueParameter, std::string(document::kVector3ValueParameterSchemaKey),
             document::ConstantValueSource{document::Vec3d{-12.5, 0.1, 4096.0}}}) &&
        // The driven one: inserted as a driver source from the start, so what round-trips is the
        // binding itself rather than a constant that was later replaced.
        composition.parameters().insert(
            {*opacityParameter, std::string(document::kOpacityParameterSchemaKey),
             document::DriverBindingSource{*scalarNodeId, std::string(document::kValuePortName)}});
    if (!authoredParameters) {
        throw std::runtime_error("fixture parameters");
    }
    if (!draft.validate().ok()) {
        throw std::runtime_error("fixture validation");
    }
    const auto committed = owned->commit(snapshot.revision(), std::move(draft));
    if (!committed.committed()) {
        throw std::runtime_error("fixture publication");
    }
    return {std::move(owned), initial.initialCompositionId, *vector3ValueParameter,
            *opacityParameter, *scalarNodeId};
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive()) {
        throw std::runtime_error("value graph archive");
    }
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::string documentTextOf(const std::vector<std::byte>& archive) {
    auto entries = readZipContainer(archive, {}, memory());
    if (!entries) {
        throw std::runtime_error("value graph entries");
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
    expect(text.find("\"kind\": \"vec3\"") != std::string::npos,
           "a three-component constant is written with the vec3 discriminator");
    expect(text.find("\"z\": -") != std::string::npos ||
               text.find("\"z\": 4096") != std::string::npos,
           "and carries its own third component by name");
    expect(text.find("\"kind\": \"driver\"") != std::string::npos,
           "a driven parameter is written with the driver discriminator");
    expect(text.find("\"outputPort\": \"value\"") != std::string::npos,
           "and names the output port it reads");
    expect(text.find("\"minor\": 20") != std::string::npos,
           "both constructs declare the current document schema minor");

    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened,
           "a project with a vec3 constant and a driver source opens");
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

    const auto* vector3 = composition->parameters().find(authored.vector3Parameter);
    const auto* vector3Constant =
        vector3 == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&vector3->source);
    const auto* vector3Value = vector3Constant == nullptr
                                   ? nullptr
                                   : std::get_if<document::Vec3d>(&vector3Constant->value);
    expect(vector3Value != nullptr && *vector3Value == document::Vec3d{-12.5, 0.1, 4096.0},
           "every vec3 component survives the round trip exactly, negative and HDR included");

    const auto* driven = composition->parameters().find(authored.drivenOpacity);
    const auto* driver =
        driven == nullptr ? nullptr : std::get_if<document::DriverBindingSource>(&driven->source);
    expect(driver != nullptr &&
               *driver == document::DriverBindingSource{authored.scalarValueNode,
                                                        std::string(document::kValuePortName)},
           "a driver binding round-trips as the exact node-and-port pair it addressed");

    // Byte identity, not just value equality: re-encoding the reopened document must reproduce the
    // original archive's document.json exactly, or something in the pair was normalized on the way.
    const auto reencoded = documentTextOf(archiveOf(reopened, opened.colorSettings));
    expect(reencoded == text, "re-encoding the reopened document reproduces identical bytes");
}

// The additive-minor rule in both directions: a file claiming a minor that PREDATES 1.4 may not
// carry a 1.4 construct, while one claiming a later minor may.
void minorGating() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    auto baseline = documentTextOf(archiveOf(authored.document->snapshot(), settings));
    const auto anchor = std::string_view("\"minor\": 20");
    const auto minor = baseline.find(anchor);
    expect(minor != std::string::npos, "the value-graph fixture declares the current minor");
    if (minor == std::string::npos) {
        return;
    }

    const auto decodeClaiming = [&](const std::string_view replacement) {
        auto text = baseline;
        text.replace(minor, anchor.size(), std::string(replacement));
        auto dom = parseStrictJsonDom(test::toBytes(text), {}, memory());
        if (!dom) {
            throw std::runtime_error("gating parse");
        }
        return decodeDocumentEnvelope(dom.document()->root());
    };

    // Claiming 1.3 -- a minor BELOW the 1.12 floor. The document is refused at the floor, before
    // any member of it is interpreted, so a construct 1.3 never declared can no more be read as
    // part of 1.3 than it could be silently migrated forward.
    {
        const auto decoded = decodeClaiming("\"minor\": 3");
        expect(decoded.outcome() == DocumentDecodeOutcome::Failed &&
                   decoded.error() == DocumentDecodeError::UnsupportedSchemaVersion,
               "a document claiming 1.3 while carrying a driver source is refused, never decoded "
               "as if the construct were part of 1.3");
        expect(decoded.path() == "/schemaVersion",
               "and the refusal names the schema version it could not accept");
    }

    // Claiming 1.5 -- a minor NEWER than this build -- still decodes both constructs, because the
    // gate is "the minor that declares it or later", not "exactly 1.4". That is what makes 1.4's
    // additions additive rather than a one-version island.
    {
        const auto decoded = decodeClaiming("\"minor\": 21");
        expect(
            decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                decoded.classification() == DocumentClassification::EditableWithRoundTrip,
            "a document claiming a newer minor still decodes its vec3 constant and driver source");
    }
}

} // namespace

int main() {
    try {
        roundTripAndReopen();
        minorGating();
    } catch (const std::exception& error) {
        std::cerr << "value graph persistence fixture failure: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
