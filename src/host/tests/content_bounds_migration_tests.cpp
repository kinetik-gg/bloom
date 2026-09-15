#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace bloom;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::shared_ptr<const runtime::ProcessFrame>
evaluate(const document::Snapshot& snapshot, const core::RationalTime time = {},
         runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{}) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto composition = snapshot.project().compositions().front().id();
    const auto compiled = compiler.compile({snapshot, composition}, {});
    if (!compiled.plan) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << diagnostic.summary << '\n';
        throw std::runtime_error("fixture compiles");
    }
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result =
        evaluator.evaluate(compiled.plan,
                           {.time = time,
                            .output = compiled.plan->output(),
                            .resolution = resolution,
                            .quality = runtime::EvaluationQuality::Reference,
                            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                            .pixelStorageByteLimit = 16U << 20U},
                           {});
    if (!result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << diagnostic.summary << '\n';
        throw std::runtime_error("fixture evaluates");
    }
    return result.frame();
}
std::string encode(const document::Snapshot& snapshot) {
    const auto color = document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    std::array<char, 1024> payload{};
    std::array<std::size_t, 2048> sort{};
    const project::CanonicalDocumentV1 request{&snapshot, &color, payload, sort};
    const auto size = project::canonicalDocumentSize(request);
    require(size.hasValue(), "fixture has canonical size");
    std::string bytes(*size.value(), '\0');
    require(static_cast<bool>(project::encodeCanonicalDocument(request, bytes)), "fixture encodes");
    return bytes;
}
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create(64U << 20U);
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator.value().createOperation(64U << 20U, 64U << 20U);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(operation.value());
}
std::unique_ptr<document::Document> decode(const project::JsonValue& root) {
    auto decoded = project::decodeDocumentEnvelope(root);
    if (!decoded.value()) {
        std::cerr << "decode error " << static_cast<int>(decoded.error()) << ' ' << decoded.path()
                  << '\n';
        throw std::runtime_error("fixture decodes");
    }
    auto restored = project::reconstructDocument(*decoded.value());
    require(static_cast<bool>(restored), "fixture reconstructs");
    return std::move(restored.value()->document);
}
void comparePixels(const runtime::ProcessFrame& before, const runtime::ProcessFrame& after) {
    const auto window = before.processImage().descriptor()->dataWindow();
    require(window == after.processImage().descriptor()->dataWindow(), "same frame dimensions");
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            const auto a = before.processImage().read(x, y);
            const auto b = after.processImage().read(x, y);
            require(a && b, "pixel reads");
            require(std::bit_cast<std::array<std::uint32_t, 4>>(*a.value()) ==
                        std::bit_cast<std::array<std::uint32_t, 4>>(*b.value()),
                    "migration preserves every RGBA32F bit");
        }
}
void migrationProof() {
    const auto format = document::CompositionFormat::create(41, 47);
    if (!format)
        throw std::runtime_error("format");
    auto initial = document::makeNewProject("Bounds migration", "Off centre",
                                            core::RationalTime::fromInteger(10), format.value());
    const auto compositionId = initial.initialCompositionId;
    document::Document doc(std::move(initial.project));
    const auto base = doc.snapshot();
    auto draft = doc.draft(base);
    const auto solid =
        commands::AddSolidLayer(compositionId, "Solid", {0.2, 0.4, 0.8, 0.8}, {27.25, 19.5})
            .apply(draft);
    const auto text =
        commands::AddTextLayer(compositionId, "Clipped text", "jBounds\n", {24.5, 20.25}, 0.7, 25.0)
            .apply(draft);
    require(solid.status == commands::OperationStatus::Applied &&
                text.status == commands::OperationStatus::Applied,
            "legacy fixture sources");
    auto* composition = draft.project().findComposition(compositionId);
    std::vector<document::NodeId> ids;
    for (const auto& node : composition->graph().nodes())
        ids.push_back(node.id);
    for (const auto id : ids) {
        auto* node = composition->graph().findNode(id);
        if (node->typeId == document::kSolidSourceNodeType ||
            node->typeId == document::kTextSourceNodeType) {
            node->schemaVersion = 1;
            std::erase_if(node->parameters, [&](const auto& binding) {
                const bool keep =
                    binding.role == "color" || binding.role == "text" || binding.role == "size";
                if (!keep)
                    require(composition->parameters().erase(binding.parameterId),
                            "remove post-1.6 parameter");
                return !keep;
            });
        } else if (node->typeId == document::kLayerOutputNodeType) {
            node->schemaVersion = 3;
            for (const auto& binding : node->parameters) {
                if (binding.role == "anchor") {
                    const auto curve = draft.ids().allocateAnimationCurve();
                    const auto first = draft.ids().allocateKeyframe();
                    const auto last = draft.ids().allocateKeyframe();
                    if (!curve || !first || !last)
                        throw std::runtime_error("animated pivot IDs");
                    require(composition->animationCurves().insert(document::Vec2AnimationCurve{
                                curve.value(),
                                {{first.value(),
                                  {},
                                  {7.25, -4.5},
                                  document::KeyframeInterpolation::EaseInOut},
                                 {last.value(),
                                  core::RationalTime::fromInteger(1),
                                  {-2.5, 9.75},
                                  document::KeyframeInterpolation::Linear}}}),
                            "animated pivot curve");
                    require(composition->parameters().setSource(
                                binding.parameterId, document::AnimationCurveSource{curve.value()}),
                            "off-centre animated pivot");
                }
                if (binding.role == "scale")
                    require(composition->parameters().setSource(
                                binding.parameterId,
                                document::ConstantValueSource{document::Vec2d{0.8, 1.25}}),
                            "nonuniform scale");
                if (binding.role == "rotation")
                    require(composition->parameters().setSource(
                                binding.parameterId, document::ConstantValueSource{27.0}),
                            "rotation");
            }
        } else if (node->typeId == document::kLayerStackNodeType)
            node->schemaVersion = 1;
    }
    require(doc.commit(base.revision(), std::move(draft)).status ==
                document::CommitStatus::Committed,
            "legacy fixture commits");
    auto bytes = encode(doc.snapshot());
    const auto version = bytes.find("\"minor\": 9");
    require(version != std::string::npos, "root version pinned");
    bytes.replace(version, std::string_view("\"minor\": 9").size(), "\"minor\": 6");
    auto operation = memory();
    const auto dom = project::parseStrictJsonDom(std::as_bytes(std::span(bytes)), {}, operation);
    require(static_cast<bool>(dom), "1.6 DOM parses");
    auto beforeDocument = decode(dom.document()->root());
    const auto before = evaluate(beforeDocument->snapshot());
    const auto migrated =
        project::migrateDocumentDom(dom.document()->root(), {1, 6}, {1, 7},
                                    project::kProductionDocumentMigrationSteps, {}, operation);
    require(migrated.outcome() == project::MigrationOutcome::Migrated &&
                migrated.stepsApplied() == 1,
            "exact 1.6 to 1.7 step");
    auto afterDocument = decode(*migrated.migratedRoot());
    const auto after = evaluate(afterDocument->snapshot());
    comparePixels(*before, *after);
    const auto saved = encode(afterDocument->snapshot());
    const auto reopenedDom =
        project::parseStrictJsonDom(std::as_bytes(std::span(saved)), {}, memory());
    require(static_cast<bool>(reopenedDom), "1.7 round trip parses");
    auto reopened = decode(reopenedDom.document()->root());
    comparePixels(*before, *evaluate(reopened->snapshot()));
    const auto half = core::RationalTime::create(1, 2);
    if (!half)
        throw std::runtime_error("half time");
    for (const auto time : {core::RationalTime::fromInteger(1), *half}) {
        comparePixels(*evaluate(beforeDocument->snapshot(), time),
                      *evaluate(afterDocument->snapshot(), time));
        comparePixels(*evaluate(beforeDocument->snapshot(), time),
                      *evaluate(reopened->snapshot(), time));
    }
    const auto afterSnapshot = afterDocument->snapshot();
    const auto& migratedComposition = afterSnapshot.project().compositions().front();
    std::size_t dimensions = 0;
    for (const auto& parameter : migratedComposition.parameters().records()) {
        if (parameter.schemaKey == document::kSolidWidthParameterSchemaKey ||
            parameter.schemaKey == document::kSolidHeightParameterSchemaKey) {
            const auto value =
                std::get<double>(std::get<document::ConstantValueSource>(parameter.source).value);
            require(
                value ==
                    (parameter.schemaKey == document::kSolidWidthParameterSchemaKey ? 41.0 : 47.0),
                "migrated dimension equals composition size");
            ++dimensions;
        }
    }
    require(dimensions == 2, "both solid dimensions migrated");
    const auto proxy = render::ImageExtent::create(7, 3);
    if (!proxy)
        throw std::runtime_error("proxy extent");
    for (const auto time : {core::RationalTime{}, *half, core::RationalTime::fromInteger(1)}) {
        const runtime::EvaluationResolution resolution = runtime::ProxyResolution{*proxy.value()};
        const auto oldProxy = evaluate(beforeDocument->snapshot(), time, resolution);
        comparePixels(*oldProxy, *evaluate(afterDocument->snapshot(), time, resolution));
        comparePixels(*oldProxy, *evaluate(reopened->snapshot(), time, resolution));
    }
    std::cout << "1.6 -> 1.7 -> saved/reopened 1.7: every pixel bit identical at full and 7x3 "
                 "proxy resolution; off-centre "
                 "scaled/rotated solid and clipped text\n";
}
} // namespace
int main() {
    try {
        migrationProof();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
