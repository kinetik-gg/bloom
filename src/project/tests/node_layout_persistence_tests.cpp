#include "zip_container_test_support.hpp"

#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

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
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
std::vector<std::byte> currentArchive() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    const document::Document document(std::move(initial.project));
    const auto snapshot = document.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    const auto archive = buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    if (!archive)
        throw std::runtime_error("current layout archive");
    const auto bytes = archive.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}
void layoutAndReopen() {
    const auto archive = currentArchive();
    auto result = openProjectArchive(archive, {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened,
           "production open accepts current layout");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    auto snapshot = opened.document->snapshot();
    const auto& composition = snapshot.project().compositions().front();
    expect(composition.nodeLayout() == document::defaultNodeLayout(composition.graph().nodes()),
           "creation assigns all default records");
    expect(opened.schemaMinor == kCanonicalDocumentSchemaVersionV1.minor && !opened.roundTrip,
           "open preserves current editable schema");
    auto draft = opened.document->draft(snapshot);
    const auto frameTime = core::RationalTime::create(1, 24);
    if (!frameTime)
        throw std::logic_error("frame time fixture");
    auto* editing = draft.project().findComposition(composition.id());
    for (const auto& boundary : composition.graph().layerOutputs()) {
        auto* layer = editing->graph().findLayer(boundary.layerId);
        layer->labelColor = std::array<std::uint8_t, 3>{12, 34, 56};
        layer->enabled = false;
        layer->solo = true;
        layer->locked = true;
        layer->inPoint = *frameTime;
        layer->outPoint = core::RationalTime::fromInteger(1);
    }
    editing->setWorkArea(document::WorkArea{*frameTime, core::RationalTime::fromInteger(1)});
    auto& layout = editing->nodeLayout();
    const auto first = composition.graph().nodes().front().id;
    layout[first] = {{-123.5, 89.25}, 276.5, true, true};
    layout[document::NodeId::fromRaw(9999)] = {{45, 67}, 120, false, false};
    expect(draft.validate().ok() && !draft.validate().issues().empty(),
           "unknown layout warns without rejecting");
    expect(opened.document->commit(snapshot.revision(), std::move(draft)).committed(),
           "layout edit publishes");
    snapshot = opened.document->snapshot();
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &opened.colorSettings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    expect(static_cast<bool>(saved), "layout archive passes close/reopen verification");
    if (!saved || !saved.archive())
        return;
    auto reopenedResult = openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(reopenedResult.outcome() == OpenArchiveOutcome::Opened, "layout archive opens");
    if (reopenedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(reopenedResult).takeOpened();
    const auto reopenedSnapshot = reopened.document->snapshot();
    expect(reopenedSnapshot.project().compositions().front().nodeLayout() ==
               snapshot.project().compositions().front().nodeLayout(),
           "all layout fields and unknown-node records round-trip exactly");
    expect(reopenedSnapshot.project().compositions().front().workArea() ==
               snapshot.project().compositions().front().workArea(),
           "work area survives verified reopen");
    expect(
        std::ranges::equal(reopenedSnapshot.project().compositions().front().graph().layerOutputs(),
                           snapshot.project().compositions().front().graph().layerOutputs()),
        "layer ranges survive verified archive reopen");
    expect(reopenedSnapshot.ids().highWater() == snapshot.ids().highWater(),
           "layout references never allocate semantic IDs");
}
void discreteImageNodesReopen() {
    auto initial =
        document::makeNewProject("Discrete", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(initial.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* composition = draft.project().findComposition(initial.initialCompositionId);
    for (const auto* type : {"bloom.flip", "bloom.rotate-90"}) {
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        if (!definition)
            throw std::logic_error("discrete definition");
        const auto node = *draft.ids().allocateNode();
        std::vector<document::ParameterBinding> parameters;
        for (const auto& parameter : definition->parameters) {
            const auto id = *draft.ids().allocateParameter();
            auto value = parameter.defaultValue;
            if (parameter.role == "quarterTurns")
                value = std::int64_t{-3};
            expect(composition->parameters().insert(
                       {id, parameter.schemaKey, document::ConstantValueSource{value}}),
                   "discrete authored parameter is valid");
            parameters.push_back({parameter.role, id});
        }
        expect(composition->graph().addNode({node, type, std::move(parameters), 1}),
               "discrete node inserts");
        composition->nodeLayout()[node] = {};
    }
    expect(document.commit(snapshot.revision(), std::move(draft)).committed(),
           "discrete graph draft commits");
    snapshot = document.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    const auto saved = buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(saved), "both discrete nodes pass verified archive publication");
    if (!saved)
        return;
    auto reopened = openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(reopened.outcome() == OpenArchiveOutcome::Opened, "discrete archive reopens editable");
    if (reopened.outcome() != OpenArchiveOutcome::Opened)
        return;
    const auto opened = std::move(reopened).takeOpened();
    const auto next = opened.document->snapshot();
    const auto& before = snapshot.project().compositions().front();
    const auto& after = next.project().compositions().front();
    expect(std::ranges::equal(before.graph().nodes(), after.graph().nodes()) &&
               std::ranges::equal(before.parameters().records(), after.parameters().records()),
           "discrete node IDs, signed turns, booleans and schema versions round trip exactly");
}

void blurNodeReopen() {
    auto initial = document::makeNewProject("Blur Project", "Blur Composition",
                                            core::RationalTime::fromInteger(10));
    document::Document owned(std::move(initial.project));
    const auto before = owned.snapshot();
    auto draft = owned.draft(before);
    auto* composition = draft.project().findComposition(initial.initialCompositionId);
    std::vector<document::NodeId> nodes;
    for (const auto* type : {"bloom.gaussian-blur", "bloom.box-blur"}) {
        const auto node = draft.ids().allocateNode();
        if (!node)
            throw std::logic_error("blur node identity");
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        if (!definition)
            throw std::logic_error("blur definition");
        std::vector<document::ParameterBinding> bindings;
        for (const auto& parameter : definition->parameters) {
            const auto id = draft.ids().allocateParameter();
            if (!id)
                throw std::logic_error("blur parameter identity");
            const document::ParameterValue value = parameter.role == "radius"
                                                       ? document::ParameterValue{2.5}
                                                       : document::ParameterValue{std::int64_t{1}};
            if (!composition->parameters().insert(
                    {*id, parameter.schemaKey, document::ConstantValueSource{value}}))
                throw std::logic_error("blur parameter inserts");
            bindings.push_back({parameter.role, *id});
        }
        if (!composition->graph().addNode({*node, type, std::move(bindings), 1}))
            throw std::logic_error("blur node inserts");
        nodes.push_back(*node);
    }
    expect(owned.commit(before.revision(), std::move(draft)).committed(), "blur nodes publish");
    const auto snapshot = owned.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "blur graph passes verified save");
    if (!archive)
        return;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "blur archive reopens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* oldComposition = snapshot.project().findComposition(initial.initialCompositionId);
    const auto* newComposition = reopened.project().findComposition(initial.initialCompositionId);
    for (const auto id : nodes) {
        const auto* oldNode = oldComposition->graph().findNode(id);
        const auto* newNode = newComposition->graph().findNode(id);
        expect(oldNode && newNode, "blur typed nodes survive reopen");
        if (!oldNode || !newNode)
            continue;
        auto canonicalNode = *oldNode;
        // The archive canonicalizes binding order by role; the binding identities and values
        // remain exact even when the registry declares radius before edge.
        std::ranges::sort(canonicalNode.parameters, {}, &document::ParameterBinding::role);
        expect(canonicalNode == *newNode, "blur typed node and bindings survive reopen");
        for (const auto& binding : oldNode->parameters) {
            const auto* oldParameter = oldComposition->parameters().find(binding.parameterId);
            const auto* newParameter = newComposition->parameters().find(binding.parameterId);
            expect(oldParameter && newParameter && oldParameter->source == newParameter->source &&
                       oldParameter->schemaKey == newParameter->schemaKey,
                   "blur radius/edge schema and exact values survive reopen");
        }
    }
}

// #361: Set Alpha and Multiply Alpha with inverted matte selectors (9 Luma Inverted, 7 Green
// Inverted) and no matte connection save, reopen and validate with exact stored values.
void matteNodeReopen() {
    auto initial = document::makeNewProject("Matte Project", "Matte Composition",
                                            core::RationalTime::fromInteger(10));
    document::Document owned(std::move(initial.project));
    const auto before = owned.snapshot();
    auto draft = owned.draft(before);
    auto* composition = draft.project().findComposition(initial.initialCompositionId);
    std::vector<document::NodeId> nodes;
    for (const auto* type : {"bloom.set-alpha", "bloom.multiply-alpha"}) {
        const auto node = draft.ids().allocateNode();
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        if (!node || !definition)
            throw std::logic_error("matte node definition");
        std::vector<document::ParameterBinding> bindings;
        for (const auto& parameter : definition->parameters) {
            const auto id = draft.ids().allocateParameter();
            if (!id)
                throw std::logic_error("matte parameter identity");
            const document::ParameterValue value =
                parameter.role == "factor"
                    ? document::ParameterValue{0.5}
                    : document::ParameterValue{std::int64_t{nodes.empty() ? 9 : 7}};
            if (!composition->parameters().insert(
                    {*id, parameter.schemaKey, document::ConstantValueSource{value}}))
                throw std::logic_error("matte parameter inserts");
            bindings.push_back({parameter.role, *id});
        }
        if (!composition->graph().addNode({*node, type, std::move(bindings), 1}))
            throw std::logic_error("matte node inserts");
        nodes.push_back(*node);
    }
    expect(owned.commit(before.revision(), std::move(draft)).committed(),
           "inverted matte selectors without a matte publish");
    const auto snapshot = owned.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes({}));
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "matte graph passes verified save");
    if (!archive)
        return;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "matte archive reopens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto* oldComposition = snapshot.project().findComposition(initial.initialCompositionId);
    const auto* newComposition = reopened.project().findComposition(initial.initialCompositionId);
    for (const auto id : nodes) {
        const auto* oldNode = oldComposition->graph().findNode(id);
        const auto* newNode = newComposition->graph().findNode(id);
        expect(oldNode && newNode, "matte nodes survive reopen");
        if (!oldNode || !newNode)
            continue;
        for (const auto& binding : oldNode->parameters) {
            const auto* oldParameter = oldComposition->parameters().find(binding.parameterId);
            const auto* newParameter = newComposition->parameters().find(binding.parameterId);
            expect(oldParameter && newParameter && oldParameter->source == newParameter->source,
                   "matte selector and factor values survive reopen exactly");
        }
    }
}

void futureLayoutAttachments() {
    auto baseline = openProjectArchive(currentArchive(), {}, memory());
    if (baseline.outcome() != OpenArchiveOutcome::Opened)
        throw std::logic_error("layout future baseline");
    auto current = std::move(baseline).takeOpened();
    auto snapshot = current.document->snapshot();
    auto saved = buildVerifiedSaveArchive(
        CanonicalManifestV1{},
        CanonicalDocumentV1{.snapshot = &snapshot, .colorSettings = &current.colorSettings}, {},
        memory());
    if (!saved)
        throw std::logic_error("layout future archive");
    auto entries = readZipContainer(saved.archive()->bytes(), {}, memory());
    if (!entries)
        throw std::logic_error("layout future entries");
    const auto bytes = entries.document()->documentBytes();
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto rootMinor = text.find("\"minor\": 30");
    const auto layoutStart = text.find("\"nodeLayout\"");
    if (rootMinor == std::string::npos || layoutStart == std::string::npos)
        throw std::logic_error("layout future anchors");
    text.replace(rootMinor, std::string_view("\"minor\": 30").size(), "\"minor\": 31");
    const auto y = text.find("\"y\": 32.0", layoutStart);
    if (y == std::string::npos)
        throw std::logic_error("layout position anchor");
    text.insert(y + std::string_view("\"y\": 32.0").size(), R"(, "zzzPosition":42)");
    const auto muted = text.find("\"muted\": false", layoutStart);
    if (muted == std::string::npos)
        throw std::logic_error("layout record anchor");
    text.insert(muted + std::string_view("\"muted\": false").size(), R"(, "zzzLayout":"retained")");
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 31}};
    const auto size = canonicalManifestSize(manifest);
    if (!size)
        throw std::logic_error("future manifest size");
    std::vector<char> encoded(*size.value());
    if (!encodeCanonicalManifest(manifest, encoded))
        throw std::logic_error("future manifest encode");
    const auto archive = test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes({encoded.data(), encoded.size()})),
        test::makeStoredEntry("document.json", test::toBytes(text)));
    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened,
           "future layout and position members open editable");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.roundTrip && opened.schemaMinor == 31,
           "future layout retains its schema and attachments");
    if (!opened.roundTrip)
        throw std::logic_error("layout round-trip state");
    snapshot = opened.document->snapshot();
    auto rewritten =
        buildVerifiedSaveArchive(manifest,
                                 CanonicalDocumentV1{.snapshot = &snapshot,
                                                     .colorSettings = &opened.colorSettings,
                                                     .roundTrip = &*opened.roundTrip,
                                                     .schemaMinor = 31},
                                 {}, memory());
    expect(static_cast<bool>(rewritten), "future layout passes verified overlay save");
    if (!rewritten)
        return;
    auto rewrittenEntries = readZipContainer(rewritten.archive()->bytes(), {}, memory());
    if (!rewrittenEntries)
        throw std::logic_error("rewritten layout entries");
    const auto rewrittenBytes = rewrittenEntries.document()->documentBytes();
    const std::string_view retained(reinterpret_cast<const char*>(rewrittenBytes.data()),
                                    rewrittenBytes.size());
    expect(retained.find("\"zzzPosition\": 42") != std::string_view::npos &&
               retained.find("\"zzzLayout\": \"retained\"") != std::string_view::npos,
           "unknown position and NodeId-keyed layout members survive reopen and save");
}

} // namespace
int main() {
    try {
        layoutAndReopen();
        discreteImageNodesReopen();
        blurNodeReopen();
        matteNodeReopen();
        futureLayoutAttachments();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
