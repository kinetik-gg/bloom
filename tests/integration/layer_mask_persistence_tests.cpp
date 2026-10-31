// ADR 0028 persistence: layer masks (constant and keyed paths), a track matte and the layerMask
// allocator namespace save, reopen and decode exactly in document 1.24; a 1.23 document migrates
// with none of them, and a 1.23 document carrying a 1.24 member is refused.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <regex>

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

// Document 1.29 (ADR 0033) members a downgraded fixture must not carry.
[[maybe_unused]] void stripExportQueueV1_29(std::string& text) {
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportQueue"\s*:\s*\{\s*"jobs"\s*:\s*\[\]\s*\})"), "");
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportJob"\s*:\s*"0",\s*"exportOutput"\s*:\s*"0")"), "");
}
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
document::PathValue rectangle(const double size, const bool curved) {
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{0.0, 0.0}, std::nullopt, std::nullopt},
                    {{size, 0.0},
                     std::nullopt,
                     curved ? std::optional{document::Vec2d{size, 5.0}} : std::nullopt},
                    {{size, size}, std::nullopt, std::nullopt},
                    {{0.0, size}, std::nullopt, std::nullopt}};
    return path;
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Masks", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Masks", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    Fixture() {
        const auto source =
            *run<commands::AddSolidLayer>(composition, "Matte", core::Color4d{1, 1, 1, 1})
                 .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        const auto target =
            *run<commands::AddSolidLayer>(composition, "Target", core::Color4d{1, 0, 0, 1})
                 .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        const auto first = *run<commands::AddLayerMask>(composition, target, rectangle(32, true))
                                .outputId<document::LayerMaskId>("mask");
        run<commands::AddLayerMask>(composition, target, rectangle(16, false),
                                    document::LayerMaskMode::Subtract);
        run<commands::SetLayerMaskInverted>(composition, target, first, true);
        run<commands::AnimateLayerMaskPath>(composition, target, first, core::RationalTime{});
        run<commands::SetLayerMaskPath>(composition, target, first,
                                        core::RationalTime::fromInteger(1), rectangle(40, true));
        run<commands::SetTrackMatte>(
            composition, target,
            document::TrackMatte{source, document::TrackMatteChannel::Luma, true});
    }
};

std::string encode(const document::Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

project::DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("mask parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceOnce(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
}

void roundTrip() {
    Fixture fixture;
    const auto snapshot = fixture.document.snapshot();
    expect(snapshot.project().validate().ok(), "the masked project validates");
    const auto text = encode(fixture.document);
    expect(text.find("\"masks\"") != std::string::npos &&
               text.find("\"trackMatte\"") != std::string::npos &&
               text.find("\"kind\": \"path\"") != std::string::npos &&
               text.find("\"layerMask\"") != std::string::npos,
           "masks, the matte, the Path curve and the layerMask namespace are written");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded), "the 1.24 document decodes");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the masked archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the masked archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    expect(std::ranges::equal(before.graph().layerOutputs(), after.graph().layerOutputs()),
           "every layer boundary, its masks and its matte equal themselves after save and open");
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()) &&
            std::ranges::equal(before.parameters().records(), after.parameters().records()),
        "mask parameters and the Path curve equal themselves after save and open");
    expect(reopened.project().validate().ok(), "the reopened project validates");
}

void olderDocumentsMigrateAndRefuseNewMembers() {
    document::Document plain{
        document::makeNewProject(
            "Plain", "Main", core::RationalTime::fromInteger(2),
            *document::CompositionFormat::create(8, 8, core::PixelAspectRatio::square(),
                                                 document::FrameRate::framesPerSecond24()))
            .project};
    auto text = encode(plain);
    replaceOnce(text, "\"minor\": 32", "\"minor\": 23");
    stripExportQueueV1_29(text);
    expect(static_cast<bool>(decodeText(text)), "a 1.23 document without masks decodes");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (parsed) {
        const auto migrated =
            project::migrateDocumentDom(parsed.document()->root(), {1, 23}, {1, 32},
                                        project::kProductionDocumentMigrationSteps, {}, operation);
        expect(
            migrated.outcome() == project::MigrationOutcome::Migrated &&
                migrated.stepsApplied() == 8 &&
                static_cast<bool>(project::decodeDocumentEnvelope(*migrated.migratedRoot())),
            "the 1.23 -> 1.24 mask, 1.24 -> 1.25 video-metadata and 1.25 -> 1.26 channel-set and "
            "1.26 -> 1.27 composition-folder and 1.27 -> 1.28 video-colour-default steps only "
            "advance the version, 1.28 -> 1.29 adds the empty export queue, and 1.29 -> 1.30 easing and 1.30 -> 1.31 "
            "node-group-colour only advances the version");
    }

    Fixture fixture;
    auto masked = encode(fixture.document);
    replaceOnce(masked, "\"minor\": 32", "\"minor\": 23");
    stripExportQueueV1_29(masked);
    expect(!static_cast<bool>(decodeText(masked)), "a 1.23 document carrying masks is refused");
}

// Every handle mode on a path; the explicit modes are the ones the geometry would not imply.
document::PathValue modedPath(const double shift) {
    using document::PathAnchorMode;
    using document::Vec2d;
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{0.0 + shift, 0.0}, std::nullopt, std::nullopt, PathAnchorMode::Corner},
                    {{20.0 + shift, 0.0},
                     Vec2d{15.0 + shift, -5.0},
                     Vec2d{25.0 + shift, 5.0},
                     PathAnchorMode::Mirrored},
                    {{40.0 + shift, 0.0},
                     Vec2d{35.0 + shift, -5.0},
                     Vec2d{50.0 + shift, 10.0},
                     PathAnchorMode::Aligned},
                    {{40.0 + shift, 30.0},
                     Vec2d{45.0 + shift, 25.0},
                     Vec2d{30.0 + shift, 35.0},
                     PathAnchorMode::Free},
                    // Explicit: Free with collinear equal handles, and Auto.
                    {{20.0 + shift, 40.0},
                     Vec2d{25.0 + shift, 40.0},
                     Vec2d{15.0 + shift, 40.0},
                     PathAnchorMode::Free},
                    {{0.0 + shift, 30.0},
                     Vec2d{0.0 + shift, 36.0},
                     Vec2d{0.0 + shift, 24.0},
                     PathAnchorMode::Auto}};
    return path;
}

void anchorModesPersistAndMigrate() {
    Fixture fixture;
    const auto target =
        *fixture
             .run<commands::AddSolidLayer>(fixture.composition, "Moded", core::Color4d{1, 1, 1, 1})
             .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto mask =
        *fixture.run<commands::AddLayerMask>(fixture.composition, target, modedPath(0))
             .outputId<document::LayerMaskId>("mask");
    fixture.run<commands::AnimateLayerMaskPath>(fixture.composition, target, mask,
                                                core::RationalTime{});
    auto keyed = modedPath(5);
    keyed.anchors[1].mode = document::PathAnchorMode::Auto;
    fixture.run<commands::SetLayerMaskPath>(fixture.composition, target, mask,
                                            core::RationalTime::fromInteger(2), keyed);
    const auto text = encode(fixture.document);
    expect(text.find("\"mode\": \"auto\"") != std::string::npos &&
               text.find("\"mode\": \"free\"") != std::string::npos,
           "explicit anchor modes are written");
    expect(text.find("\"mode\": \"mirrored\"") == std::string::npos &&
               text.find("\"mode\": \"aligned\"") == std::string::npos,
           "modes the geometry implies are omitted");

    const auto snapshot = fixture.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the moded archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the moded archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *opened.document->snapshot().project().findComposition(fixture.composition);
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()),
        "every key's anchor modes equal themselves after save and reopen");

    // A file written before the mode existed carries none: each anchor migrates to its inferred
    // mode, and an explicit mode in a pre-1.24 file is refused.
    auto stripped = text;
    for (const auto* name : {"corner", "mirrored", "aligned", "free", "auto"}) {
        const std::string member = std::string("\"mode\": \"") + name + "\"";
        for (auto at = stripped.find(member); at != std::string::npos; at = stripped.find(member)) {
            // Remove the anchor member and the comma that ended the line before it.
            const auto comma = stripped.rfind(',', at);
            stripped.erase(comma, at + member.size() - comma);
        }
    }
    const auto decoded = decodeText(stripped);
    expect(static_cast<bool>(decoded), "a path without anchor modes decodes");
    if (decoded) {
        bool inferred = false;
        const auto& compositions = decoded.value()->compositions;
        const auto composition =
            std::ranges::find(compositions, fixture.composition, &project::DecodedComposition::id);
        if (composition != compositions.end())
            for (const auto& record : composition->animationCurves)
                if (const auto* curve = std::get_if<document::PathAnimationCurve>(&record)) {
                    const auto& anchors = curve->keyframes.front().value.anchors;
                    if (anchors.size() != 6)
                        continue;
                    using document::PathAnchorMode;
                    inferred = anchors[0].mode == PathAnchorMode::Corner &&
                               anchors[1].mode == PathAnchorMode::Mirrored &&
                               anchors[2].mode == PathAnchorMode::Aligned &&
                               anchors[3].mode == PathAnchorMode::Free &&
                               anchors[4].mode == PathAnchorMode::Mirrored &&
                               anchors[5].mode == PathAnchorMode::Mirrored;
                }
        expect(inferred, "omitted modes migrate as Corner, Mirrored, Aligned or Free");
    }

    document::Document shaped{
        document::makeNewProject(
            "Shaped", "Main", core::RationalTime::fromInteger(2),
            *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                                 document::FrameRate::framesPerSecond24()))
            .project};
    commands::CommandStack shapedCommands{shaped};
    commands::Transaction add("Shape", shaped.snapshot().revision());
    commands::ShapeLayerGeometry geometry;
    geometry.path = modedPath(0);
    add.emplace<commands::AddShapeLayer>(shaped.snapshot().project().compositions().front().id(),
                                         document::ShapeKind::Path, geometry);
    expect(shapedCommands.execute(std::move(add)).succeeded(), "a moded shape path is added");
    auto shapeText = encode(shaped);
    replaceOnce(shapeText, "\"minor\": 32", "\"minor\": 23");
    stripExportQueueV1_29(shapeText);
    expect(!static_cast<bool>(decodeText(shapeText)),
           "a pre-1.24 file carrying an anchor mode is refused");
}

// #376: a composition's timecode labelling (document 1.26) is written only when set, saves and
// reopens exactly, and is refused in an older file.
void compositionTimecodePersists() {
    Fixture fixture;
    expect(encode(fixture.document).find("\"timecode\"") == std::string::npos,
           "the default timecode labelling is not written");
    commands::Transaction transaction("Settings", fixture.document.snapshot().revision());
    auto* composition = fixture.document.snapshot().project().findComposition(fixture.composition);
    auto settings = commands::compositionSettingsOf(*composition);
    settings.timecode = {*core::RationalTime::create(3601, 2), true};
    transaction.emplace<commands::SetCompositionSettings>(fixture.composition, settings);
    expect(fixture.commands.execute(std::move(transaction)).succeeded(), "the timecode is set");
    const auto text = encode(fixture.document);
    expect(text.find("\"timecode\"") != std::string::npos &&
               text.find("\"dropFrame\": true") != std::string::npos,
           "the timecode labelling is written");
    const auto snapshot = fixture.document.snapshot();
    const auto colour = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &colour}, {}, memory());
    expect(static_cast<bool>(archive), "the archive with a timecode saves");
    if (archive) {
        auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
        expect(result.outcome() == project::OpenArchiveOutcome::Opened, "and opens");
        if (result.outcome() == project::OpenArchiveOutcome::Opened) {
            auto opened = std::move(result).takeOpened();
            const auto* reopened =
                opened.document->snapshot().project().findComposition(fixture.composition);
            expect(reopened != nullptr && reopened->timecode() == settings.timecode,
                   "the start timecode and drop-frame flag reopen exactly");
        }
    }
    auto older = text;
    replaceOnce(older, "\"minor\": 32", "\"minor\": 25");
    stripExportQueueV1_29(older);
    expect(!static_cast<bool>(decodeText(older)), "a pre-1.26 file carrying a timecode is refused");
}

} // namespace

int main() {
    try {
        roundTrip();
        olderDocumentsMigrateAndRefuseNewMembers();
        anchorModesPersistAndMigrate();
        compositionTimecodePersists();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "layer mask persistence PASS\n";
    return failures == 0 ? 0 : 1;
}
