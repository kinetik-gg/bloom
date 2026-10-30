#include "zip_container_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_time.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <iostream>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// Document 1.29 (ADR 0033) members a downgraded fixture must not carry.
[[maybe_unused]] void stripExportQueueV1_29(std::string& text) {
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportQueue"\s*:\s*\{\s*"jobs"\s*:\s*\[\]\s*\})"), "");
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportJob"\s*:\s*"0",\s*"exportOutput"\s*:\s*"0")"), "");
}
using namespace bloom;
using namespace bloom::project;
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

constexpr auto kLayer = LayerId::fromRaw(1);

Project makeProject(const LayerTiming& timing) {
    CanonicalGraph graph{NodeId::fromRaw(1)};
    const bool nodes = graph.addNode({NodeId::fromRaw(3),
                                      std::string(kLayerOutputNodeType),
                                      {{"anchor", ParameterId::fromRaw(8)},
                                       {"blendMode", ParameterId::fromRaw(11)},
                                       {"opacity", ParameterId::fromRaw(3)},
                                       {"position", ParameterId::fromRaw(5)},
                                       {"rotation", ParameterId::fromRaw(10)},
                                       {"scale", ParameterId::fromRaw(9)}},
                                      kLayerOutputNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(1),
                                      std::string(kLayerStackNodeType),
                                      {},
                                      kLayerStackNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(4),
                                      std::string(kCompositionOutputNodeType),
                                      {},
                                      kCompositionOutputNodeSchemaVersion}) &&
                       graph.addNode({NodeId::fromRaw(2),
                                      std::string(kSolidSourceNodeType),
                                      {{"color", ParameterId::fromRaw(7)},
                                       {"height", ParameterId::fromRaw(13)},
                                       {"width", ParameterId::fromRaw(12)}},
                                      kSolidSourceNodeSchemaVersion});
    const bool edges =
        graph.addEdge(
            {EdgeId::fromRaw(3),
             {NodeId::fromRaw(1), std::string(kLayerStackOutputPort)},
             NodeInputRef{NodeId::fromRaw(4), std::string(kCompositionOutputInputPort)}}) &&
        graph.addEdge(
            {EdgeId::fromRaw(1),
             {NodeId::fromRaw(2), std::string(kSolidSourceOutputPort)},
             NodeInputRef{NodeId::fromRaw(3), std::string(kLayerOutputContentInputPort)}}) &&
        graph.addEdge({EdgeId::fromRaw(2),
                       {NodeId::fromRaw(3), std::string(kLayerOutputOutputPort)},
                       LayerStackInputRef{NodeId::fromRaw(1), LayerSlotId::fromRaw(1),
                                          std::string(kLayerStackContentInputRole)}});
    LayerOutputBoundary boundary{NodeId::fromRaw(3), kLayer, "Layer",
                                 std::string(kLayerOutputOutputPort)};
    boundary.timing = timing;
    if (!nodes || !edges || !graph.addLayerOutput(boundary) ||
        !graph.layerStack().append({LayerSlotId::fromRaw(1), kLayer}))
        throw std::logic_error("layer time graph");
    graph.setCompositionOutput({NodeId::fromRaw(4), std::string(kCompositionOutputOutputPort)});
    Composition composition{CompositionId::fromRaw(1), "Main", core::RationalTime::fromInteger(10),
                            std::move(graph),
                            *CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(),
                                                       FrameRate::framesPerSecond24())};
    const bool parameters =
        composition.parameters().insert({ParameterId::fromRaw(12),
                                         std::string(kSolidWidthParameterSchemaKey),
                                         ConstantValueSource{1920.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(13),
                                         std::string(kSolidHeightParameterSchemaKey),
                                         ConstantValueSource{1080.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(7),
                                         std::string(kSolidColorParameterSchemaKey),
                                         ConstantValueSource{core::Color4d{0.0, 0.5, 1.0, 1.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(5),
                                         std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{96.0, -48.0}}}) &&
        composition.parameters().insert({ParameterId::fromRaw(3),
                                         std::string(kOpacityParameterSchemaKey),
                                         ConstantValueSource{1.0}}) &&
        composition.parameters().insert({ParameterId::fromRaw(8),
                                         std::string(kAnchorParameterSchemaKey),
                                         ConstantValueSource{kDefaultAnchor}}) &&
        composition.parameters().insert({ParameterId::fromRaw(9),
                                         std::string(kScaleParameterSchemaKey),
                                         ConstantValueSource{kDefaultScale}}) &&
        composition.parameters().insert({ParameterId::fromRaw(10),
                                         std::string(kRotationParameterSchemaKey),
                                         ConstantValueSource{kDefaultRotationDegrees}}) &&
        composition.parameters().insert({ParameterId::fromRaw(11),
                                         std::string(kBlendModeParameterSchemaKey),
                                         ConstantValueSource{kDefaultBlendModeValue}});
    Project project{ProjectId::fromRaw(1), "Layer Time"};
    if (!parameters || !project.addComposition(std::move(composition)))
        throw std::logic_error("layer time project");
    return project;
}

LayerTiming nonDefaultTiming(const bool sourceExtent) {
    LayerTiming timing;
    timing.startTime = *core::RationalTime::create(-5, 24);
    timing.speed = *LayerSpeed::create(3, 2);
    if (sourceExtent)
        timing.contentExtent = SourceLayerExtent{NodeId::fromRaw(2)};
    else
        timing.contentExtent =
            FixedLayerExtent{*core::RationalTime::create(48, 24), *FrameRate::create(24000, 1001)};
    timing.beforeMode = LayerTimeMode::Hold;
    timing.afterMode = LayerTimeMode::PingPong;
    return timing;
}

std::string encode(const Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

std::string encodeProject(const LayerTiming& timing) {
    return encode(Document{makeProject(timing)});
}

DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("layer time parse");
    return decodeDocumentEnvelope(parsed.document()->root());
}

std::string replaceRegex(const std::string& text, const char* pattern, const char* replacement) {
    const auto result = std::regex_replace(text, std::regex(pattern), replacement);
    if (result == text)
        throw std::logic_error(std::string("fixture anchor missing: ") + pattern);
    return result;
}

constexpr std::array<std::string_view, 5> kMembers{"startTime", "speed", "contentExtent",
                                                   "beforeMode", "afterMode"};

void identityWritesNoTimingMembers() {
    const auto text = encodeProject({});
    for (const auto member : kMembers)
        expect(text.find(std::string("\"") + std::string(member) + "\"") == std::string::npos,
               "identity layer timing writes no layer time member");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded) &&
               decoded.value()->compositions.front().graph.layerOutputs.front().timing ==
                   LayerTiming{},
           "identity layer timing decodes as identity");
}

void roundTripsEveryMember() {
    for (const bool sourceExtent : {false, true}) {
        const auto timing = nonDefaultTiming(sourceExtent);
        const auto text = encodeProject(timing);
        for (const auto member : kMembers)
            expect(text.find(std::string("\"") + std::string(member) + "\"") != std::string::npos,
                   "non-default layer timing writes every member");
        const auto decoded = decodeText(text);
        expect(static_cast<bool>(decoded) &&
                   decoded.value()->compositions.front().graph.layerOutputs.front().timing ==
                       timing,
               "document decode returns the exact layer timing");

        Document document{makeProject(timing)};
        const auto snapshot = document.snapshot();
        const auto settings = makeBloomNeutralColorSettingsV1({});
        auto archive = buildVerifiedSaveArchive(
            {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
        expect(static_cast<bool>(archive), "layer timing archive saves and verifies");
        if (!archive)
            continue;
        auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
        expect(result.outcome() == OpenArchiveOutcome::Opened, "layer timing archive opens");
        if (result.outcome() != OpenArchiveOutcome::Opened)
            continue;
        auto opened = std::move(result).takeOpened();
        const auto reopened = opened.document->snapshot();
        expect(reopened.project().compositions().front().graph().layerOutputs().front() ==
                   snapshot.project().compositions().front().graph().layerOutputs().front(),
               "layer boundary equals itself after save and open");
    }
}

void olderDocumentMigratesAndResavesWithoutTiming() {
    auto text = encodeProject({});
    text = replaceRegex(text, R"("minor": 29\s*\})", "\"minor\": 22\n  }");
    stripExportQueueV1_29(text);
    const auto typeAt = text.find("\"bloom.layer-output\"");
    const auto versionAt = text.find("\"schemaVersion\": 5", typeAt);
    if (typeAt == std::string::npos || versionAt == std::string::npos)
        throw std::logic_error("layer output node anchor");
    text.replace(versionAt, std::string_view("\"schemaVersion\": 5").size(),
                 "\"schemaVersion\": 4");
    {
        auto operation = memory();
        auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
        expect(static_cast<bool>(parsed), "1.22 layer output fixture parses");
        if (parsed) {
            const auto migrated =
                migrateDocumentDom(parsed.document()->root(), {1, 22}, {1, 23},
                                   kProductionDocumentMigrationSteps, {}, operation);
            expect(migrated.outcome() == MigrationOutcome::Migrated && migrated.stepsApplied() == 1,
                   "1.22 to 1.23 runs one step");
            const auto decoded = migrated.outcome() == MigrationOutcome::Migrated
                                     ? decodeDocumentEnvelope(*migrated.migratedRoot())
                                     : decodeDocumentEnvelope(parsed.document()->root());
            bool upgraded = false;
            if (decoded)
                for (const auto& node : decoded.value()->compositions.front().graph.nodes)
                    if (node.typeId == kLayerOutputNodeType)
                        upgraded = node.schemaVersion == 5 && node.parameters.size() == 6;
            expect(upgraded, "layer output node schema is 5 with identical bindings");
        }
    }
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 22}, .requirements = {}};
    const auto manifestSize = canonicalManifestSize(manifest);
    if (!manifestSize)
        throw std::runtime_error("manifest size");
    std::string manifestText(*manifestSize.value(), '\0');
    if (!encodeCanonicalManifest(manifest, manifestText))
        throw std::runtime_error("manifest encode");
    const auto archive = test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes(manifestText)),
        test::makeStoredEntry("document.json", test::toBytes(text)));
    auto opened = openProjectArchive(archive, {}, memory());
    expect(opened.outcome() == OpenArchiveOutcome::Opened, "1.22 archive migrates on open");
    if (opened.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto result = std::move(opened).takeOpened();
    expect(result.schemaMinor == 22 && !result.roundTrip,
           "1.22 archive opens as an editable document that reports its own minor");
    expect(result.document->snapshot()
                   .project()
                   .compositions()
                   .front()
                   .graph()
                   .layerOutputs()
                   .front()
                   .timing == LayerTiming{},
           "migrated layer has identity timing");
    const auto resaved = encode(*result.document);
    for (const auto member : kMembers)
        expect(resaved.find(std::string("\"") + std::string(member) + "\"") == std::string::npos,
               "re-saved 1.22 document has no layer time member");
    expect(resaved.find("\"minor\": 29") != std::string::npos, "re-save writes the current minor");
}

void rejectsInvalidMembers() {
    const auto fixed = encodeProject(nonDefaultTiming(false));
    const auto speedReplace = [&](const char* numerator, const char* denominator) {
        return replaceRegex(fixed,
                            R"re("speed": \{(\s*)"numerator": "3",(\s*)"denominator": "2")re",
                            (std::string("\"speed\": {$1\"numerator\": \"") + numerator +
                             "\",$2\"denominator\": \"" + denominator + "\"")
                                .c_str());
    };
    expect(!decodeText(speedReplace("0", "1")), "zero speed is rejected");
    expect(!decodeText(speedReplace("-3", "2")), "negative speed is rejected");
    expect(!decodeText(speedReplace("1", "5000")), "speed outside [1/1000, 1000] is rejected");
    expect(!decodeText(replaceRegex(fixed, R"("beforeMode": "hold")", "\"beforeMode\": \"bogus\"")),
           "unknown mode string is rejected");
    expect(!decodeText(replaceRegex(
               fixed, R"re("duration": \{(\s*)"numerator": "2",(\s*)"denominator": "1")re",
               "\"duration\": {$1\"numerator\": \"0\",$2\"denominator\": \"1\"")),
           "fixed extent with zero duration is rejected");
    expect(!decodeText(replaceRegex(fixed, R"("kind": "fixed")", "\"kind\": \"bogus\"")),
           "unknown extent kind is rejected");
    expect(!decodeText(replaceRegex(fixed, R"("minor": 29\s*\})", "\"minor\": 22\n  }")),
           "layer time members are not accepted in a 1.22 document");
}
} // namespace

int main() try {
    identityWritesNoTimingMembers();
    roundTripsEveryMember();
    olderDocumentMigratesAndResavesWithoutTiming();
    rejectsInvalidMembers();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
